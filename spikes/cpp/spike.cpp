// op1-og-native language spike, C++ side. Same behaviour and algorithms as ../rust/src/main.rs; see
// ../README.md. ALSA duplex audio at 44.1 kHz, ALSA sequencer MIDI input, an SDL3 window, measured.
#include <alsa/asoundlib.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr int kVoices = 6;
constexpr int kW = 320, kH = 160;
constexpr float kPi = 3.14159265358979f;

int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
int64_t thread_cpu_ns() {
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

std::atomic<bool> g_run{true};

struct Config {
  std::string device = "hw:0,0";
  unsigned rate = 44100, period = 64, periods = 2;
  double duration = 30;
  bool window = true, capture = true, mute = false, rt = false;
  int rt_prio = 20;
  std::string json, notes, shot;
  double bench = 0;
};

// ---------------------------------------------------------------- lock-free primitives

// Single-producer single-consumer ring. push() on one thread, pop() on another; no locks, no allocation.
template <typename T, size_t N>
class Spsc {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
  T buf_[N];
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};

 public:
  bool push(const T& v) {
    size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) == N) return false;
    buf_[h & (N - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& v) {
    size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    v = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
};

struct NoteEvent {
  int64_t t_recv;
  uint8_t on, note, vel, src;  // src 1 = MIDI, 2 = window keyboard
};

struct Scope {
  float wave[kW];
  uint32_t active;
  uint64_t period;
};

// Wait-free triple buffer: the audio thread publishes the newest scope, the UI takes the newest one.
class TripleBuffer {
  Scope slot_[3]{};
  std::atomic<uint32_t> middle_{1};
  uint32_t back_ = 0, front_ = 2;
  static constexpr uint32_t kFresh = 4;

 public:
  Scope& back() { return slot_[back_]; }
  void publish() { back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & 3; }
  const Scope& read() {
    if (middle_.load(std::memory_order_acquire) & kFresh)
      front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3;
    return slot_[front_];
  }
};

// ---------------------------------------------------------------- DSP, float

inline float poly_blep(float t, float dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

struct Voice {
  float phase = 0, inc = 0, env = 0, amp = 0, ic1 = 0, ic2 = 0, lfo = 0, lfo_inc = 0;
  bool gate = false;
  int note = -1;
  uint64_t age = 0;
};

struct Synth {
  float sr;
  Voice v[kVoices];
  uint64_t clock = 0;
  float attack_step, release_mul;
  float hp_x[2] = {0, 0}, hp_y[2] = {0, 0};

  explicit Synth(float sample_rate)
      : sr(sample_rate),
        attack_step(1.0f / (0.005f * sample_rate)),
        release_mul(float(std::exp(-1.0 / (0.2 * sample_rate)))) {}

  void note_on(int note, int vel) {
    int pick = -1;
    for (int i = 0; i < kVoices; ++i)
      if (v[i].note < 0) { pick = i; break; }
    if (pick < 0) {  // steal the oldest
      pick = 0;
      for (int i = 1; i < kVoices; ++i)
        if (v[i].age < v[pick].age) pick = i;
    }
    Voice& vo = v[pick];
    vo.note = note;
    vo.inc = float(440.0 * std::pow(2.0, (note - 69) / 12.0) / sr);
    vo.lfo_inc = (2.0f + 0.37f * float(note % 5)) / sr;  // cutoff LFO, 2-3.5 Hz
    vo.amp = 0.25f * float(vel) / 127.0f;
    vo.gate = true;
    vo.age = ++clock;
  }
  void note_off(int note) {
    for (auto& vo : v)
      if (vo.note == note && vo.gate) vo.gate = false;
  }
  uint32_t active_mask() const {
    uint32_t m = 0;
    for (int i = 0; i < kVoices; ++i)
      if (v[i].note >= 0) m |= 1u << i;
    return m;
  }

  void render_voice(Voice& vo, float* l, float* r, unsigned n) {
    const float k = 1.0f;  // SVF damping (resonance 0.5)
    for (unsigned i = 0; i < n; ++i) {
      if (vo.gate) {
        vo.env += attack_step;
        if (vo.env > 1.0f) vo.env = 1.0f;
      } else {
        vo.env *= release_mul;
      }
      float t = vo.phase;
      float y = 2.0f * t - 1.0f - poly_blep(t, vo.inc);
      vo.phase += vo.inc;
      if (vo.phase >= 1.0f) vo.phase -= 1.0f;
      vo.lfo += vo.lfo_inc;
      if (vo.lfo >= 1.0f) vo.lfo -= 1.0f;
      float tri = 2.0f * std::fabs(2.0f * vo.lfo - 1.0f) - 1.0f;
      float fc = 300.0f + 6000.0f * vo.env * (0.6f + 0.4f * tri);
      float g = std::tan(kPi * fc / sr);
      float a1 = 1.0f / (1.0f + g * (g + k));
      float a2 = g * a1, a3 = g * a2;
      float v3 = y - vo.ic2;
      float v1 = a1 * vo.ic1 + a2 * v3;
      float v2 = vo.ic2 + a2 * vo.ic1 + a3 * v3;
      vo.ic1 = 2.0f * v1 - vo.ic1;
      vo.ic2 = 2.0f * v2 - vo.ic2;
      float out = v2 * vo.env * vo.amp;
      l[i] += out;
      r[i] += out;
    }
    if (!vo.gate && vo.env < 1e-5f) {
      vo.note = -1;
      vo.env = 0;
    }
  }

  // in_l/in_r may be null (no capture). Outputs are overwritten.
  void render(float* l, float* r, const float* in_l, const float* in_r, unsigned n) {
    for (unsigned i = 0; i < n; ++i) l[i] = r[i] = 0.0f;
    for (auto& vo : v)
      if (vo.note >= 0) render_voice(vo, l, r, n);
    if (in_l) {  // input monitor through a DC blocker
      const float* in[2] = {in_l, in_r};
      float* out[2] = {l, r};
      for (int c = 0; c < 2; ++c)
        for (unsigned i = 0; i < n; ++i) {
          float x = in[c][i];
          float y = x - hp_x[c] + 0.995f * hp_y[c];
          hp_x[c] = x;
          hp_y[c] = y;
          out[c][i] += 0.5f * y;
        }
    }
  }
};

// ---------------------------------------------------------------- DSP, Blackfin-style fixed point

inline int16_t sat16(int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : int16_t(x)); }
inline int16_t mult_fr16(int16_t a, int16_t b) { return sat16((int32_t(a) * int32_t(b)) >> 15); }
inline int32_t add_sat32(int32_t a, int32_t b) {
  int64_t s = int64_t(a) + int64_t(b);
  return s > INT32_MAX ? INT32_MAX : (s < INT32_MIN ? INT32_MIN : int32_t(s));
}

struct FixVoice {
  uint32_t phase = 0, inc = 0;
  int32_t env = 0, att = 0;
  int16_t low = 0, band = 0, amp = 0;
  bool gate = false;

  void render(int32_t* acc, unsigned n) {
    const int16_t q = 16384;
    for (unsigned i = 0; i < n; ++i) {
      if (gate) env = add_sat32(env, att);
      else env -= env >> 13;
      int16_t saw = int16_t(uint16_t(phase >> 16));
      phase += inc;
      int16_t env15 = int16_t(env >> 16);
      int16_t f = int16_t(1000 + ((int32_t(env15) * 12000) >> 15));
      low = sat16(int32_t(low) + mult_fr16(f, band));
      int16_t high = sat16(int32_t(saw) - int32_t(low) - int32_t(mult_fr16(q, band)));
      band = sat16(int32_t(band) + mult_fr16(f, high));
      int16_t o = mult_fr16(mult_fr16(low, env15), amp);
      acc[i] = add_sat32(acc[i], int32_t(o) << 14);
    }
  }
};

// ---------------------------------------------------------------- stats helpers

struct Pct {
  double p50 = 0, p99 = 0, max = 0, mean = 0;
  size_t n = 0;
};
Pct pct(std::vector<float> v) {
  Pct p;
  p.n = v.size();
  if (v.empty()) return p;
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (float x : v) sum += x;
  p.mean = sum / double(v.size());
  p.p50 = v[v.size() / 2];
  p.p99 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.99))];
  p.max = v.back();
  return p;
}
std::string pct_json(const Pct& p) {
  char b[200];
  snprintf(b, sizeof b, "{\"n\":%zu,\"mean\":%.4f,\"p50\":%.4f,\"p99\":%.4f,\"max\":%.4f}", p.n, p.mean, p.p50, p.p99,
           p.max);
  return b;
}

