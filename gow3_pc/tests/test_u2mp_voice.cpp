// u2mp voice tests (gow3_pc/src/u2mp/voice.cpp, core only: no audio device, no game).
// Build: ninja -C gow3_pc/out/gpu u2mp-voice-test
// 1. Opus round trip: a voice-band tone survives encode/decode; silence stays silent; concealment
//    and FEC decode full frames.
// 2. Jitter buffer: reordering, loss (with FEC look-ahead), late and duplicate frames, rebuffering,
//    sequence wrap-around, latency cap.
// 3. Mixer clamp and the voice-activity gate.
#include "../src/u2mp/voice.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace u2mp::voice;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static std::vector<int16_t> tone(int frames, double hz, double amplitude) {
    std::vector<int16_t> s(size_t(frames) * kFrameSamples);
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = int16_t(amplitude * std::sin(2 * 3.14159265358979323846 * hz * double(i) / kSampleRate));
    return s;
}

static double rms(const int16_t *p, size_t n) {
    double e = 0;
    for (size_t i = 0; i < n; ++i) e += double(p[i]) * p[i];
    return n ? std::sqrt(e / double(n)) : 0;
}

static void codec_round_trip() {
    Encoder enc;
    Decoder dec;
    CHECK(enc.ok());
    CHECK(dec.ok());
    const int frames = 100;
    const std::vector<int16_t> in = tone(frames, 440, 8000);
    std::vector<int16_t> out(in.size());
    std::vector<uint8_t> packet;
    size_t bytes = 0;
    for (int f = 0; f < frames; ++f) {
        CHECK(enc.encode(&in[size_t(f) * kFrameSamples], packet));
        CHECK(!packet.empty() && packet.size() <= kMaxEncoded);
        bytes += packet.size();
        CHECK(dec.decode(packet.data(), packet.size(), &out[size_t(f) * kFrameSamples]));
    }
    std::printf("codec: %.1f bytes per frame (%.1f kbit/s)\n", double(bytes) / frames, double(bytes) * 8 * 50 / frames / 1000);
    // Opus delays the signal by a few ms: compare at the best lag, after the first 10 frames.
    const size_t from = 10 * kFrameSamples, n = 60 * kFrameSamples;
    double best = 0;
    for (size_t lag = 0; lag < 400; ++lag) {
        double xy = 0, xx = 0, yy = 0;
        for (size_t i = from; i < from + n; ++i) {
            const double x = in[i - lag], y = out[i];
            xy += x * y; xx += x * x; yy += y * y;
        }
        if (xx > 0 && yy > 0) best = std::max(best, xy / std::sqrt(xx * yy));
    }
    std::printf("codec: correlation %.3f, rms in %.0f out %.0f\n", best, rms(&in[from], n), rms(&out[from], n));
    CHECK(best > 0.9);
    CHECK(std::fabs(rms(&out[from], n) / rms(&in[from], n) - 1.0) < 0.25);

    // concealment and FEC give full frames
    int16_t pcm[kFrameSamples];
    CHECK(dec.decode(nullptr, 0, pcm));
    CHECK(enc.encode(&in[0], packet));
    CHECK(dec.decode(packet.data(), packet.size(), pcm, true));

    // silence in, (near) silence out
    Encoder enc2;
    Decoder dec2;
    const std::vector<int16_t> quiet(size_t(20) * kFrameSamples, 0);
    double loudest = 0;
    for (int f = 0; f < 20; ++f) {
        CHECK(enc2.encode(&quiet[size_t(f) * kFrameSamples], packet));
        CHECK(dec2.decode(packet.data(), packet.size(), pcm));
        loudest = std::max(loudest, rms(pcm, kFrameSamples));
    }
    CHECK(loudest < 50);
}

// One-byte "packets" carrying their sequence's low byte, to see what comes out.
static void push(JitterBuffer &jb, uint16_t seq) {
    const uint8_t b = uint8_t(seq);
    jb.push(seq, &b, 1);
}

