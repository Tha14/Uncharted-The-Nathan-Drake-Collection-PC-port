// u2mp voice chat (voice.h). The codec, jitter buffer, mixer and gate come first and need nothing
// but libopus (the host test builds them alone with U2MP_VOICE_CORE_ONLY); the SDL3 streams and the
// transport side follow.
#include <utility>
#include <vector>
#include "voice.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opus/opus.h>

namespace u2mp::voice {

// ---- codec --------------------------------------------------------------------------------------
Encoder::Encoder() {
    int error = 0;
    OpusEncoder *e = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !e) return;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(e, OPUS_SET_INBAND_FEC(1));         // a lost frame can be rebuilt from the next
    opus_encoder_ctl(e, OPUS_SET_PACKET_LOSS_PERC(10));
    state_ = e;
}

Encoder::~Encoder() {
    if (state_) opus_encoder_destroy(static_cast<OpusEncoder *>(state_));
}

bool Encoder::encode(const int16_t *pcm, std::vector<uint8_t> &packet) {
    packet.clear();
    if (!state_) return false;
    packet.resize(kMaxEncoded);
    const int n = opus_encode(static_cast<OpusEncoder *>(state_), pcm, kFrameSamples, packet.data(), opus_int32(kMaxEncoded));
    if (n <= 0) {
        packet.clear();
        return false;
    }
    packet.resize(size_t(n));
    return true;
}

Decoder::Decoder() {
    int error = 0;
    OpusDecoder *d = opus_decoder_create(kSampleRate, 1, &error);
    if (error == OPUS_OK) state_ = d;
}

Decoder::~Decoder() {
    if (state_) opus_decoder_destroy(static_cast<OpusDecoder *>(state_));
}

bool Decoder::decode(const uint8_t *packet, size_t size, int16_t *pcm, bool fec) {
    if (!state_) return false;
    if (!packet) size = 0;
    const int n = opus_decode(static_cast<OpusDecoder *>(state_), packet, opus_int32(size), pcm, kFrameSamples, fec ? 1 : 0);
    if (n != kFrameSamples) {
        std::memset(pcm, 0, sizeof(int16_t) * kFrameSamples);
        return false;
    }
    return true;
}

void Decoder::reset() {
    if (state_) opus_decoder_ctl(static_cast<OpusDecoder *>(state_), OPUS_RESET_STATE);
}

// ---- jitter buffer --------------------------------------------------------------------------------
static int16_t distance(uint16_t from, uint16_t to) { return int16_t(uint16_t(to - from)); }

void JitterBuffer::clear() {
    for (Slot &s : slots_) s.used = false;
    count_ = 0;
    waited_ = 0;
    playing_ = false;
}

bool JitterBuffer::find_earliest(uint16_t &sequence) const {
    bool any = false;
    for (const Slot &s : slots_)
        if (s.used && (!any || distance(sequence, s.sequence) < 0)) {
            sequence = s.sequence;
            any = true;
        }
    return any;
}

void JitterBuffer::push(uint16_t sequence, const uint8_t *data, size_t size) {
    if (!data || !size || size > kMaxEncoded) return;
    if (playing_) {
        const int16_t ahead = distance(next_, sequence);
        if (ahead < 0) {   // already played (or skipped as lost)
            ++late;
            return;
        }
        if (ahead >= kSlots) {   // far ahead: the speaker restarted, or we fell far behind
            ++overflow;
            clear();
        }
    } else {
        for (const Slot &s : slots_)   // everything held must fit one window
            if (s.used && std::abs(int(distance(s.sequence, sequence))) >= kSlots) {
                ++overflow;
                clear();
                break;
            }
    }
    Slot &s = slots_[sequence % kSlots];
    if (s.used) {
        if (s.sequence == sequence) return;   // duplicate
        --count_;
        ++overflow;
    }
    s.used = true;
    s.sequence = sequence;
    s.data.assign(data, data + size);
    ++count_;
}