// ---------------------------------------------------------------- offline DSP benchmark

// sr comes from the command line so neither compiler can fold the sample rate into the DSP.
int run_bench(double secs, unsigned sr) {
  const unsigned block = 64;
  const uint64_t chord_len = sr / 4;
  const uint64_t frames = uint64_t(secs * sr);
  double best_float = 1e30, best_fixed = 1e30;
  double sum_float = 0;
  uint64_t sum_fixed = 0;
  for (int run = 0; run < 5; ++run) {
    {  // float
      Synth s(static_cast<float>(sr));
      float l[block], r[block];
      double acc = 0;
      int64_t t0 = now_ns();
      int chord = 0;
      for (uint64_t f = 0; f < frames; f += block) {
        if (f % chord_len < block) {
          for (int j = 0; j < kVoices; ++j) s.note_off(48 + ((chord - 1) * 7 + j * 4 + 36) % 36);
          for (int j = 0; j < kVoices; ++j) s.note_on(48 + (chord * 7 + j * 4) % 36, 100);
          ++chord;
        }
        s.render(l, r, nullptr, nullptr, block);
        for (unsigned i = 0; i < block; ++i) acc += double(std::fabs(l[i]));
      }
      double dt = double(now_ns() - t0);
      best_float = std::min(best_float, dt);
      sum_float = acc;
    }
    {  // fixed
      FixVoice fv[kVoices];
      int32_t accb[block];
      uint64_t check = 0;
      int64_t t0 = now_ns();
      int chord = 0;
      for (uint64_t f = 0; f < frames; f += block) {
        if (f % chord_len < block) {
          for (int j = 0; j < kVoices; ++j) {
            int note = 48 + (chord * 7 + j * 4) % 36;
            fv[j].inc = uint32_t(440.0 * std::pow(2.0, (note - 69) / 12.0) / sr * 4294967296.0);
            fv[j].att = int32_t(2147483647.0 / (0.005 * sr));
            fv[j].amp = 8192;
            fv[j].gate = (chord & 1) == 0;
          }
          ++chord;
        }
        for (unsigned i = 0; i < block; ++i) accb[i] = 0;
        for (auto& v : fv) v.render(accb, block);
        for (unsigned i = 0; i < block; ++i) check += uint32_t(accb[i]);
      }
      double dt = double(now_ns() - t0);
      best_fixed = std::min(best_fixed, dt);
      sum_fixed = check;
    }
  }
  double audio_ns = secs * 1e9;
  printf(
      "{\"lang\":\"cpp\",\"bench_seconds\":%.1f,\"voices\":%d,"
      "\"float\":{\"wall_ms\":%.3f,\"realtime_x\":%.1f,\"ns_per_voice_sample\":%.3f,\"checksum\":%.6f},"
      "\"fixed\":{\"wall_ms\":%.3f,\"realtime_x\":%.1f,\"ns_per_voice_sample\":%.3f,\"checksum\":%llu}}\n",
      secs, kVoices, best_float / 1e6, audio_ns / best_float, best_float / double(frames * kVoices), sum_float,
      best_fixed / 1e6, audio_ns / best_fixed, best_fixed / double(frames * kVoices), (unsigned long long)sum_fixed);
  return 0;
}

