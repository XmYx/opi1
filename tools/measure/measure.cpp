// t7 measurement harness for op1-og-native (QA; additive, not part of the build glob).
//
//   measure offline [--blocks N]            per-engine / per-effect / per-LFO / worst-case CPU of Core::process,
//                                           one 64-frame block per sample, no device. Run under taskset.
//   measure live --seconds S [--worst] [--period P --fill F --device DEV --key-ms MS --no-capture]
//                                           Core + ALSA backend + MIDI port like op1-og, plus scripted setup
//                                           (worst case: 4 recorded tape tracks, 6+6 voices, fx, lfo, master
//                                           fx, arpeggio on both sides) and keyboard notes (Source::Keyboard)
//                                           every --key-ms. Prints one JSON object.
//
// Method = language-benchmark.md: latency from the backend's NoteTiming (received -> DAC estimate), xruns and
// dsp time from AudioBackend::stats(). Build: tools/measure/build.sh.
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "op1/core.h"
#include "op1/dsp.h"
#include "op1/io.h"
#include "op1/sequencer.h"
#include "op1/tape.h"

using namespace op1;

static int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

static long vm_kb(const char* key) {
  FILE* f = std::fopen("/proc/self/status", "r");
  char line[256];
  long v = -1;
  while (f && std::fgets(line, sizeof line, f))
    if (!std::strncmp(line, key, std::strlen(key))) { v = std::atol(line + std::strlen(key)); break; }
  if (f) std::fclose(f);
  return v;
}

struct Rig {
  Core core;
  std::vector<Sample> l = std::vector<Sample>(kBlock), r = std::vector<Sample>(kBlock);
  std::vector<int16_t> smp;  // user-style test audio for sampler/drum (no TE audio)
  SampleData sd;
  bool offline = true;

  Rig() {
    smp.resize(2 * kSampleRate);
    for (size_t i = 0; i < smp.size(); ++i) smp[i] = int16_t(((i / 60) % 2 ? 9000 : -9000) + int((i * 7919) % 2001) - 1000);
    sd.frames = smp.data();
    sd.length = int32_t(smp.size());
  }
  void cmd(Domain d, uint16_t op, int32_t value, int32_t index = 0, uint8_t slot = 0, void* ptr = nullptr) {
    Command c;
    c.domain = d; c.op = op; c.value = value; c.index = index; c.slot = slot; c.ptr = ptr;
    while (!core.post_ui(c)) { if (offline) block(); else usleep(1000); }
  }
  void block() { core.process(nullptr, nullptr, l.data(), r.data(), kBlock); }
  void run(int blocks) { for (int i = 0; i < blocks; ++i) block(); }
  void notes_on(Target t, int n, int base) {
    for (int i = 0; i < n; ++i) core.post_midi(note_on(uint8_t(base + 3 * i), 100, Source::Midi, now_ns(), t));
  }
  void notes_off(Target t, int n, int base) {
    for (int i = 0; i < n; ++i) core.post_midi(note_off(uint8_t(base + 3 * i), Source::Midi, now_ns(), t));
  }
  void set_engine(Domain d, InstrumentType t) {
    cmd(d, uint16_t(InstrumentOp::SetType), int32_t(t));
    if (t == InstrumentType::Sampler || t == InstrumentType::Drum)
      cmd(d, uint16_t(InstrumentOp::SetSample), 0, 0, 0, const_cast<SampleData*>(&sd));
  }
};

struct Stat { double mean, p50, p99, max; };
static Stat stat_of(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (double x : v) sum += x;
  auto q = [&](double p) { return v[std::min(v.size() - 1, size_t(double(v.size()) * p))]; };
  return {sum / double(v.size()), q(0.5), q(0.99), v.back()};
}

// time `blocks` consecutive 64-frame process() calls, in microseconds each
// retrigger: notes are sent again every 70 blocks (~100 ms) on that target, so one-shot samples (drum, sampler)
// keep sounding instead of ending after their 2 s.
static Stat time_blocks(Rig& g, int blocks, Target retrig = Target::Auto, int base = 0) {
  std::vector<double> us(static_cast<size_t>(blocks), 0.0);
  for (int i = 0; i < blocks; ++i) {
    if (retrig != Target::Auto && i % 70 == 0) { g.notes_off(retrig, 6, base); g.notes_on(retrig, 6, base); }
    const int64_t t0 = now_ns();
    g.block();
    us[size_t(i)] = double(now_ns() - t0) / 1e3;
  }
  return stat_of(us);
}

static const double kBlockUs = 1e6 * kBlock / kSampleRate;  // 1451.2

