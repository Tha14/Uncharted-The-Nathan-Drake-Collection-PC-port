// u2mp network core: UDP transport between PCs, independent of the game.
//
// The PS3 game's transport (net-connection.cpp: UDP, sequence numbers, an ack window, a message
// layer packing 1200-byte packets) is absent from the PS4 build. This is its PC replacement. The
// game-facing side (which events go where) is in a separate file; this one only moves messages.
//
// Wire format (little-endian), one UDP datagram = one packet, at most kMaxPacket bytes:
//   u32 magic 'U2MP'   u8 version   u8 kind (0 data, 1 hello, 2 welcome, 3 bye, 4 ping)
//   u16 sequence       u16 ack      u32 ack_bits   (bit i: sequence ack-1-i was received)
//   u8 flags (bit 0: the ack fields are valid; clear until the sender has received a packet)
//   messages: { u8 channel (0 unreliable, 1 reliable), u16 reliable id (channel 1 only),
//               u16 length, payload }...
// Reliable messages are resent in every packet until a packet carrying them is acknowledged,
// and delivered in order (by reliable id). Unreliable ones are sent once.
#pragma once
#include <utility>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace u2mp::net {

constexpr size_t kMaxPacket = 1400;           // the game's 1200-byte packets plus our headers, under the Ethernet MTU
constexpr size_t kMaxPeers = 10;              // Uncharted 2 multiplayer: 10 players
constexpr uint32_t kMagic = 0x504d3255;       // "U2MP"
constexpr uint8_t kVersion = 1;
constexpr double kTimeoutSeconds = 10.0;

struct Address {
    uint32_t ip = 0;      // network byte order
    uint16_t port = 0;    // host byte order
    bool operator==(const Address &o) const { return ip == o.ip && port == o.port; }
    std::string text() const;
    static bool parse(const std::string &text, uint16_t default_port, Address &out);
};

// One remote peer: sequence/ack state and the reliable queue.
class Connection {
public:
    Address address;
    uint8_t slot = 0;               // 1..kMaxPeers (0 = unused)
    double last_heard = 0;
    double last_sent = 0;
    bool connected = false;
    void reset() { *this = Connection(); }

    void queue(bool reliable, const uint8_t *data, size_t size);
    // Builds the next packet (kind 0) into `out`; false when there is nothing to send.
    bool build(std::vector<uint8_t> &out, bool keepalive);
    // Takes a received data packet; delivers its messages, in order for reliable ones.
    void receive(const uint8_t *packet, size_t size, const std::function<void(const uint8_t *, size_t)> &deliver);
    size_t pending_reliable() const { return reliable_.size(); }

private:
    struct Pending { uint16_t id; std::vector<uint8_t> data; std::vector<uint16_t> sent_in; };
    uint16_t next_sequence_ = 0;
    uint16_t remote_sequence_ = 0;
    uint32_t remote_bits_ = 0;
    bool any_received_ = false;
    uint16_t next_reliable_ = 0;
    uint16_t expected_reliable_ = 0;
    std::deque<Pending> reliable_;
    std::vector<std::vector<uint8_t>> unreliable_;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> early_;   // reliable messages ahead of order
    void acknowledge(uint16_t ack, uint32_t bits);
};

// A datagram path other than the transport's own UDP socket (online play: online.cpp sends through the
// online link's peer-to-peer socket). The link names its peers with Addresses of its own choosing.
struct Link {
    virtual ~Link() = default;
    virtual void send(const Address &to, const uint8_t *data, size_t size) = 0;
    // One received datagram: its length, 0 when none is waiting.
    virtual int receive(Address &from, uint8_t *data, size_t capacity) = 0;
};

// The local endpoint: a UDP socket (or a Link) and up to kMaxPeers connections. Host or client.
class Transport {
public:
    using Deliver = std::function<void(uint8_t slot, const uint8_t *data, size_t size)>;
    using PeerEvent = std::function<void(uint8_t slot, bool joined)>;

    ~Transport();
    bool host(uint16_t port);                                  // listen; we are slot 0
    bool join(const Address &host_address, uint16_t local_port = 0);
    // The same over a Link instead of a socket of our own (the link stays the caller's).
    bool host_link(Link *link);
    bool join_link(Link *link, const Address &host_address);
    void close();
    // Once per frame: receive everything, time out silent peers, send.
    void pump(double now);
    void send(uint8_t slot, bool reliable, const uint8_t *data, size_t size);   // slot 0 = host
    void broadcast(bool reliable, const uint8_t *data, size_t size);
    bool is_host() const { return hosting_; }
    uint8_t local_slot() const { return local_slot_; }
    bool connected() const;
    // The address of the peer in `slot` (slot 0 = the host when we are a client); false when nobody is there.
    bool peer_address(uint8_t slot, Address &out) const;
    Deliver on_message;
    PeerEvent on_peer;
    uint16_t bound_port() const { return port_; }

private:
    uintptr_t socket_ = ~uintptr_t(0);
    Link *link_ = nullptr;
    uint16_t port_ = 0;
    bool hosting_ = false;
    uint8_t local_slot_ = 0;
    Connection peers_[kMaxPeers + 1];   // index = slot (0 = host when we are a client)
    Address host_address_;
    double hello_sent_ = -1;
    bool open(uint16_t port);
    bool is_open() const { return socket_ != ~uintptr_t(0) || link_; }
    void send_raw(const Address &to, const std::vector<uint8_t> &packet);
    void send_control(const Address &to, uint8_t kind, uint8_t value);
    Connection *find(const Address &from);
    void handle(const Address &from, const uint8_t *packet, size_t size, double now);
};

}  // namespace u2mp::net