// ---------------------------------------------------------------- realtime priority via rtkit

bool request_rt(int prio) {
  rlimit rl{200000, 200000};  // rtkit requires RLIMIT_RTTIME
  setrlimit(RLIMIT_RTTIME, &rl);
  char cmd[320];
  snprintf(cmd, sizeof cmd,
           "busctl --system call org.freedesktop.RealtimeKit1 /org/freedesktop/RealtimeKit1 "
           "org.freedesktop.RealtimeKit1 MakeThreadRealtimeWithPID ttu %d %ld %d >/dev/null 2>&1",
           int(getpid()), long(syscall(SYS_gettid)), prio);
  int rc = system(cmd);
  (void)rc;
  const int policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;  // rtkit grants SCHED_RR|RESET_ON_FORK
  return policy == SCHED_RR || policy == SCHED_FIFO;
}

// ---------------------------------------------------------------- audio

struct Pcm {
  snd_pcm_t* h = nullptr;
  snd_pcm_format_t fmt = SND_PCM_FORMAT_S32_LE;
  snd_pcm_uframes_t period = 0, buffer = 0;
};

bool open_pcm(Pcm& p, const Config& c, snd_pcm_stream_t dir, std::string& err) {
  int rc = snd_pcm_open(&p.h, c.device.c_str(), dir, 0);
  if (rc < 0) {
    err = std::string("snd_pcm_open: ") + snd_strerror(rc);
    return false;
  }
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_malloc(&hw);
  snd_pcm_hw_params_any(p.h, hw);
  snd_pcm_hw_params_set_access(p.h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (snd_pcm_hw_params_test_format(p.h, hw, SND_PCM_FORMAT_S32_LE) < 0) p.fmt = SND_PCM_FORMAT_S16_LE;
  snd_pcm_hw_params_set_format(p.h, hw, p.fmt);
  snd_pcm_hw_params_set_channels(p.h, hw, 2);
  snd_pcm_hw_params_set_rate_resample(p.h, hw, 0);
  if ((rc = snd_pcm_hw_params_set_rate(p.h, hw, c.rate, 0)) < 0) {
    err = std::string("rate: ") + snd_strerror(rc);
    snd_pcm_hw_params_free(hw);
    snd_pcm_close(p.h);
    return false;
  }
  snd_pcm_uframes_t per = c.period;
  int d = 0;
  snd_pcm_hw_params_set_period_size_near(p.h, hw, &per, &d);
  // The hardware buffer is larger than the latency target so a late wakeup never blocks a write;
  // latency is the fill level (c.periods periods queued ahead), as PipeWire and JACK's -n do it.
  unsigned np = std::max(c.periods + 2, 4u);
  if (snd_pcm_hw_params_set_periods(p.h, hw, np, 0) < 0) {
    snd_pcm_uframes_t b = per * np;
    snd_pcm_hw_params_set_buffer_size_near(p.h, hw, &b);
  }
  if ((rc = snd_pcm_hw_params(p.h, hw)) < 0) {
    err = std::string("hw_params: ") + snd_strerror(rc);
    snd_pcm_hw_params_free(hw);
    snd_pcm_close(p.h);
    return false;
  }
  snd_pcm_hw_params_get_period_size(hw, &p.period, &d);
  snd_pcm_hw_params_get_buffer_size(hw, &p.buffer);
  snd_pcm_hw_params_free(hw);

  snd_pcm_sw_params_t* sw;
  snd_pcm_sw_params_malloc(&sw);
  snd_pcm_sw_params_current(p.h, sw);
  snd_pcm_uframes_t boundary = 0;
  snd_pcm_sw_params_get_boundary(sw, &boundary);
  snd_pcm_sw_params_set_start_threshold(p.h, sw, boundary);  // started explicitly
  // playback-only: wake when the fill drops to (periods - 1) periods; duplex: capture paces the loop
  snd_pcm_uframes_t avail_min = p.period;
  if (dir == SND_PCM_STREAM_PLAYBACK && !c.capture && p.buffer > (c.periods - 1) * p.period)
    avail_min = p.buffer - (c.periods - 1) * p.period;
  snd_pcm_sw_params_set_avail_min(p.h, sw, avail_min);
  snd_pcm_sw_params(p.h, sw);
  snd_pcm_sw_params_free(sw);
  return true;
}

struct NoteLog {
  int64_t t_recv, t_out;
  uint8_t on, note, vel, src;
};

struct AudioResult {
  bool ok = false;
  std::string err;
  std::string fmt;
  unsigned period = 0, buffer = 0;
  bool capture = false, linked = false, rt = false;
  uint64_t periods_run = 0, xrun_play = 0, xrun_cap = 0;
  std::vector<float> dsp_us, delay_frames;
  std::vector<NoteLog> notes;
  size_t n_notes = 0;
  double thread_cpu_s = 0, wall_s = 0;
};

using Ring = Spsc<NoteEvent, 1024>;

void audio_thread(const Config* cfg, Ring* midi_ring, Ring* ui_ring, TripleBuffer* tb, AudioResult* res) {
  const Config& c = *cfg;
  Pcm play, cap;
  if (!open_pcm(play, c, SND_PCM_STREAM_PLAYBACK, res->err)) {
    res->err = "playback " + res->err;
    g_run = false;
    return;
  }
  bool capture = c.capture;
  if (capture && !open_pcm(cap, c, SND_PCM_STREAM_CAPTURE, res->err)) {
    fprintf(stderr, "audio: capture unavailable (%s), playback only\n", res->err.c_str());
    res->err.clear();
    capture = false;
  }
  if (capture && (cap.period != play.period || cap.fmt != play.fmt)) {
    fprintf(stderr, "audio: capture params differ from playback, playback only\n");
    snd_pcm_close(cap.h);
    capture = false;
  }
  res->capture = capture;
  res->linked = capture && snd_pcm_link(cap.h, play.h) == 0;
  const unsigned P = unsigned(play.period);
  res->period = P;
  res->buffer = unsigned(play.buffer);
  res->fmt = play.fmt == SND_PCM_FORMAT_S32_LE ? "S32_LE" : "S16_LE";
  const bool s32 = play.fmt == SND_PCM_FORMAT_S32_LE;
  const size_t bps = s32 ? 4 : 2;

  std::vector<uint8_t> obuf(P * 2 * bps), ibuf(P * 2 * bps), silence(P * 2 * bps, 0);
  std::vector<float> l(P), r(P), il(P), ir(P);
  const size_t cap_periods = size_t(c.duration * c.rate / P) + 4096;
  res->dsp_us.assign(cap_periods, 0.0f);
  res->delay_frames.assign(cap_periods, 0.0f);
  res->notes.assign(200000, NoteLog{});
  float hist[kW] = {0};
  unsigned hist_pos = 0;
  Synth synth(float(c.rate));
  const float gain = c.mute ? 0.0f : 1.0f;
  size_t pending[64];

  if (c.rt) res->rt = request_rt(c.rt_prio);

  auto restart = [&]() {
    if (capture) snd_pcm_drop(cap.h);
    snd_pcm_drop(play.h);
    snd_pcm_prepare(play.h);
    if (capture) snd_pcm_prepare(cap.h);
    for (unsigned k = 0; k < c.periods; ++k) snd_pcm_writei(play.h, silence.data(), P);  // latency target
    snd_pcm_start(play.h);
    if (capture && !res->linked) snd_pcm_start(cap.h);
  };

  restart();
  const int64_t t_start = now_ns(), cpu_start = thread_cpu_ns();
  const int64_t t_end = t_start + int64_t(c.duration * 1e9);
  size_t idx = 0;
  while (g_run) {
    if (now_ns() >= t_end) break;  // also ends runs that xrun on every period
    if (capture) {
      snd_pcm_sframes_t got = 0;
      bool xr = false;
      while (got < snd_pcm_sframes_t(P)) {
        snd_pcm_sframes_t n = snd_pcm_readi(cap.h, ibuf.data() + size_t(got) * 2 * bps, P - snd_pcm_uframes_t(got));
        if (n < 0) { xr = true; break; }
        got += n;
      }
      if (xr) { res->xrun_cap++; restart(); continue; }
    } else {
      int w = snd_pcm_wait(play.h, 1000);
      snd_pcm_sframes_t avail = snd_pcm_avail_update(play.h);
      if (w < 0 || avail < 0) { res->xrun_play++; restart(); continue; }
      if (avail < snd_pcm_sframes_t(play.buffer - (c.periods - 1) * P)) continue;
    }
    const int64_t t0 = now_ns();
    // events
    size_t npend = 0;
    NoteEvent ev;
    for (Ring* ring : {midi_ring, ui_ring})
      while (ring->pop(ev)) {
        if (ev.on) synth.note_on(ev.note, ev.vel);
        else synth.note_off(ev.note);
        if (res->n_notes < res->notes.size()) {
          res->notes[res->n_notes] = NoteLog{ev.t_recv, 0, ev.on, ev.note, ev.vel, ev.src};
          if (npend < 64) pending[npend++] = res->n_notes;
          res->n_notes++;
        }
      }
    // input
    const float* inl = nullptr;
    const float* inr = nullptr;
    if (capture) {
      if (s32) {
        const int32_t* s = reinterpret_cast<const int32_t*>(ibuf.data());
        for (unsigned i = 0; i < P; ++i) {
          il[i] = float(s[2 * i]) * (1.0f / 2147483648.0f);
          ir[i] = float(s[2 * i + 1]) * (1.0f / 2147483648.0f);
        }
      } else {
        const int16_t* s = reinterpret_cast<const int16_t*>(ibuf.data());
        for (unsigned i = 0; i < P; ++i) {
          il[i] = float(s[2 * i]) * (1.0f / 32768.0f);
          ir[i] = float(s[2 * i + 1]) * (1.0f / 32768.0f);
        }
      }
      inl = il.data();
      inr = ir.data();
    }
    synth.render(l.data(), r.data(), inl, inr, P);
    if (s32) {
      int32_t* d = reinterpret_cast<int32_t*>(obuf.data());
      for (unsigned i = 0; i < P; ++i) {
        d[2 * i] = int32_t(std::clamp(l[i] * gain, -1.0f, 1.0f) * 2147483520.0f);
        d[2 * i + 1] = int32_t(std::clamp(r[i] * gain, -1.0f, 1.0f) * 2147483520.0f);
      }
    } else {
      int16_t* d = reinterpret_cast<int16_t*>(obuf.data());
      for (unsigned i = 0; i < P; ++i) {
        d[2 * i] = int16_t(std::clamp(l[i] * gain, -1.0f, 1.0f) * 32767.0f);
        d[2 * i + 1] = int16_t(std::clamp(r[i] * gain, -1.0f, 1.0f) * 32767.0f);
      }
    }
    for (unsigned i = 0; i < P; ++i) {
      hist[hist_pos] = l[i];
      hist_pos = (hist_pos + 1) % kW;
    }
    Scope& sc = tb->back();
    for (int x = 0; x < kW; ++x) sc.wave[x] = hist[(hist_pos + unsigned(x)) % kW];
    sc.active = synth.active_mask();
    sc.period = res->periods_run;
    tb->publish();
    const int64_t t1 = now_ns();

    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(play.h, &delay) < 0) { res->xrun_play++; restart(); continue; }
    const int64_t t_out = now_ns() + int64_t(double(delay) * 1e9 / c.rate);
    for (size_t k = 0; k < npend; ++k) res->notes[pending[k]].t_out = t_out;
    snd_pcm_sframes_t wr = snd_pcm_writei(play.h, obuf.data(), P);
    if (wr < 0) { res->xrun_play++; restart(); continue; }
    if (idx < cap_periods) {
      res->dsp_us[idx] = float(t1 - t0) / 1000.0f;
      res->delay_frames[idx] = float(delay);
      ++idx;
    }
    res->periods_run++;
  }
  g_run = false;
  res->thread_cpu_s = double(thread_cpu_ns() - cpu_start) / 1e9;
  res->wall_s = double(now_ns() - t_start) / 1e9;
  res->dsp_us.resize(idx);
  res->delay_frames.resize(idx);
  if (capture) snd_pcm_close(cap.h);
  snd_pcm_drop(play.h);
  snd_pcm_close(play.h);
  res->ok = true;
}

