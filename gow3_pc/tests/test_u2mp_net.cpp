// u2mp network core tests (gow3_pc/src/u2mp/net.cpp). Build: ninja -C gow3_pc/out/gpu u2mp-net-test
// 1. Two connections over a lossy link: every reliable message arrives once, in order.
// 2. A host and a client over real UDP on 127.0.0.1: join, messages both ways, leave.
#include "../src/u2mp/net.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace u2mp::net;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static void lossy_link() {
    Connection a, b;
    std::vector<int> got_b, got_a;
    int unreliable_b = 0;
    const int total = 300;
    int sent = 0;
    std::vector<uint8_t> packet;
    for (int tick = 0; tick < 4000 && (got_b.size() < size_t(total) || got_a.size() < size_t(total)); ++tick) {
        if (sent < total) {
            const uint8_t m[4] = {uint8_t(sent), uint8_t(sent >> 8), 0xab, 0xcd};
            a.queue(true, m, sizeof m);
            b.queue(true, m, sizeof m);
            const uint8_t u[2] = {0x55, 0x66};
            a.queue(false, u, sizeof u);
            ++sent;
        }
        if (a.build(packet, true) && tick % 3 != 0)   // a -> b, every third packet lost
            b.receive(packet.data(), packet.size(), [&](const uint8_t *d, size_t n) {
                if (n == 4) got_b.push_back(d[0] | d[1] << 8);
                else ++unreliable_b;
            });
        if (b.build(packet, true) && tick % 5 != 1)   // b -> a, every fifth lost
            a.receive(packet.data(), packet.size(), [&](const uint8_t *d, size_t n) {
                if (n == 4) got_a.push_back(d[0] | d[1] << 8);
            });
    }
    CHECK(got_b.size() == size_t(total));
    CHECK(got_a.size() == size_t(total));
    for (size_t i = 0; i < got_b.size(); ++i) CHECK(got_b[i] == int(i));
    for (size_t i = 0; i < got_a.size(); ++i) CHECK(got_a[i] == int(i));
    CHECK(unreliable_b > 0 && unreliable_b < total);   // some unreliable ones lost, none resent
    CHECK(a.pending_reliable() == 0);
    std::printf("lossy link: %zu/%zu reliable in order, %d unreliable\n", got_b.size(), got_a.size(), unreliable_b);
}

static void loopback() {
    Transport host, client;
    CHECK(host.host(0));
    Address to;
    CHECK(Address::parse("127.0.0.1:" + std::to_string(host.bound_port()), 0, to));
    CHECK(client.join(to));
    int joined = 0, left = 0, host_got = 0, client_got = 0;
    host.on_peer = [&](uint8_t slot, bool in) { (in ? joined : left) += slot == 1; };
    host.on_message = [&](uint8_t slot, const uint8_t *d, size_t n) { host_got += slot == 1 && n == 3 && d[0] == 'c'; };
    client.on_message = [&](uint8_t slot, const uint8_t *d, size_t n) { client_got += slot == 0 && n == 3 && d[0] == 'h'; };
    double now = 0;
    for (int i = 0; i < 200 && !client.connected(); ++i) {
        client.pump(now);
        host.pump(now);
        now += 0.016;
    }
    CHECK(client.connected());
    CHECK(client.local_slot() == 1);
    CHECK(joined == 1);
    for (int i = 0; i < 20; ++i) {
        client.send(0, true, reinterpret_cast<const uint8_t *>("cli"), 3);
        host.broadcast(true, reinterpret_cast<const uint8_t *>("hst"), 3);
    }
    for (int i = 0; i < 200 && (host_got < 20 || client_got < 20); ++i) {
        client.pump(now);
        host.pump(now);
        now += 0.016;
    }
    CHECK(host_got == 20);
    CHECK(client_got == 20);
    client.close();
    for (int i = 0; i < 20 && !left; ++i) {
        host.pump(now);
        now += 0.016;
    }
    CHECK(left == 1);
    std::printf("loopback: joined %d, host got %d, client got %d, left %d\n", joined, host_got, client_got, left);
}

int main() {
    lossy_link();
    loopback();
    std::printf(failures ? "u2mp net: %d FAILED\n" : "u2mp net: all passed\n", failures);
    return failures ? 1 : 0;
}