JitterBuffer::Out JitterBuffer::pop(std::vector<uint8_t> &frame, std::vector<uint8_t> *next) {
    if (!playing_) {
        if (!count_) {
            waited_ = 0;
            return Out::Idle;
        }
        // start once the target depth is held, or the first frame waited that long
        if (count_ < target_ && ++waited_ < target_) return Out::Idle;
        playing_ = true;
        waited_ = 0;
        find_earliest(next_);
    }
    // more than twice the target held (the sender's clock runs fast, or a burst): drop the oldest
    while (count_ > 2 * target_ + 2) {
        Slot &old = slots_[next_ % kSlots];
        if (old.used && old.sequence == next_) {
            old.used = false;
            --count_;
            ++overflow;
        }
        ++next_;
    }
    Slot &s = slots_[next_ % kSlots];
    if (s.used && s.sequence == next_) {
        frame.swap(s.data);
        s.used = false;
        --count_;
        ++next_;
        return Out::Frame;
    }
    if (!count_) {   // ran dry: the speaker stopped (or a long outage); rebuffer
        playing_ = false;
        return Out::Idle;
    }
    ++lost;
    ++next_;
    if (next) {
        const Slot &n = slots_[next_ % kSlots];
        if (n.used && n.sequence == next_) *next = n.data;
        else next->clear();
    }
    return Out::Lost;
}

// ---- mixer and gate --------------------------------------------------------------------------------
void mix_into(int32_t *mix, const int16_t *pcm, int samples, float gain) {
    for (int i = 0; i < samples; ++i) mix[i] += int32_t(std::lrintf(float(pcm[i]) * gain));
}

void to_pcm16(const int32_t *mix, int16_t *pcm, int samples) {
    for (int i = 0; i < samples; ++i) pcm[i] = int16_t(std::clamp<int32_t>(mix[i], -32768, 32767));
}

bool Gate::update(const int16_t *pcm, int samples) {
    double energy = 0;
    for (int i = 0; i < samples; ++i) energy += double(pcm[i]) * pcm[i];
    const float rms = samples ? float(std::sqrt(energy / samples)) : 0.0f;
    const bool speech = rms > std::max(floor_ * 3.0f, 300.0f);
    if (!speech) floor_ += (rms - floor_) * (rms < floor_ ? 0.2f : 0.02f);   // fall fast, rise slowly
    floor_ = std::clamp(floor_, 30.0f, 3000.0f);
    if (speech) hold_ = 15;   // 300 ms
    else if (hold_ > 0) --hold_;
    return speech || hold_ > 0;
}

}  // namespace u2mp::voice

#ifndef U2MP_VOICE_CORE_ONLY
// ---- runtime -----------------------------------------------------------------------------------------
#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "gamenet.h"
#include "net.h"
#include "session_api.h"
#include "u2mp_internal.h"