// ---------------------------------------------------------------- MIDI

void midi_thread(Ring* ring, std::string* info) {
  snd_seq_t* seq = nullptr;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
    *info = "no sequencer";
    return;
  }
  snd_seq_set_client_name(seq, "op1og-spike-cpp");
  int port = snd_seq_create_simple_port(seq, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  int me = snd_seq_client_id(seq);
  int subscribed = 0;
  snd_seq_client_info_t* ci;
  snd_seq_port_info_t* pi;
  snd_seq_client_info_malloc(&ci);
  snd_seq_port_info_malloc(&pi);
  snd_seq_client_info_set_client(ci, -1);
  while (snd_seq_query_next_client(seq, ci) >= 0) {
    int cl = snd_seq_client_info_get_client(ci);
    if (cl == 0 || cl == me) continue;
    if (strstr(snd_seq_client_info_get_name(ci), "Through")) continue;
    snd_seq_port_info_set_client(pi, cl);
    snd_seq_port_info_set_port(pi, -1);
    while (snd_seq_query_next_port(seq, pi) >= 0) {
      // hardware (USB/kernel) MIDI sources only; virtual senders connect to us themselves
      unsigned caps = snd_seq_port_info_get_capability(pi);
      unsigned need = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
      if ((caps & need) == need && !(caps & SND_SEQ_PORT_CAP_NO_EXPORT) &&
          (snd_seq_port_info_get_type(pi) & SND_SEQ_PORT_TYPE_HARDWARE) &&
          snd_seq_connect_from(seq, port, cl, snd_seq_port_info_get_port(pi)) >= 0)
        ++subscribed;
    }
  }
  snd_seq_client_info_free(ci);
  snd_seq_port_info_free(pi);
  char b[128];
  snprintf(b, sizeof b, "op1og-spike-cpp %d:%d, %d hardware source(s)", me, port, subscribed);
  *info = b;
  fprintf(stderr, "midi: %s\n", b);

  int npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
  std::vector<pollfd> pfd(static_cast<size_t>(npfd));
  snd_seq_poll_descriptors(seq, pfd.data(), unsigned(npfd), POLLIN);
  while (g_run) {
    if (poll(pfd.data(), nfds_t(npfd), 100) <= 0) continue;
    snd_seq_event_t* ev = nullptr;
    while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
      const int64_t t = now_ns();
      if (ev->type == SND_SEQ_EVENT_NOTEON || ev->type == SND_SEQ_EVENT_NOTEOFF) {
        NoteEvent ne{t, uint8_t(ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0),
                     ev->data.note.note, ev->data.note.velocity, 1};
        ring->push(ne);
      }
      ev = nullptr;
    }
  }
  snd_seq_close(seq);
}

