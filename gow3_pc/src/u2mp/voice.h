// u2mp voice chat: party and match voice between PCs.
//
// The PS3 game talked through cellVoice; the PS4 build imports no voice library, so this is a PC
// implementation, separate from the game: SDL3 recording/playback streams of their own (not the
// game's audio ports), Opus (libopus, 16 kHz mono, 20 ms frames) and the session's transport.
//
// Topology: star, like the rest of the session layer. Clients send their voice to the host (slot 0);
// the host plays it and forwards it to every other client; the host's own voice goes to everyone.
// Wire format (one unreliable transport message):
//   u8 gamenet::kVoice   u8 speaker net id   u16 sequence (LE)   Opus packet
// The host overwrites the speaker id of what a client sends with that client's net id.
//
// Threads: capture (SDL recording callback: gate, encode, queue) and playback (SDL playback
// callback: jitter buffers, decode, mix) run on SDL's audio threads; frame() and receive() run on
// the game thread inside the session tick and are the only ones that touch the transport.
//
// Environment:
//   UNCHARTED_U2MP_VOICE          off | open (default: voice-activity gate) | ptt (push to talk)
//   UNCHARTED_U2MP_VOICE_PTT_KEY  SDL key name for push to talk (default "T")
//   UNCHARTED_U2MP_VOICE_VOLUME   playback gain, 0..4 (default 1)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace u2mp::net { class Transport; }

namespace u2mp::voice {

constexpr int kSampleRate = 16000;
constexpr int kFrameSamples = kSampleRate / 50;   // 20 ms
constexpr size_t kMaxEncoded = 400;               // one Opus frame; ~60 bytes at 24 kbit/s
constexpr int kJitterFrames = 3;                  // 60 ms of playout delay
constexpr uint8_t kMaxSpeakers = 12;              // net ids 1..11 (host 1, slot s -> s + 1)

// ---- codec (Opus) --------------------------------------------------------------------------------
class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;
    bool ok() const { return state_ != nullptr; }
    // One 20 ms frame (kFrameSamples) in; the packet (cleared first) out. False on error.
    bool encode(const int16_t *pcm, std::vector<uint8_t> &packet);
private:
    void *state_ = nullptr;
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    bool ok() const { return state_ != nullptr; }
    // Decodes a packet into kFrameSamples samples. `packet` null: concealment of a lost frame.
    // `fec`: recover the lost frame before `packet` from its in-band redundancy.
    bool decode(const uint8_t *packet, size_t size, int16_t *pcm, bool fec = false);
    void reset();
private:
    void *state_ = nullptr;
};

// ---- jitter buffer (one per speaker) -------------------------------------------------------------
// Orders frames by sequence, holds kJitterFrames before playout starts, drops late and duplicate
// frames, reports gaps as lost (with the following frame, if already here, for Opus FEC), and goes
// idle (rebuffering) when it runs dry.
class JitterBuffer {
public:
    enum class Out { Idle, Frame, Lost };
    static constexpr int kSlots = 16;
    explicit JitterBuffer(int target = kJitterFrames) : target_(target) {}
    void push(uint16_t sequence, const uint8_t *data, size_t size);
    // Once per 20 ms of playback. Frame: `frame` holds the packet. Lost: play concealment;
    // `next` (when non-null and the following packet is here) receives it for FEC.
    Out pop(std::vector<uint8_t> &frame, std::vector<uint8_t> *next = nullptr);
    void clear();
    int buffered() const { return count_; }
    bool playing() const { return playing_; }
    unsigned late = 0, lost = 0, overflow = 0;
private:
    struct Slot { bool used = false; uint16_t sequence = 0; std::vector<uint8_t> data; };
    Slot slots_[kSlots];
    int target_;
    int count_ = 0;
    int waited_ = 0;
    bool playing_ = false;
    uint16_t next_ = 0;
    bool find_earliest(uint16_t &sequence) const;
};

// Adds `pcm` times `gain` into the 32-bit mix; to_pcm16 clamps the mix back.
void mix_into(int32_t *mix, const int16_t *pcm, int samples, float gain);
void to_pcm16(const int32_t *mix, int16_t *pcm, int samples);

// Voice-activity gate on 20 ms frames: an energy threshold over an adaptive noise floor, held
// open 300 ms after speech.
class Gate {
public:
    bool update(const int16_t *pcm, int samples);
private:
    float floor_ = 200.0f;
    int hold_ = 0;
};

// ---- runtime (game thread) -------------------------------------------------------------------------
// The session's transport (null when it goes away).
void set_transport(net::Transport *transport);
// Per frame from the session tick: opens the audio streams once there is someone to talk to,
// sends what the microphone produced, reads the push-to-talk key.
void frame();
// A voice message (after the tag byte) from transport slot `slot`.
void receive(uint8_t slot, const uint8_t *data, size_t size);
// Closes the audio streams and forgets the speakers (the party was left).
void stop();
// False when UNCHARTED_U2MP_VOICE=off.
bool enabled();
// Local mute of one member (by net id); toggle returns the new state.
bool muted(uint32_t net_id);
bool toggle_mute(uint32_t net_id);

}  // namespace u2mp::voice
