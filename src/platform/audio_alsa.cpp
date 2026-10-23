// ALSA audio backend (owner: t3). The duplex loop measured in spikes/ (language-benchmark.md):
// capture is linked to playback and paces the loop; the playback buffer is larger than the latency
// target and kept at `fill` periods; the audio thread asks rtkit for SCHED_RR and never allocates.
#include <alsa/asoundlib.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "op1/core.h"
#include "op1/io.h"

namespace op1 {
namespace {

int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// SCHED_RR for the calling thread through rtkit (no CAP_SYS_NICE / rtprio limit needed).
bool request_rt(int prio) {
  rlimit rl{200000, 200000};  // rtkit requires RLIMIT_RTTIME
  setrlimit(RLIMIT_RTTIME, &rl);
  char cmd[320];
  std::snprintf(cmd, sizeof cmd,
                "busctl --system call org.freedesktop.RealtimeKit1 /org/freedesktop/RealtimeKit1 "
                "org.freedesktop.RealtimeKit1 MakeThreadRealtimeWithPID ttu %d %ld %d >/dev/null 2>&1",
                int(getpid()), long(syscall(SYS_gettid)), prio);
  if (std::system(cmd) != 0) return false;
  const int policy = sched_getscheduler(0) & ~SCHED_RESET_ON_FORK;
  return policy == SCHED_RR || policy == SCHED_FIFO;
}

struct Pcm {
  snd_pcm_t* h = nullptr;
  snd_pcm_format_t fmt = SND_PCM_FORMAT_S32_LE;
  snd_pcm_uframes_t period = 0, buffer = 0;
  void close() {
    if (h) snd_pcm_close(h);
    h = nullptr;
  }
};

bool open_pcm(Pcm& p, const AudioOptions& o, snd_pcm_stream_t dir, std::string& err) {
  int rc = snd_pcm_open(&p.h, o.device.c_str(), dir, 0);
  if (rc < 0) { err = std::string("snd_pcm_open ") + o.device + ": " + snd_strerror(rc); p.h = nullptr; return false; }
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(p.h, hw);
  snd_pcm_hw_params_set_access(p.h, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  p.fmt = snd_pcm_hw_params_test_format(p.h, hw, SND_PCM_FORMAT_S32_LE) == 0 ? SND_PCM_FORMAT_S32_LE : SND_PCM_FORMAT_S16_LE;
  snd_pcm_hw_params_set_format(p.h, hw, p.fmt);
  snd_pcm_hw_params_set_channels(p.h, hw, 2);
  snd_pcm_hw_params_set_rate_resample(p.h, hw, 0);
  if ((rc = snd_pcm_hw_params_set_rate(p.h, hw, kSampleRate, 0)) < 0) {
    err = std::string("44100 Hz not supported: ") + snd_strerror(rc);
    p.close();
    return false;
  }
  snd_pcm_uframes_t per = snd_pcm_uframes_t(o.period);
  int d = 0;
  snd_pcm_hw_params_set_period_size_near(p.h, hw, &per, &d);
  const unsigned np = unsigned(std::max(o.fill + 2, 4));  // buffer > latency target: a late wakeup never blocks a write
  if (snd_pcm_hw_params_set_periods(p.h, hw, np, 0) < 0) {
    snd_pcm_uframes_t b = per * np;
    snd_pcm_hw_params_set_buffer_size_near(p.h, hw, &b);
  }
  if ((rc = snd_pcm_hw_params(p.h, hw)) < 0) { err = std::string("hw_params: ") + snd_strerror(rc); p.close(); return false; }
  snd_pcm_hw_params_get_period_size(hw, &p.period, &d);
  snd_pcm_hw_params_get_buffer_size(hw, &p.buffer);

  snd_pcm_sw_params_t* sw;
  snd_pcm_sw_params_alloca(&sw);
  snd_pcm_sw_params_current(p.h, sw);
  snd_pcm_uframes_t boundary = 0;
  snd_pcm_sw_params_get_boundary(sw, &boundary);
  snd_pcm_sw_params_set_start_threshold(p.h, sw, boundary);  // started explicitly
  snd_pcm_uframes_t avail_min = p.period;
  const snd_pcm_uframes_t keep = snd_pcm_uframes_t(o.fill - 1) * p.period;
  if (dir == SND_PCM_STREAM_PLAYBACK && !o.capture && p.buffer > keep) avail_min = p.buffer - keep;
  snd_pcm_sw_params_set_avail_min(p.h, sw, avail_min);
  snd_pcm_sw_params(p.h, sw);
  return true;
}

class AlsaBackend final : public AudioBackend {
 public:
  ~AlsaBackend() override { stop(); }

  bool start(Core& core, const AudioOptions& opt, std::string& err) override {
    opt_ = opt;
    if (opt_.fill < 2) opt_.fill = 2;
    if (!open_pcm(play_, opt_, SND_PCM_STREAM_PLAYBACK, err)) return false;
    capture_ = opt_.capture;
    std::string cerr;
    if (capture_ && !open_pcm(cap_, opt_, SND_PCM_STREAM_CAPTURE, cerr)) {
      std::fprintf(stderr, "op1-og: capture unavailable (%s), playback only\n", cerr.c_str());
      capture_ = false;
    }
    if (capture_ && (cap_.period != play_.period || cap_.fmt != play_.fmt)) {
      std::fprintf(stderr, "op1-og: capture parameters differ from playback, playback only\n");
      cap_.close();
      capture_ = false;
    }
    linked_ = capture_ && snd_pcm_link(cap_.h, play_.h) == 0;
    // Everything the loop touches is allocated and written here, before the realtime thread starts.
    const size_t n = play_.period * 2;
    const size_t bps = play_.fmt == SND_PCM_FORMAT_S32_LE ? 4 : 2;
    obuf_.assign(n * bps, 0);
    ibuf_.assign(n * bps, 0);
    silence_.assign(n * bps, 0);
    in_l_.assign(play_.period, 0); in_r_.assign(play_.period, 0);
    out_l_.assign(play_.period, 0); out_r_.assign(play_.period, 0);
    hist_.assign(kHistBins, 0);
    run_ = true;
    thread_ = std::thread([this, &core] { loop(core); });
    return true;
  }

  void stop() override {
    run_ = false;
    if (thread_.joinable()) thread_.join();
    if (play_.h) snd_pcm_drop(play_.h);
    cap_.close();
    play_.close();
  }

  AudioStats stats() const override {
    AudioStats s;
    s.device = opt_.device;
    s.format = play_.fmt == SND_PCM_FORMAT_S32_LE ? "S32_LE" : "S16_LE";
    s.period = int(play_.period);
    s.buffer = int(play_.buffer);
    s.fill = opt_.fill;
    s.rt = rt_;
    s.capture = capture_;
    s.linked = linked_;
    s.periods = periods_;
    s.xruns = xruns_;
    s.out_latency_ms = delay_count_ ? double(delay_sum_) / double(delay_count_) * 1000.0 / kSampleRate : 0;
    s.dsp_us_p50 = percentile(0.50);
    s.dsp_us_p99 = percentile(0.99);
    s.dsp_us_max = double(dsp_max_ns_) / 1000.0;
    return s;
  }

 private:
  static constexpr size_t kHistBins = 20000;  // 0.5 us bins up to 10 ms

  double percentile(double q) const {
    uint64_t total = 0;
    for (uint64_t c : hist_) total += c;
    if (!total) return 0;
    uint64_t want = uint64_t(double(total) * q), acc = 0;
    for (size_t i = 0; i < hist_.size(); ++i)
      if ((acc += hist_[i]) > want) return double(i) * 0.5;
    return double(hist_.size()) * 0.5;
  }

  void restart() {
    if (capture_) snd_pcm_drop(cap_.h);
    snd_pcm_drop(play_.h);
    snd_pcm_prepare(play_.h);
    if (capture_) snd_pcm_prepare(cap_.h);
    for (int k = 0; k < opt_.fill; ++k) snd_pcm_writei(play_.h, silence_.data(), play_.period);  // latency target
    snd_pcm_start(play_.h);
    if (capture_ && !linked_) snd_pcm_start(cap_.h);
  }

  void loop(Core& core) {
    if (opt_.rt) rt_ = request_rt(opt_.rt_prio);
    const snd_pcm_uframes_t P = play_.period;
    const bool s32 = play_.fmt == SND_PCM_FORMAT_S32_LE;
    const size_t frame_bytes = s32 ? 8 : 4;
    const snd_pcm_sframes_t want_avail = snd_pcm_sframes_t(play_.buffer - snd_pcm_uframes_t(opt_.fill - 1) * P);
    restart();
    while (run_) {
      if (capture_) {
        snd_pcm_uframes_t got = 0;
        bool xr = false;
        while (got < P) {
          const snd_pcm_sframes_t r = snd_pcm_readi(cap_.h, ibuf_.data() + got * frame_bytes, P - got);
          if (r < 0) { xr = true; break; }
          got += snd_pcm_uframes_t(r);
        }
        if (xr) { xrun(core); continue; }
        if (s32) {
          const int32_t* s = reinterpret_cast<const int32_t*>(ibuf_.data());
          for (size_t i = 0; i < P; ++i) { in_l_[i] = s[2 * i]; in_r_[i] = s[2 * i + 1]; }
        } else {
          const int16_t* s = reinterpret_cast<const int16_t*>(ibuf_.data());
          for (size_t i = 0; i < P; ++i) { in_l_[i] = int32_t(s[2 * i]) << 16; in_r_[i] = int32_t(s[2 * i + 1]) << 16; }
        }
      } else {
        const int w = snd_pcm_wait(play_.h, 1000);
        const snd_pcm_sframes_t avail = snd_pcm_avail_update(play_.h);
        if (w < 0 || avail < 0) { xrun(core); continue; }
        if (avail < want_avail) continue;
      }
      snd_pcm_sframes_t delay = 0;
      if (snd_pcm_delay(play_.h, &delay) < 0) { xrun(core); continue; }
      const int64_t t0 = now_ns();
      core.set_dac_time(t0 + delay * 1000000000 / kSampleRate);
      core.process(capture_ ? in_l_.data() : nullptr, capture_ ? in_r_.data() : nullptr, out_l_.data(), out_r_.data(),
                   int(P));
      if (s32) {
        int32_t* d = reinterpret_cast<int32_t*>(obuf_.data());
        for (size_t i = 0; i < P; ++i) { d[2 * i] = out_l_[i]; d[2 * i + 1] = out_r_[i]; }
      } else {
        int16_t* d = reinterpret_cast<int16_t*>(obuf_.data());
        for (size_t i = 0; i < P; ++i) { d[2 * i] = int16_t(out_l_[i] >> 16); d[2 * i + 1] = int16_t(out_r_[i] >> 16); }
      }
      const int64_t dt = now_ns() - t0;
      if (snd_pcm_writei(play_.h, obuf_.data(), P) < 0) { xrun(core); continue; }
      hist_[std::min<size_t>(size_t(dt / 500), kHistBins - 1)]++;
      dsp_max_ns_ = std::max(dsp_max_ns_, dt);
      delay_sum_ += uint64_t(delay);
      delay_count_++;
      periods_++;
    }
  }

  void xrun(Core& core) {
    xruns_++;
    core.note_xrun();
    restart();
  }

  AudioOptions opt_;
  Pcm play_, cap_;
  bool capture_ = false, linked_ = false, rt_ = false;
  std::vector<uint8_t> obuf_, ibuf_, silence_;
  std::vector<Sample> in_l_, in_r_, out_l_, out_r_;
  std::vector<uint64_t> hist_;
  int64_t dsp_max_ns_ = 0;
  uint64_t delay_sum_ = 0, delay_count_ = 0, periods_ = 0, xruns_ = 0;
  std::atomic<bool> run_{false};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<AudioBackend> create_alsa_backend() { return std::make_unique<AlsaBackend>(); }

namespace {
bool socket_exists(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}
bool alsa_pcm_defined(const char* name) {
  if (snd_config_update() < 0 || !snd_config) return false;
  snd_config_t* n = nullptr;
  return snd_config_search(snd_config, (std::string("pcm.") + name).c_str(), &n) == 0;
}
}  // namespace

std::string default_audio_device() {
  if (const char* v = std::getenv("OP1_OG_DEVICE"); v && *v) return v;
  const char* rt = std::getenv("XDG_RUNTIME_DIR");
  const std::string dir = rt && *rt ? std::string(rt) : "/run/user/" + std::to_string(getuid());
  // A desktop sound server owns the cards: play into it, so the sound goes where the user listens.
  if (socket_exists(dir + "/pipewire-0") && alsa_pcm_defined("pipewire")) return "pipewire";
  if (socket_exists(dir + "/pipewire-0") || socket_exists(dir + "/pulse/native")) return "default";
  return "hw:0,0";
}

}  // namespace op1
