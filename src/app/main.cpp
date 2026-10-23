// op1-og: the OG OP-1 native port. Wires the modules together (owner: t3).
//
//   op1-og [--device pipewire|hw:0,0|default|none|wav:FILE] [--period 32] [--fill 4] [--no-capture] [--no-rt]
//          [--firmware PATH] [--keyboard] [--no-window] [--duration S] [--screenshot FILE.bmp]
//          [--stats FILE.json] [--midi-name NAME] [--help]
//
// Environment: OP1_OG_FIRMWARE (default for --firmware), OP1_OG_DEVICE (default for --device),
// PIPEWIRE_LATENCY (set to 64/44100 here for the pipewire device unless the user set it).
// Exit status: 0 ok, 1 the audio device could not be opened, 64 usage.
// run.sh (t6) finds the desktop session and starts this; it is display-only unless --keyboard is given.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "op1/core.h"
#include "op1/io.h"
#include "op1/ui.h"

namespace {

std::atomic<bool> g_stop{false};

const char* kUsage =
    "usage: op1-og [options]\n"
    "  --device DEV       ALSA PCM: 'pipewire' (the desktop graph), 'default', hw:X,Y (direct, lowest latency),\n"
    "                     'none' (no audio), or wav:FILE (render the output in real time to a 16-bit stereo WAV).\n"
    "                     Default: $OP1_OG_DEVICE, else pipewire on a desktop session, else hw:0,0\n"
    "  --period N         frames per period (default 32)\n"
    "  --fill N           periods queued ahead = latency target (default 4)\n"
    "  --no-capture       playback only (no live input)\n"
    "  --no-rt            do not ask rtkit for realtime priority\n"
    "  --firmware PATH    your OG firmware image, read at run time for the original screens\n"
    "                     (default $OP1_OG_FIRMWARE)\n"
    "  --keyboard         show the on-screen keyboard and key-mapping overlay\n"
    "  --no-window        run headless\n"
    "  --duration S       exit after S seconds\n"
    "  --screenshot FILE  save the window once (BMP) after about 1.5 s\n"
    "  --ui-script S      scripted input: steps (key=F1,enc=blue:3,wait=200,shot=a.png,...) or @FILE;\n"
    "                     with --no-window the UI runs headless and the run ends with the script\n"
    "  --stats FILE       write run statistics as JSON\n"
    "  --midi-name NAME   ALSA sequencer client name (default 'OP-1 OG')\n";

int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

std::string env_or(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  return v && *v ? v : fallback;
}

// A 16-bit stereo WAV written by the --device wav:FILE render thread (not the realtime path).
struct WavOut {
  FILE* f = nullptr;
  uint32_t frames = 0;
  bool open(const std::string& path) {
    f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    uint8_t zero[44] = {};
    return std::fwrite(zero, 1, 44, f) == 44;  // header written by close()
  }
  void write(const op1::Sample* l, const op1::Sample* r, int n) {
    int16_t buf[2 * 64];
    for (int i = 0; i < n; ++i) {
      buf[2 * i] = int16_t(l[i] >> 16);
      buf[2 * i + 1] = int16_t(r[i] >> 16);
    }
    frames += uint32_t(std::fwrite(buf, 4, size_t(n), f));
  }
  void close() {
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f); u32(36 + frames * 4); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(op1::kSampleRate); u32(op1::kSampleRate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(frames * 4);
    std::fclose(f);
    f = nullptr;
  }
};

}  // namespace