// ---------------------------------------------------------------- UI

void put(uint32_t* fb, int x, int y, uint32_t c) {
  if (x >= 0 && x < kW && y >= 0 && y < kH) fb[y * kW + x] = c;
}
void line(uint32_t* fb, int x0, int y0, int x1, int y1, uint32_t c) {
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) {
    put(fb, x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}
void disc(uint32_t* fb, int cx, int cy, int rad, uint32_t c) {
  for (int y = -rad; y <= rad; ++y)
    for (int x = -rad; x <= rad; ++x)
      if (x * x + y * y <= rad * rad) put(fb, cx + x, cy + y, c);
}
void ring_outline(uint32_t* fb, int cx, int cy, int rad, uint32_t c) {
  for (int y = -rad; y <= rad; ++y)
    for (int x = -rad; x <= rad; ++x) {
      int d = x * x + y * y;
      if (d <= rad * rad && d >= (rad - 2) * (rad - 2)) put(fb, cx + x, cy + y, c);
    }
}

void draw(uint32_t* fb, const Scope& s, uint64_t frame) {
  static const uint32_t kDial[4] = {0xFF2F6BFFu, 0xFF1FCB5Au, 0xFFF0F0F0u, 0xFFFF6A1Fu};
  std::fill(fb, fb + kW * kH, 0xFF000000u);
  for (int x = 0; x < kW; x += 2) put(fb, x, 70, 0xFF303030u);
  int py = 70;
  for (int x = 0; x < kW; ++x) {
    int y = 70 - int(std::clamp(s.wave[x], -1.0f, 1.0f) * 55.0f);
    line(fb, x > 0 ? x - 1 : 0, py, x, y, 0xFFE0E0E0u);
    py = y;
  }
  for (int i = 0; i < kVoices; ++i) disc(fb, 20 + i * 14, 10, 4, (s.active >> i) & 1 ? 0xFFFF6A1Fu : 0xFF303030u);
  for (int i = 0; i < 4; ++i) {
    int cx = 40 + i * 80, cy = 136;
    ring_outline(fb, cx, cy, 16, kDial[i]);
    float a = float(frame) * 0.03f + float(i);
    line(fb, cx, cy, cx + int(std::cos(a) * 13.0f), cy + int(std::sin(a) * 13.0f), kDial[i]);
  }
}

int key_note(SDL_Keycode k) {
  static const char* row = "awsedftgyhujk";
  for (int i = 0; row[i]; ++i)
    if (k == SDL_Keycode(row[i])) return 60 + i;
  return -1;
}

struct UiResult {
  bool ok = false;
  std::string err, driver, renderer;
  std::vector<float> frame_cpu_ms, frame_interval_ms;
  bool shot_ok = false;
};

void run_ui(const Config& c, Ring* ui_ring, TripleBuffer* tb, UiResult* res) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    res->err = SDL_GetError();
    return;
  }
  SDL_Window* w = SDL_CreateWindow("op1-og spike (C++)", kW * 2, kH * 2, 0);
  SDL_Renderer* r = w ? SDL_CreateRenderer(w, nullptr) : nullptr;
  if (!r) {
    res->err = SDL_GetError();
    SDL_Quit();
    return;
  }
  SDL_SetRenderVSync(r, 1);
  SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  res->driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?";
  res->renderer = SDL_GetRendererName(r) ? SDL_GetRendererName(r) : "?";
  std::vector<uint32_t> fb(kW * kH);
  res->frame_cpu_ms.reserve(size_t(c.duration * 240) + 1000);
  res->frame_interval_ms.reserve(size_t(c.duration * 240) + 1000);
  uint64_t frame = 0;
  int64_t last = 0;
  while (g_run) {
    const int64_t t0 = now_ns();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_run = false;
      if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_KEY_UP) {
        int n = key_note(e.key.key);
        if (n >= 0) ui_ring->push(NoteEvent{now_ns(), uint8_t(e.type == SDL_EVENT_KEY_DOWN), uint8_t(n), 100, 2});
      }
    }
    draw(fb.data(), tb->read(), frame);
    SDL_UpdateTexture(tex, nullptr, fb.data(), kW * 4);
    SDL_RenderClear(r);
    SDL_RenderTexture(r, tex, nullptr, nullptr);
    if (!c.shot.empty() && frame == 90) {
      if (SDL_Surface* su = SDL_RenderReadPixels(r, nullptr)) {
        res->shot_ok = SDL_SaveBMP(su, c.shot.c_str());
        SDL_DestroySurface(su);
      }
    }
    const int64_t t1 = now_ns();
    SDL_RenderPresent(r);
    const int64_t t2 = now_ns();
    if (frame > 0) {
      res->frame_cpu_ms.push_back(float(t1 - t0) / 1e6f);
      res->frame_interval_ms.push_back(float(t2 - last) / 1e6f);
    }
    last = t2;
    ++frame;
  }
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(r);
  SDL_DestroyWindow(w);
  SDL_Quit();
  res->ok = true;
}

