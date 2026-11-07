// u2mp network core: see net.h for the wire format.
#include "net.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_type = socklen_t;
#endif

namespace u2mp::net {
namespace {

constexpr size_t kHeader = 15;
enum Kind : uint8_t { kData = 0, kHello = 1, kWelcome = 2, kBye = 3 };
constexpr double kKeepalive = 0.1;
constexpr double kHelloRetry = 0.5;

bool newer(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)) > 0; }

void put16(std::vector<uint8_t> &v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// has_ack 0: the sender has received nothing yet, its ack fields mean nothing.
void header(std::vector<uint8_t> &v, uint8_t kind, uint16_t sequence, uint16_t ack, uint32_t bits,
            uint8_t has_ack = 0) {
    put32(v, kMagic);
    v.push_back(kVersion);
    v.push_back(kind);
    put16(v, sequence);
    put16(v, ack);
    put32(v, bits);
    v.push_back(has_ack);
}

bool winsock_ready() {
#ifdef _WIN32
    static int state = 0;   // 0 untried, 1 ok, -1 failed
    if (!state) {
        WSADATA data;
        state = WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 1 : -1;
    }
    return state == 1;
#else
    return true;
#endif
}

}  // namespace

std::string Address::text() const {
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&ip);
    char s[32];
    std::snprintf(s, sizeof s, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], port);
    return s;
}

bool Address::parse(const std::string &text, uint16_t default_port, Address &out) {
    std::string host = text;
    unsigned port = default_port;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host = text.substr(0, colon);
        char *end = nullptr;
        port = unsigned(std::strtoul(text.c_str() + colon + 1, &end, 10));
        if (!end || *end || !port || port > 65535) return false;
    }
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) != 1) return false;
    std::memcpy(&out.ip, &a, 4);
    out.port = uint16_t(port);
    return true;
}

// ---- Connection --------------------------------------------------------------------------

void Connection::queue(bool reliable, const uint8_t *data, size_t size) {
    if (reliable)
        reliable_.push_back(Pending{next_reliable_++, std::vector<uint8_t>(data, data + size), {}});
    else
        unreliable_.emplace_back(data, data + size);
}

bool Connection::build(std::vector<uint8_t> &out, bool keepalive) {
    if (reliable_.empty() && unreliable_.empty() && !keepalive) return false;
    out.clear();
    const uint16_t sequence = next_sequence_++;
    header(out, kData, sequence, remote_sequence_, remote_bits_, any_received_ ? 1 : 0);
    for (Pending &p : reliable_) {
        if (out.size() + 5 + p.data.size() > kMaxPacket) break;
        out.push_back(1);
        put16(out, p.id);
        put16(out, uint16_t(p.data.size()));
        out.insert(out.end(), p.data.begin(), p.data.end());
        p.sent_in.push_back(sequence);
        if (p.sent_in.size() > 64) p.sent_in.erase(p.sent_in.begin());
    }
    size_t taken = 0;
    for (const auto &m : unreliable_) {
        if (out.size() + 3 + m.size() > kMaxPacket) break;
        out.push_back(0);
        put16(out, uint16_t(m.size()));
        out.insert(out.end(), m.begin(), m.end());
        ++taken;
    }
    unreliable_.erase(unreliable_.begin(), unreliable_.begin() + ptrdiff_t(taken));
    return true;
}

void Connection::acknowledge(uint16_t ack, uint32_t bits) {
    auto acked = [&](uint16_t s) {
        if (s == ack) return true;
        const uint16_t back = uint16_t(ack - s);
        return back >= 1 && back <= 32 && (bits >> (back - 1) & 1);
    };
    reliable_.erase(std::remove_if(reliable_.begin(), reliable_.end(), [&](const Pending &p) {
                        return std::any_of(p.sent_in.begin(), p.sent_in.end(), acked);
                    }),
                    reliable_.end());
}