int main(int argc, char** argv) {
  op1::AudioOptions audio_opt;
  audio_opt.device = op1::default_audio_device();
  op1::UiOptions ui_opt;
  std::string firmware = env_or("OP1_OG_FIRMWARE", ""), stats_path, midi_name = "OP-1 OG";
  bool window = true;
  double duration = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { std::fprintf(stderr, "op1-og: %s needs a value\n%s", a.c_str(), kUsage); std::exit(64); }
      return argv[++i];
    };
    if (a == "--device") audio_opt.device = next();
    else if (a == "--period") audio_opt.period = std::atoi(next().c_str());
    else if (a == "--fill") audio_opt.fill = std::atoi(next().c_str());
    else if (a == "--no-capture") audio_opt.capture = false;
    else if (a == "--no-rt") audio_opt.rt = false;
    else if (a == "--firmware") firmware = next();
    else if (a == "--keyboard") ui_opt.keyboard_overlay = true;
    else if (a == "--no-window") window = false;
    else if (a == "--duration") duration = std::atof(next().c_str());
    else if (a == "--screenshot") ui_opt.screenshot = next();
    else if (a == "--ui-script") ui_opt.script = next();
    else if (a == "--stats") stats_path = next();
    else if (a == "--midi-name") midi_name = next();
    else if (a == "-h" || a == "--help") { std::fputs(kUsage, stdout); return 0; }
    else { std::fprintf(stderr, "op1-og: unknown option %s\n%s", a.c_str(), kUsage); return 64; }
  }
  if (audio_opt.period < 8 || audio_opt.period > 4096 || audio_opt.fill < 2 || audio_opt.fill > 16) {
    std::fprintf(stderr, "op1-og: --period must be 8..4096 and --fill 2..16\n");
    return 64;
  }
  ui_opt.duration_s = duration;
  // The ALSA pipewire plugin asks the graph for a short quantum when this is set (default quantum: ~8.6 ms
  // MIDI-to-DAC measured on magix-system; PARITY-AUDIT.md §1). The user's own value wins.
  if (audio_opt.device == "pipewire") setenv("PIPEWIRE_LATENCY", "64/44100", 0);
  std::signal(SIGINT, [](int) { g_stop = true; });
  std::signal(SIGTERM, [](int) { g_stop = true; });

  op1::Core core;
  auto assets = op1::create_asset_provider();
  std::string err;
  if (!assets->open(firmware, err)) std::fprintf(stderr, "op1-og: firmware assets: %s\n", err.c_str());
  auto worker = op1::create_worker();
  if (!worker->start(core, assets.get(), err)) std::fprintf(stderr, "op1-og: worker: %s\n", err.c_str());
  auto midi = op1::create_midi_io(midi_name);
  std::string midi_desc = "none";
  if (!midi->start(core, err)) std::fprintf(stderr, "op1-og: midi: %s\n", err.c_str());
  else std::fprintf(stderr, "op1-og: midi %s\n", (midi_desc = midi->describe()).c_str());
  auto audio = op1::create_alsa_backend();
  // --device none: no sound card; a timer thread runs the core at real time so the screens and controls
  // can be driven and screenshotted on a machine without audio (CI, SSH, tests). --device wav:FILE does the
  // same and writes everything the core outputs to FILE (an offline check of the whole audio path).
  std::atomic<bool> null_run{false};
  std::thread null_thread;
  const bool wav_audio = audio_opt.device.rfind("wav:", 0) == 0;
  const bool null_audio = audio_opt.device == "none" || wav_audio;
  WavOut wav;
  if (wav_audio && !wav.open(audio_opt.device.substr(4))) {
    std::fprintf(stderr, "op1-og: audio: cannot write %s\n", audio_opt.device.c_str() + 4);
    midi->stop();
    worker->stop();
    return 1;
  }
  if (null_audio) {
    null_run = true;
    null_thread = std::thread([&] {
      std::vector<op1::Sample> l(64), r(64);
      auto next = std::chrono::steady_clock::now();
      while (null_run) {
        core.set_dac_time(now_ns());
        core.process(nullptr, nullptr, l.data(), r.data(), 64);
        if (wav_audio) wav.write(l.data(), r.data(), 64);
        next += std::chrono::nanoseconds(int64_t(64) * 1000000000 / op1::kSampleRate);
        std::this_thread::sleep_until(next);
      }
    });
    std::fprintf(stderr, "op1-og: audio out: %s\n", wav_audio ? audio_opt.device.c_str() : "none (silent timer)");
  } else if (!audio->start(core, audio_opt, err)) {
    std::fprintf(stderr, "op1-og: audio: %s\n", err.c_str());
    midi->stop();
    worker->stop();
    return 1;
  } else {
    std::fprintf(stderr, "op1-og: audio out: %s%s\n", audio_opt.device.c_str(),
                 audio_opt.device == "pipewire" ? " (desktop graph, PipeWire default output)" : "");
  }

  const int64_t t_end = duration > 0 ? now_ns() + int64_t(duration * 1e9) : 0;
  const bool scripted = !ui_opt.script.empty() || std::getenv("OP1_OG_SCRIPT");
  int ui_rc = window ? op1::run_ui(core, *assets, worker.get(), ui_opt) : 2;
  if (window && ui_rc == 2) std::fprintf(stderr, "op1-og: no display, running headless\n");
  if (ui_rc == 2 && scripted) ui_rc = op1::run_ui_headless(core, *assets, ui_opt, worker.get());  // the UI without a window
  if (ui_rc != 2) g_stop = true;  // the window was closed, its duration passed, or the script ended
  while (!g_stop && (!t_end || now_ns() < t_end)) std::this_thread::sleep_for(std::chrono::milliseconds(20));

  if (null_audio) {
    null_run = false;
    null_thread.join();
    wav.close();
  } else {
    audio->stop();
  }
  midi->stop();
  worker->stop();

  // MIDI note -> DAC latency, the method of language-benchmark.md
  std::vector<double> lat;
  op1::NoteTiming nt;
  while (core.pop_note_timing(nt))
    if (nt.src == op1::Source::Midi) lat.push_back(double(nt.dac_ns - nt.received_ns) / 1e6);
  std::sort(lat.begin(), lat.end());
  auto q = [&](double p) { return lat.empty() ? 0.0 : lat[std::min(lat.size() - 1, size_t(double(lat.size()) * p))]; };

  op1::AudioStats s = null_audio ? op1::AudioStats{} : audio->stats();
  if (null_audio) s.device = audio_opt.device;
  char js[1024];
  std::snprintf(js, sizeof js,
                "{\"device\":\"%s\",\"format\":\"%s\",\"rate\":%d,\"period\":%d,\"buffer\":%d,\"fill\":%d,\"rt\":%s,"
                "\"capture\":%s,\"linked\":%s,\"periods\":%llu,\"xruns\":%llu,\"out_latency_ms\":%.3f,"
                "\"dsp_us\":{\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                "\"midi_note_to_dac_ms\":{\"n\":%zu,\"p50\":%.3f,\"p99\":%.3f,\"max\":%.3f},\"midi\":\"%s\"}",
                s.device.c_str(), s.format.c_str(), op1::kSampleRate, s.period, s.buffer, s.fill, s.rt ? "true" : "false",
                s.capture ? "true" : "false", s.linked ? "true" : "false", (unsigned long long)s.periods,
                (unsigned long long)s.xruns, s.out_latency_ms, s.dsp_us_p50, s.dsp_us_p99, s.dsp_us_max, lat.size(),
                q(0.5), q(0.99), lat.empty() ? 0.0 : lat.back(), midi_desc.c_str());
  std::printf("%s\n", js);
  if (!stats_path.empty())
    if (FILE* f = std::fopen(stats_path.c_str(), "w")) {
      std::fprintf(f, "%s\n", js);
      std::fclose(f);
    }
  return 0;
}