namespace u2mp::voice {
namespace {

enum class Mode { Off, Open, PushToTalk };
struct Config {
    bool loaded = false;
    Mode mode = Mode::Open;
    SDL_Scancode ptt = SDL_SCANCODE_T;
    float volume = 1.0f;
} config;

void load_config() {
    if (config.loaded) return;
    config.loaded = true;
    if (const char *m = std::getenv("UNCHARTED_U2MP_VOICE"); m && *m) {
        const std::string v(m);
        if (v == "off" || v == "0") config.mode = Mode::Off;
        else if (v == "ptt") config.mode = Mode::PushToTalk;
        else if (v != "open") log("voice: UNCHARTED_U2MP_VOICE=%s not understood (off|open|ptt), using open", m);
    }
    if (const char *k = std::getenv("UNCHARTED_U2MP_VOICE_PTT_KEY"); k && *k) {
        const SDL_Scancode code = SDL_GetScancodeFromName(k);
        if (code != SDL_SCANCODE_UNKNOWN) config.ptt = code;
        else log("voice: push-to-talk key \"%s\" unknown, using T", k);
    }
    if (const char *v = std::getenv("UNCHARTED_U2MP_VOICE_VOLUME"); v && *v)
        config.volume = std::clamp(float(std::atof(v)), 0.0f, 4.0f);
    static const char *const names[] = {"off", "open (voice-activity gate)", "push to talk"};
    log("voice: %s, volume %.2f", names[int(config.mode)], double(config.volume));
}

net::Transport *transport = nullptr;
SDL_AudioStream *capture = nullptr, *playback = nullptr;
bool audio_open = false, audio_failed = false, sdl_audio = false;
uint16_t send_sequence = 0;
bool mutes[kMaxSpeakers] = {};
bool present[kMaxSpeakers] = {};   // members seen last frame (to forget the ones who left)
// When each net id was last heard (ms of the steady clock; the local id: last frame sent). PS3 read
// cellVoice's energy level per port (attribute 1001) for the roster rows' talking icon.
std::atomic<int64_t> heard_ms[kMaxSpeakers];
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// capture thread -> game thread
std::atomic<bool> ptt_down{false};
std::mutex out_mutex;
std::deque<std::vector<uint8_t>> outgoing;
std::unique_ptr<Encoder> encoder;   // capture thread only (while the stream exists)
Gate gate;

// game thread -> playback thread
struct Speaker {
    JitterBuffer jitter;
    Decoder decoder;
};
std::mutex play_mutex;
std::unique_ptr<Speaker> speakers[kMaxSpeakers];

void SDLCALL on_capture(void *, SDL_AudioStream *stream, int, int) {
    int16_t pcm[kFrameSamples];
    std::vector<uint8_t> packet;
    while (SDL_GetAudioStreamAvailable(stream) >= int(sizeof pcm)) {
        if (SDL_GetAudioStreamData(stream, pcm, sizeof pcm) != int(sizeof pcm)) break;
        const bool voiced = gate.update(pcm, kFrameSamples);   // keeps the noise floor current
        const bool talk = config.mode == Mode::PushToTalk ? ptt_down.load(std::memory_order_relaxed) : voiced;
        if (!talk || !encoder || !encoder->encode(pcm, packet)) continue;
        std::lock_guard<std::mutex> lock(out_mutex);
        if (outgoing.size() < 25) outgoing.push_back(std::move(packet));   // 0.5 s: the game thread stalled
    }
}

void SDLCALL on_playback(void *, SDL_AudioStream *stream, int additional, int) {
    static thread_local std::vector<uint8_t> frame, next;
    while (additional > 0) {
        int32_t mix[kFrameSamples] = {};
        int16_t pcm[kFrameSamples];
        {
            std::lock_guard<std::mutex> lock(play_mutex);
            for (auto &sp : speakers) {
                if (!sp) continue;
                const JitterBuffer::Out out = sp->jitter.pop(frame, &next);
                bool ok = false;
                if (out == JitterBuffer::Out::Frame) ok = sp->decoder.decode(frame.data(), frame.size(), pcm);
                else if (out == JitterBuffer::Out::Lost)
                    ok = next.empty() ? sp->decoder.decode(nullptr, 0, pcm) : sp->decoder.decode(next.data(), next.size(), pcm, true);
                if (ok) mix_into(mix, pcm, kFrameSamples, config.volume);
            }
        }
        to_pcm16(mix, pcm, kFrameSamples);
        SDL_PutAudioStreamData(stream, pcm, sizeof pcm);
        additional -= int(sizeof pcm);
    }
}

const char *device_name(SDL_AudioStream *stream) {
    const char *n = stream ? SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream)) : nullptr;
    return n ? n : "?";
}

void open_audio() {
    audio_open = true;
    sdl_audio = SDL_InitSubSystem(SDL_INIT_AUDIO);   // reference counted: the game's audio may have it
    if (!sdl_audio) {
        log("voice: SDL audio unavailable (%s): no voice chat", SDL_GetError());
        audio_failed = true;
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kSampleRate};
    playback = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_playback, nullptr);
    if (playback) {
        SDL_ResumeAudioStreamDevice(playback);
        log("voice: playing on %s", device_name(playback));
    } else {
        log("voice: no playback stream (%s)", SDL_GetError());
    }
    encoder = std::make_unique<Encoder>();
    if (!encoder->ok()) {
        log("voice: Opus encoder unavailable: listening only");
    } else if ((capture = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, on_capture, nullptr))) {
        SDL_ResumeAudioStreamDevice(capture);
        log("voice: recording from %s", device_name(capture));
    } else {
        log("voice: no microphone (%s): listening only", SDL_GetError());
    }
    if (!playback && !capture) audio_failed = true;
}

// A member left (or the slot changed hands): forget its stream and mute.
void forget(uint8_t id) {
    mutes[id] = false;
    std::lock_guard<std::mutex> lock(play_mutex);
    speakers[id].reset();
}

}  // namespace

bool enabled() {
    load_config();
    return config.mode != Mode::Off;
}