static void jitter_ordering() {
    JitterBuffer jb(3);
    std::vector<uint8_t> f, next;
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Idle);   // nothing yet
    push(jb, 102);
    push(jb, 100);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Idle);   // 2 of 3 held: still buffering
    push(jb, 101);
    push(jb, 103);
    push(jb, 101);                                       // duplicate
    for (uint16_t want = 100; want <= 103; ++want) {
        CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame);
        CHECK(f.size() == 1 && f[0] == uint8_t(want));
    }
    CHECK(jb.buffered() == 0);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Idle);   // ran dry: rebuffer
    CHECK(!jb.playing());
    push(jb, 99);                                        // older than anything played, but buffer idle
    // a lone frame starts after waiting the target time
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Idle);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Idle);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame && f[0] == 99);
}

static void jitter_loss() {
    JitterBuffer jb(3);
    std::vector<uint8_t> f, next;
    push(jb, 10);
    push(jb, 11);
    push(jb, 13);   // 12 lost
    push(jb, 15);   // 14 lost, 16 not here yet
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame && f[0] == 10);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame && f[0] == 11);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Lost);   // 12: FEC from 13
    CHECK(next.size() == 1 && next[0] == 13);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame && f[0] == 13);
    push(jb, 12);                                        // too late now
    CHECK(jb.late == 1);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Lost);   // 14: FEC from 15
    CHECK(next.size() == 1 && next[0] == 15);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame && f[0] == 15);
    CHECK(jb.lost == 2);
    // a gap whose following frame has not arrived: plain concealment
    push(jb, 18);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Lost && next.empty());   // 16
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Lost && next.size() == 1 && next[0] == 18);   // 17
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame && f[0] == 18);
}

static void jitter_wrap_and_cap() {
    JitterBuffer jb(3);
    std::vector<uint8_t> f, next;
    for (uint16_t s = 65533; s != 3; ++s) push(jb, s);   // 65533..65535, 0..2
    uint16_t want = 65533;
    for (int i = 0; i < 6; ++i, ++want) {
        CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame);
        CHECK(f.size() == 1 && f[0] == uint8_t(want));
    }
    // a burst of 12 frames: the oldest are dropped to keep at most 2 * target + 2 = 8
    for (uint16_t s = 3; s < 15; ++s) push(jb, s);
    CHECK(jb.pop(f, &next) == JitterBuffer::Out::Frame);
    CHECK(f[0] == 7);   // 3..6 dropped
    CHECK(jb.buffered() == 7);
    // the speaker restarted far ahead: start over
    push(jb, 1000);
    CHECK(jb.buffered() == 1);
}

static void mixer_and_gate() {
    int32_t mix[4] = {};
    const int16_t a[4] = {30000, -30000, 100, 0}, b[4] = {30000, -30000, -100, 0};
    mix_into(mix, a, 4, 1.0f);
    mix_into(mix, b, 4, 0.5f);
    int16_t out[4];
    to_pcm16(mix, out, 4);
    CHECK(out[0] == 32767 && out[1] == -32768 && out[2] == 50 && out[3] == 0);

    Gate gate;
    const std::vector<int16_t> quiet(kFrameSamples, 0), noise = tone(1, 3000, 150), speech = tone(1, 300, 6000);
    for (int i = 0; i < 50; ++i) CHECK(!gate.update(noise.data(), kFrameSamples));   // steady background
    CHECK(gate.update(speech.data(), kFrameSamples));
    int open = 0;
    for (int i = 0; i < 30; ++i) open += gate.update(quiet.data(), kFrameSamples) ? 1 : 0;
    CHECK(open == 14);   // held 300 ms (15 frames, counting the speech frame)
}

int main() {
    codec_round_trip();
    jitter_ordering();
    jitter_loss();
    jitter_wrap_and_cap();
    mixer_and_gate();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("u2mp voice: all tests passed\n");
    return 0;
}