// ---------------------------------------------------------------- main

bool parse(int argc, char** argv, Config& c) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--device") c.device = next();
    else if (a == "--period") c.period = unsigned(atoi(next()));
    else if (a == "--periods") c.periods = unsigned(atoi(next()));
    else if (a == "--rate") c.rate = unsigned(atoi(next()));
    else if (a == "--duration") c.duration = atof(next());
    else if (a == "--no-window") c.window = false;
    else if (a == "--no-capture") c.capture = false;
    else if (a == "--mute") c.mute = true;
    else if (a == "--rt") c.rt = true;
    else if (a == "--rt-prio") c.rt_prio = atoi(next());
    else if (a == "--json") c.json = next();
    else if (a == "--notes") c.notes = next();
    else if (a == "--shot") c.shot = next();
    else if (a == "--bench-dsp") c.bench = atof(next());
    else {
      fprintf(stderr, "unknown option %s (see spikes/README.md)\n", a.c_str());
      return false;
    }
  }
  return true;
}

const char* jb(bool b) { return b ? "true" : "false"; }

}  // namespace

int main(int argc, char** argv) {
  Config c;
  if (!parse(argc, argv, c)) return 64;
  if (c.bench > 0) return run_bench(c.bench, c.rate);
  signal(SIGINT, [](int) { g_run = false; });
  signal(SIGTERM, [](int) { g_run = false; });

  static Ring midi_ring, ui_ring;
  static TripleBuffer tb;
  AudioResult ares;
  UiResult ures;
  std::string midi_info;
  rusage ru0;
  getrusage(RUSAGE_SELF, &ru0);
  const int64_t w0 = now_ns();

  std::thread mt(midi_thread, &midi_ring, &midi_info);
  std::thread at(audio_thread, &c, &midi_ring, &ui_ring, &tb, &ares);
  if (c.window) {
    run_ui(c, &ui_ring, &tb, &ures);
    if (!ures.ok) fprintf(stderr, "ui: %s (continuing without a window)\n", ures.err.c_str());
  }
  while (g_run) usleep(20000);
  at.join();
  mt.join();

  rusage ru1;
  getrusage(RUSAGE_SELF, &ru1);
  const double wall = double(now_ns() - w0) / 1e9;
  auto tv = [](const timeval& t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; };
  const double cpu = tv(ru1.ru_utime) - tv(ru0.ru_utime) + tv(ru1.ru_stime) - tv(ru0.ru_stime);

  if (!ares.ok) {
    fprintf(stderr, "audio: %s\n", ares.err.c_str());
    return 1;
  }
  const double period_us = double(ares.period) * 1e6 / c.rate;
  std::vector<float> load(ares.dsp_us.size());
  for (size_t i = 0; i < load.size(); ++i) load[i] = float(ares.dsp_us[i] / period_us * 100.0);
  std::vector<float> note_ms;
  for (size_t i = 0; i < ares.n_notes; ++i)
    if (ares.notes[i].src == 1 && ares.notes[i].on && ares.notes[i].t_out > 0)
      note_ms.push_back(float(ares.notes[i].t_out - ares.notes[i].t_recv) / 1e6f);
  Pct delay = pct(ares.delay_frames);

  char head[1200];
  snprintf(head, sizeof head,
           "{\"lang\":\"cpp\",\"device\":\"%s\",\"rate\":%u,\"format\":\"%s\",\"period\":%u,\"buffer\":%u,\"fill_periods\":%u,"
           "\"capture\":%s,\"linked\":%s,\"rt\":%s,\"mute\":%s,\"duration_s\":%.2f,\"periods_run\":%llu,"
           "\"xruns_play\":%llu,\"xruns_capture\":%llu,\"output_latency_ms\":%.3f,\"roundtrip_ms\":%.3f,"
           "\"process_cpu_pct\":%.2f,\"audio_thread_cpu_pct\":%.2f,\"max_rss_kb\":%ld,\"midi\":\"%s\","
           "\"window\":%s,\"video\":\"%s/%s\",\"shot\":%s",
           c.device.c_str(), c.rate, ares.fmt.c_str(), ares.period, ares.buffer, c.periods, jb(ares.capture), jb(ares.linked),
           jb(ares.rt), jb(c.mute), ares.wall_s, (unsigned long long)ares.periods_run,
           (unsigned long long)ares.xrun_play, (unsigned long long)ares.xrun_cap, delay.mean * 1000.0 / c.rate,
           ares.capture ? (double(ares.period) + delay.mean) * 1000.0 / c.rate : 0.0, cpu / wall * 100.0,
           ares.thread_cpu_s / ares.wall_s * 100.0, ru1.ru_maxrss, midi_info.c_str(), jb(ures.ok),
           ures.driver.c_str(), ures.renderer.c_str(), jb(ures.shot_ok));
  std::string js = head;
  js += ",\"dsp_us\":" + pct_json(pct(ares.dsp_us));
  js += ",\"dsp_load_pct\":" + pct_json(pct(load));
  js += ",\"delay_frames\":" + pct_json(delay);
  js += ",\"note_to_dac_ms\":" + pct_json(pct(note_ms));
  js += ",\"frame_cpu_ms\":" + pct_json(pct(ures.frame_cpu_ms));
  js += ",\"frame_interval_ms\":" + pct_json(pct(ures.frame_interval_ms));
  js += "}";
  printf("%s\n", js.c_str());
  if (!c.json.empty())
    if (FILE* f = fopen(c.json.c_str(), "w")) {
      fprintf(f, "%s\n", js.c_str());
      fclose(f);
    }
  if (!c.notes.empty())
    if (FILE* f = fopen(c.notes.c_str(), "w")) {
      fprintf(f, "i,on,note,vel,src,t_recv_ns,t_out_ns\n");
      for (size_t i = 0; i < ares.n_notes; ++i) {
        const NoteLog& n = ares.notes[i];
        fprintf(f, "%zu,%u,%u,%u,%u,%lld,%lld\n", i, n.on, n.note, n.vel, n.src, (long long)n.t_recv,
                (long long)n.t_out);
      }
      fclose(f);
    }
  return 0;
}