void set_transport(net::Transport *t) {
    if (t != transport) stop();
    transport = t;
}

void stop() {
    if (capture) SDL_DestroyAudioStream(capture);   // no callback runs after this returns
    if (playback) SDL_DestroyAudioStream(playback);
    capture = playback = nullptr;
    encoder.reset();
    if (sdl_audio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (audio_open) log("voice: closed");
    sdl_audio = audio_open = audio_failed = false;
    {
        std::lock_guard<std::mutex> lock(out_mutex);
        outgoing.clear();
    }
    for (uint8_t id = 0; id < kMaxSpeakers; ++id) {
        present[id] = false;
        speakers[id].reset();   // no playback callback is left to race with
        mutes[id] = false;
    }
    gate = Gate{};
}

void frame() {
    if (!transport || !enabled()) return;
    // members: forget the ones who left; is there anyone to talk to?
    bool others = false;
    const uint32_t me = session::local_id();
    for (uint8_t id = 1; id < kMaxSpeakers; ++id) {
        std::string name;
        const bool here = session::member_name(id, name);
        if (present[id] && !here) forget(id);
        present[id] = here;
        others = others || (here && id != me);
    }
    if (others && !audio_open) open_audio();
    if (config.mode == Mode::PushToTalk) {
        const bool *keys = SDL_GetKeyboardState(nullptr);
        ptt_down.store(keys && keys[config.ptt], std::memory_order_relaxed);
    }
    std::deque<std::vector<uint8_t>> batch;
    {
        std::lock_guard<std::mutex> lock(out_mutex);
        batch.swap(outgoing);
    }
    if (!others || !me || me >= kMaxSpeakers) return;
    if (!batch.empty()) heard_ms[me].store(now_ms(), std::memory_order_relaxed);
    std::vector<uint8_t> message;
    for (const auto &packet : batch) {
        message.assign({gamenet::kVoice, uint8_t(me), uint8_t(send_sequence), uint8_t(send_sequence >> 8)});
        message.insert(message.end(), packet.begin(), packet.end());
        ++send_sequence;
        if (transport->is_host()) transport->broadcast(false, message.data(), message.size());
        else transport->send(0, false, message.data(), message.size());
    }
}

void receive(uint8_t slot, const uint8_t *data, size_t size) {
    if (!transport || size < 4 || size - 3 > kMaxEncoded) return;
    uint8_t speaker = data[0];
    if (transport->is_host()) {
        if (slot == 0 || slot > net::kMaxPeers) return;
        speaker = uint8_t(slot + 1);   // whoever sent it to us is the speaker
        // relay to the other clients (also when our own voice is off: they still hear each other)
        std::vector<uint8_t> message{gamenet::kVoice, speaker};
        message.insert(message.end(), data + 1, data + size);
        for (uint8_t s = 1; s <= net::kMaxPeers; ++s)
            if (s != slot) transport->send(s, false, message.data(), message.size());
    } else if (slot != 0) {
        return;
    }
    if (!enabled() || !playback || !speaker || speaker >= kMaxSpeakers || speaker == session::local_id() || mutes[speaker])
        return;
    const uint16_t sequence = uint16_t(data[1] | data[2] << 8);
    heard_ms[speaker].store(now_ms(), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(play_mutex);
    if (!speakers[speaker]) speakers[speaker] = std::make_unique<Speaker>();
    speakers[speaker]->jitter.push(sequence, data + 3, size - 3);
}

bool muted(uint32_t net_id) { return net_id < kMaxSpeakers && mutes[net_id]; }

bool talking(uint32_t net_id) {
    if (!net_id || net_id >= kMaxSpeakers || !enabled()) return false;
    const int64_t last = heard_ms[net_id].load(std::memory_order_relaxed);
    return last && now_ms() - last < 300;
}

bool toggle_mute(uint32_t net_id) {
    if (!net_id || net_id >= kMaxSpeakers) return false;
    mutes[net_id] = !mutes[net_id];
    if (mutes[net_id]) {   // stop what is already buffered
        std::lock_guard<std::mutex> lock(play_mutex);
        if (speakers[net_id]) speakers[net_id]->jitter.clear();
    }
    log("voice: net id %u %s", net_id, mutes[net_id] ? "muted" : "unmuted");
    return mutes[net_id];
}

}  // namespace u2mp::voice
#endif