void Connection::receive(const uint8_t *packet, size_t size,
                         const std::function<void(const uint8_t *, size_t)> &deliver) {
    if (size < kHeader) return;
    const uint16_t sequence = get16(packet + 6);
    if (!any_received_) {
        any_received_ = true;
        remote_sequence_ = sequence;
        remote_bits_ = 0;
    } else if (newer(sequence, remote_sequence_)) {
        const uint16_t shift = uint16_t(sequence - remote_sequence_);
        remote_bits_ = shift >= 32 ? 0 : (remote_bits_ << shift) | (shift ? 1u << (shift - 1) : 0);
        remote_sequence_ = sequence;
    } else {
        const uint16_t back = uint16_t(remote_sequence_ - sequence);
        if (back == 0) return;                                        // duplicate
        if (back <= 32) {
            if (remote_bits_ >> (back - 1) & 1) return;               // duplicate
            remote_bits_ |= 1u << (back - 1);
        }
    }
    if (packet[14] & 1) acknowledge(get16(packet + 8), get32(packet + 10));
    size_t at = kHeader;
    while (at < size) {
        const uint8_t channel = packet[at++];
        uint16_t id = 0;
        if (channel == 1) {
            if (at + 2 > size) return;
            id = get16(packet + at);
            at += 2;
        } else if (channel != 0) {
            return;
        }
        if (at + 2 > size) return;
        const uint16_t length = get16(packet + at);
        at += 2;
        if (at + length > size) return;
        const uint8_t *body = packet + at;
        at += length;
        if (channel == 0) {
            deliver(body, length);
            continue;
        }
        if (id == expected_reliable_) {
            deliver(body, length);
            ++expected_reliable_;
            for (bool more = true; more;) {   // messages that arrived early, now in order
                more = false;
                for (auto it = early_.begin(); it != early_.end(); ++it) {
                    if (it->first == expected_reliable_) {
                        deliver(it->second.data(), it->second.size());
                        ++expected_reliable_;
                        early_.erase(it);
                        more = true;
                        break;
                    }
                }
            }
        } else if (newer(id, expected_reliable_) &&
                   std::none_of(early_.begin(), early_.end(), [&](const auto &e) { return e.first == id; })) {
            early_.emplace_back(id, std::vector<uint8_t>(body, body + length));
        }
    }
}

// ---- Transport ---------------------------------------------------------------------------

Transport::~Transport() { close(); }

bool Transport::open(uint16_t port) {
    close();
    if (!winsock_ready()) return false;
    const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return false;
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    if (s < 0) return false;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
        return false;
    }
    socklen_type length = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr *>(&a), &length);
    port_ = ntohs(a.sin_port);
    socket_ = uintptr_t(s);
    return true;
}

void Transport::close() {
    if (socket_ == ~uintptr_t(0)) return;
    for (Connection &c : peers_)
        if (c.connected) send_control(c.address, kBye, local_slot_);
#ifdef _WIN32
    closesocket(SOCKET(socket_));
#else
    ::close(int(socket_));
#endif
    socket_ = ~uintptr_t(0);
    for (Connection &c : peers_) c.reset();
    hosting_ = false;
    local_slot_ = 0;
    hello_sent_ = -1;
}

bool Transport::host(uint16_t port) {
    if (!open(port)) return false;
    hosting_ = true;
    local_slot_ = 0;
    return true;
}

bool Transport::join(const Address &host_address, uint16_t local_port) {
    if (!open(local_port)) return false;
    hosting_ = false;
    host_address_ = host_address;
    hello_sent_ = -1;
    return true;
}

bool Transport::connected() const {
    if (hosting_) return socket_ != ~uintptr_t(0);
    return peers_[0].connected;
}

void Transport::send_raw(const Address &to, const std::vector<uint8_t> &packet) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(to.port);
    std::memcpy(&a.sin_addr, &to.ip, 4);
    ::sendto(decltype(::socket(0, 0, 0))(socket_), reinterpret_cast<const char *>(packet.data()), int(packet.size()), 0,
             reinterpret_cast<sockaddr *>(&a), sizeof a);
}