static void row(const char* group, const std::string& name, const char* state, const Stat& s) {
  std::printf("%s,%s,%s,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%ld,%ld\n", group, name.c_str(), state, s.mean, s.p50, s.p99, s.max,
              100 * s.mean / kBlockUs, 100 * s.p99 / kBlockUs, vm_kb("VmRSS:"), vm_kb("VmHWM:"));
  std::fflush(stdout);
}

static const char* eng_name(InstrumentType t) {
  static const char* n[] = {"fm", "digital", "drwave", "string", "cluster", "pulse", "sampler", "phase", "drum",
                            "dbox", "ls", "dna", "dsynth", "voltage", "iter"};
  return n[int(t)];
}
static const char* fx_name(int i) {
  static const char* n[] = {"delay", "grid", "punch", "phone", "spring", "filter", "nitro", "cwo"};
  return n[i];
}
static const char* lfo_name(int i) {
  static const char* n[] = {"tremolo", "value", "random", "element", "midi", "bend", "crank"};
  return n[i];
}

// 4 tape tracks recorded with live notes, then transport at 0 and playing; both instruments loud; fx and lfo on
// both; master fx on, tempo + MIDI clock out running (no arpeggio: it would cut the polyphony). Offline: blocks drive time. Live: sleeps drive time.
static void setup_worst(Rig& g, bool live) {
  auto advance = [&](double sec) {
    if (live) std::this_thread::sleep_for(std::chrono::duration<double>(sec));
    else g.run(int(sec * kSampleRate / kBlock));
  };
  g.set_engine(Domain::Synth, InstrumentType::Fm);
  g.set_engine(Domain::Drum, InstrumentType::Drum);
  for (Domain d : {Domain::Synth, Domain::Drum}) {
    g.cmd(d, uint16_t(InstrumentOp::SetFxType), int32_t(FxType::Delay));
    g.cmd(d, uint16_t(InstrumentOp::SetFxOn), 1);
    g.cmd(d, uint16_t(InstrumentOp::SetLfoType), int32_t(LfoType::Tremolo));
    g.cmd(d, uint16_t(InstrumentOp::SetLfoOn), 1);
  }
  g.cmd(Domain::Mixer, uint16_t(MixerOp::SetMasterFx), int32_t(FxType::Phone));
  g.cmd(Domain::Mixer, uint16_t(MixerOp::SetMasterFxOn), 1);
  for (int t = 0; t < kTapeTracks; ++t) g.cmd(Domain::Mixer, uint16_t(MixerOp::SetVolume), 30000, 0, uint8_t(t));
  advance(0.2);
  for (int t = 0; t < kTapeTracks; ++t) {  // record 4 s of notes on each track
    g.cmd(Domain::Tape, uint16_t(TapeOp::SelectTrack), t);
    g.cmd(Domain::Tape, uint16_t(TapeOp::Seek), 0);
    g.cmd(Domain::Tape, uint16_t(TapeOp::Record), 1);
    g.cmd(Domain::Tape, uint16_t(TapeOp::Play), 0);
    g.notes_on(Target::Synth, 6, 40 + 5 * t);
    advance(4.0);
    g.notes_off(Target::Synth, 6, 40 + 5 * t);
    g.cmd(Domain::Tape, uint16_t(TapeOp::Record), 0);
    g.cmd(Domain::Tape, uint16_t(TapeOp::Stop), 0);
    advance(0.1);
  }
  g.cmd(Domain::Tape, uint16_t(TapeOp::Seek), 0);
  g.cmd(Domain::Tape, uint16_t(TapeOp::SetLoop), 1);
  g.cmd(Domain::Tape, uint16_t(TapeOp::SetLoopIn), 0);
  g.cmd(Domain::Tape, uint16_t(TapeOp::SetLoopOut), 4 * kSampleRate);
  g.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1400);
  g.cmd(Domain::Tempo, uint16_t(TempoOp::SetSyncOut), 1);
  g.cmd(Domain::Tape, uint16_t(TapeOp::Play), 0);
  g.notes_on(Target::Synth, 6, 36);
  g.notes_on(Target::Drum, 6, 53);
  advance(0.5);
}

static int offline(int blocks) {
  std::printf("group,name,state,mean_us,p50_us,p99_us,max_us,mean_pct_block,p99_pct_block,rss_kb,hwm_kb\n");
  {
    Rig g;
    g.run(2000);
    row("baseline", "empty-graph", "idle", time_blocks(g, blocks));
  }
  const InstrumentType synth[] = {InstrumentType::Fm, InstrumentType::Digital, InstrumentType::DrWave,
                                  InstrumentType::String, InstrumentType::Cluster, InstrumentType::Pulse,
                                  InstrumentType::Sampler, InstrumentType::Phase, InstrumentType::Dna,
                                  InstrumentType::DSynth, InstrumentType::Voltage, InstrumentType::Iter};
  for (InstrumentType t : synth) {
    Rig g;
    g.set_engine(Domain::Synth, t);
    g.run(2000);
    row("engine", eng_name(t), "idle", time_blocks(g, blocks));
    g.notes_on(Target::Synth, 6, 40);
    g.cmd(Domain::Synth, uint16_t(InstrumentOp::SetFxOn), 1);
    g.cmd(Domain::Synth, uint16_t(InstrumentOp::SetLfoOn), 1);
    g.run(3000);
    row("engine", eng_name(t), "6voices+fx+lfo", time_blocks(g, blocks, t == InstrumentType::Sampler ? Target::Synth : Target::Auto, 40));
  }
  for (InstrumentType t : {InstrumentType::Drum, InstrumentType::Dbox}) {
    Rig g;
    g.cmd(Domain::Core, uint16_t(CoreOp::SetMode), int32_t(Mode::Drum));
    g.set_engine(Domain::Drum, t);
    g.run(2000);
    row("engine", eng_name(t), "idle", time_blocks(g, blocks));
    g.notes_on(Target::Drum, 6, 53);
    g.cmd(Domain::Drum, uint16_t(InstrumentOp::SetFxOn), 1);
    g.cmd(Domain::Drum, uint16_t(InstrumentOp::SetLfoOn), 1);
    g.run(3000);
    row("engine", eng_name(t), "6voices+fx+lfo", time_blocks(g, blocks, Target::Drum, 53));
  }
  for (int f = 0; f < int(FxType::Count); ++f) {  // each effect on the fm synth, 6 voices
    Rig g;
    g.set_engine(Domain::Synth, InstrumentType::Fm);
    g.cmd(Domain::Synth, uint16_t(InstrumentOp::SetFxType), f);
    g.cmd(Domain::Synth, uint16_t(InstrumentOp::SetFxOn), 1);
    g.notes_on(Target::Synth, 6, 40);
    g.run(3000);
    row("fx(mono,fm)", fx_name(f), "6voices", time_blocks(g, blocks));
  }
  for (int f = 0; f < int(FxType::Count); ++f) {  // each effect as the stereo master effect
    Rig g;
    g.set_engine(Domain::Synth, InstrumentType::Fm);
    g.cmd(Domain::Mixer, uint16_t(MixerOp::SetMasterFx), f);
    g.cmd(Domain::Mixer, uint16_t(MixerOp::SetMasterFxOn), 1);
    g.notes_on(Target::Synth, 6, 40);
    g.run(3000);
    row("fx(master,fm)", fx_name(f), "6voices", time_blocks(g, blocks));
  }
  for (int i = 0; i < int(LfoType::Count); ++i) {
    Rig g;
    g.set_engine(Domain::Synth, InstrumentType::Fm);
    g.cmd(Domain::Synth, uint16_t(InstrumentOp::SetLfoType), i);
    g.cmd(Domain::Synth, uint16_t(InstrumentOp::SetLfoOn), 1);
    g.notes_on(Target::Synth, 6, 40);
    g.run(3000);
    row("lfo(fm)", lfo_name(i), "6voices", time_blocks(g, blocks));
  }
  {
    Rig g;
    setup_worst(g, false);
    g.run(1000);
    row("worst", "4tape+6synth+6drum+fx+lfo+master+clockout", "all", time_blocks(g, blocks, Target::Drum, 53));
    const CoreSnapshot& s = g.core.snapshot();
    std::fprintf(stderr, "worst: tape playing=%d pos=%d clips=%d synth voices=0x%x drum voices=0x%x\n", s.tape.playing,
                 s.tape.position, s.tape.display[3], s.synth.voices_active, s.drum.voices_active);
  }
  return 0;
}