void Transport::send_control(const Address &to, uint8_t kind, uint8_t value) {
    std::vector<uint8_t> p;
    header(p, kind, 0, 0, 0);
    p.push_back(value);
    send_raw(to, p);
}

Connection *Transport::find(const Address &from) {
    for (Connection &c : peers_)
        if (c.connected && c.address == from) return &c;
    return nullptr;
}

void Transport::handle(const Address &from, const uint8_t *packet, size_t size, double now) {
    if (size < kHeader || get32(packet) != kMagic || packet[4] != kVersion) return;
    const uint8_t kind = packet[5];
    if (kind == kHello && hosting_) {
        Connection *c = find(from);
        if (!c) {
            for (uint8_t slot = 1; slot <= kMaxPeers && !c; ++slot)
                if (!peers_[slot].connected) c = &peers_[slot];
            if (!c) return;   // full
            c->reset();
            c->address = from;
            c->slot = uint8_t(c - peers_);
            c->connected = true;
            c->last_heard = now;
            if (on_peer) on_peer(c->slot, true);
        }
        send_control(from, kWelcome, c->slot);
        return;
    }
    if (kind == kWelcome && !hosting_ && from == host_address_ && size > kHeader) {
        if (!peers_[0].connected) {
            peers_[0].reset();
            peers_[0].address = from;
            peers_[0].connected = true;
            peers_[0].last_heard = now;
            local_slot_ = packet[kHeader];
            if (on_peer) on_peer(0, true);
        }
        return;
    }
    Connection *c = find(from);
    if (!c) return;
    c->last_heard = now;
    if (kind == kBye) {
        const uint8_t slot = c->slot;
        c->reset();
        if (on_peer) on_peer(slot, false);
        return;
    }
    if (kind != kData) return;
    const uint8_t slot = c->slot;
    c->receive(packet, size, [&](const uint8_t *data, size_t length) {
        if (on_message) on_message(slot, data, length);
    });
}

void Transport::pump(double now) {
    if (socket_ == ~uintptr_t(0)) return;
    uint8_t buffer[2048];
    for (;;) {
        sockaddr_in a{};
        socklen_type length = sizeof a;
        const auto n = ::recvfrom(decltype(::socket(0, 0, 0))(socket_), reinterpret_cast<char *>(buffer), int(sizeof buffer), 0,
                                  reinterpret_cast<sockaddr *>(&a), &length);
        if (n <= 0) break;
        Address from;
        std::memcpy(&from.ip, &a.sin_addr, 4);
        from.port = ntohs(a.sin_port);
        handle(from, buffer, size_t(n), now);
    }
    if (!hosting_ && !peers_[0].connected && (hello_sent_ < 0 || now - hello_sent_ >= kHelloRetry)) {
        send_control(host_address_, kHello, 0);
        hello_sent_ = now;
    }
    std::vector<uint8_t> packet;
    for (Connection &c : peers_) {
        if (!c.connected) continue;
        if (now - c.last_heard > kTimeoutSeconds) {
            const uint8_t slot = c.slot;
            c.reset();
            if (on_peer) on_peer(slot, false);
            continue;
        }
        if (c.build(packet, now - c.last_sent >= kKeepalive)) {
            send_raw(c.address, packet);
            c.last_sent = now;
        }
    }
}

void Transport::send(uint8_t slot, bool reliable, const uint8_t *data, size_t size) {
    if (slot > kMaxPeers || !peers_[slot].connected) return;
    peers_[slot].queue(reliable, data, size);
}

void Transport::broadcast(bool reliable, const uint8_t *data, size_t size) {
    for (Connection &c : peers_)
        if (c.connected) c.queue(reliable, data, size);
}

}  // namespace u2mp::net