static int live(int argc, char** argv) {
  double seconds = 30, key_ms = 250;
  bool worst = false;
  AudioOptions ao;
  ao.device = "hw:0,0";
  std::string stats_path;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto nx = [&] { return std::string(i + 1 < argc ? argv[++i] : ""); };
    if (a == "--seconds") seconds = std::atof(nx().c_str());
    else if (a == "--worst") worst = true;
    else if (a == "--period") ao.period = std::atoi(nx().c_str());
    else if (a == "--fill") ao.fill = std::atoi(nx().c_str());
    else if (a == "--device") ao.device = nx();
    else if (a == "--key-ms") key_ms = std::atof(nx().c_str());
    else if (a == "--no-capture") ao.capture = false;
    else if (a == "--stats") stats_path = nx();
  }
  Rig g;
  g.offline = false;
  auto midi = create_midi_io("OP-1 OG measure");
  std::string err;
  if (!midi->start(g.core, err)) std::fprintf(stderr, "midi: %s\n", err.c_str());
  auto audio = create_alsa_backend();
  if (!audio->start(g.core, ao, err)) { std::fprintf(stderr, "audio: %s\n", err.c_str()); return 1; }
  const long rss0 = vm_kb("VmRSS:");
  timespec c0;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
  const int64_t t0 = now_ns();
  if (worst) setup_worst(g, true);
  else g.set_engine(Domain::Synth, InstrumentType::Fm);
  uint8_t held = 0;
  int64_t next = now_ns();
  const int64_t end = t0 + int64_t(seconds * 1e9);
  while (now_ns() < end) {  // keyboard notes: the UI thread's path (post_ui), one per --key-ms
    if (held) { g.core.post_ui(note_off(held, Source::Keyboard, now_ns())); held = 0; }
    else { held = uint8_t(60 + int((now_ns() / 1000) % 12)); g.core.post_ui(note_on(held, 100, Source::Keyboard, now_ns())); }
    next += int64_t(key_ms * 1e6 / 2);
    const int64_t d = next - now_ns();
    if (d > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(d));
  }
  const CoreSnapshot& cs = g.core.snapshot();
  const int tape_playing = cs.tape.playing, tape_clips = cs.tape.display[3];
  const unsigned sv = cs.synth.voices_active, dv = cs.drum.voices_active;
  const double wall = double(now_ns() - t0) / 1e9;
  timespec c1;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
  const double cpu_s = double(c1.tv_sec - c0.tv_sec) + double(c1.tv_nsec - c0.tv_nsec) / 1e9;
  const long rss1 = vm_kb("VmRSS:"), hwm = vm_kb("VmHWM:");
  audio->stop();
  midi->stop();
  std::vector<double> kb, mi;
  NoteTiming nt;
  while (g.core.pop_note_timing(nt)) {
    const double ms = double(nt.dac_ns - nt.received_ns) / 1e6;
    if (nt.src == Source::Keyboard) kb.push_back(ms);
    else if (nt.src == Source::Midi) mi.push_back(ms);
  }
  const AudioStats s = audio->stats();
  auto js = [](std::vector<double>& v) {
    if (v.empty()) return std::string("null");
    const Stat st = stat_of(v);
    char b[200];
    std::snprintf(b, sizeof b, "{\"n\":%zu,\"p50\":%.3f,\"p99\":%.3f,\"max\":%.3f}", v.size(), st.p50, st.p99, st.max);
    return std::string(b);
  };
  char out[2000];
  std::snprintf(out, sizeof out,
                "{\"device\":\"%s\",\"period\":%d,\"fill\":%d,\"worst\":%s,\"seconds\":%.1f,\"periods\":%llu,\"xruns\":%llu,"
                "\"out_latency_ms\":%.3f,\"round_trip_ms\":%.3f,\"dsp_us\":{\"p50\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
                "\"dsp_p99_pct_period\":%.3f,\"key_to_dac_ms\":%s,\"midi_to_dac_ms\":%s,"
                "\"process_cpu_pct_of_one_core\":%.2f,\"rss_kb_start\":%ld,\"rss_kb_end\":%ld,\"hwm_kb\":%ld,"
                "\"tape_playing\":%d,\"tape_clips\":%d,\"synth_voices\":%u,\"drum_voices\":%u}",
                s.device.c_str(), s.period, s.fill, worst ? "true" : "false", wall, (unsigned long long)s.periods,
                (unsigned long long)s.xruns, s.out_latency_ms, s.out_latency_ms + 1e3 * s.period / kSampleRate,
                s.dsp_us_p50, s.dsp_us_p99, s.dsp_us_max, 100 * s.dsp_us_p99 / (1e6 * s.period / kSampleRate),
                js(kb).c_str(), js(mi).c_str(), 100 * cpu_s / wall, rss0, rss1, hwm, tape_playing, tape_clips, sv, dv);
  std::printf("%s\n", out);
  if (!stats_path.empty())
    if (FILE* f = std::fopen(stats_path.c_str(), "w")) { std::fprintf(f, "%s\n", out); std::fclose(f); }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: measure offline [--blocks N] | live --seconds S [--worst] ...\n"); return 64; }
  dsp_init();
  if (!std::strcmp(argv[1], "offline")) {
    int blocks = 20000;
    for (int i = 2; i + 1 < argc; ++i)
      if (!std::strcmp(argv[i], "--blocks")) blocks = std::atoi(argv[i + 1]);
    return offline(blocks);
  }
  if (!std::strcmp(argv[1], "live")) return live(argc, argv);
  return 64;
}
