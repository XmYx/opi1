// Offline render tests of the engine track (t4): every synth and drum engine, every effect (mono and stereo)
// and every LFO is built through the public factories and rendered without devices. Checks:
//   - each produces sound with its type defaults (RMS above -50 dBFS while notes are held, no runaway
//     clipping) and stays bounded; integer DSP cannot produce NaN, so "NaN-free" is "bounded and not stuck"
//   - every documented parameter slot accepts its full stored range (-32768..32767): extremes stay bounded
//     and the engine keeps sounding; play modes give the documented voice counts
//   - every LFO changes the sound against the same render with the LFO off
//   - nothing allocates or frees memory while rendering, switching effects/LFOs or loading patches
// With OP1_RENDER_DIR set, one WAV per engine, effect and LFO is written there (16-bit stereo, 44.1 kHz).
// With OP1_DSP_ALLOW_MISSING=1 a module whose factory returns nullptr is reported and skipped (development).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "check.h"
#include "op1/dsp.h"
#include "../../src/dsp/dsp_internal.h"

// ---- allocation counter (global operator new/delete for this test binary)
static std::atomic<bool> g_counting{false};
static std::atomic<long> g_allocs{0}, g_frees{0};
static void* counted_alloc(size_t n, size_t align) {
  if (g_counting) ++g_allocs;
  void* p = align > alignof(std::max_align_t) ? std::aligned_alloc(align, (n + align - 1) / align * align)
                                               : std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
static void counted_free(void* p) {
  if (p && g_counting) ++g_frees;
  std::free(p);
}
void* operator new(size_t n) { return counted_alloc(n, 0); }
void* operator new[](size_t n) { return counted_alloc(n, 0); }
void* operator new(size_t n, std::align_val_t a) { return counted_alloc(n, size_t(a)); }
void* operator new[](size_t n, std::align_val_t a) { return counted_alloc(n, size_t(a)); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, size_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { counted_free(p); }

using namespace op1;

namespace {

bool g_allow_missing = false;
const char* g_render_dir = nullptr;

const char* kInstNames[] = {"fm", "digital", "drwave", "string", "cluster", "pulse", "sampler", "phase",
                            "drum", "dbox", "ls", "dna", "dsynth", "voltage", "iter"};
const char* kFxNames[] = {"delay", "grid", "punch", "phone", "spring", "filter", "nitro", "cwo"};
const char* kLfoNames[] = {"tremolo", "value", "random", "element", "midi", "bend", "crank"};

constexpr double kFs = 2147483648.0;

struct Stats {
  double rms = 0, peak = 0, clipped = 0;
};
Stats stats(const std::vector<int32_t>& x, size_t a, size_t b) {
  Stats s;
  b = std::min(b, x.size());
  if (b <= a) return s;
  double sum = 0;
  long clip = 0;
  for (size_t i = a; i < b; ++i) {
    const double v = x[i] / kFs;
    sum += v * v;
    s.peak = std::max(s.peak, std::fabs(v));
    if (x[i] >= INT32_MAX - 1 || x[i] <= INT32_MIN + 1) ++clip;
  }
  s.rms = std::sqrt(sum / double(b - a));
  s.clipped = double(clip) / double(b - a);
  return s;
}
double db(double v) { return v > 0 ? 20 * std::log10(v) : -999; }

// Renders an instrument on the 64-frame grid like the core does, into preallocated buffers.
struct Rig {
  Instrument* inst = nullptr;
  ControlInputs ctl;
  int64_t frame = 0;
  std::vector<int32_t> l, r;
  size_t pos = 0;
  int32_t zero[kBlock] = {};
  explicit Rig(size_t cap) : l(cap, 0), r(cap, 0) {}
  void run(int frames) {
    while (frames > 0) {
      const int n = std::min<int>(frames, kBlock - int(frame % kBlock));
      ProcessContext ctx;
      ctx.frame = frame;
      ctx.frames = n;
      ctx.block_start = frame % kBlock == 0;
      ctx.bpm_tenths = 1200;
      ctx.in_l = zero;
      ctx.in_r = zero;
      ctx.controls = &ctl;
      int32_t bl[kBlock], br[kBlock];
      inst->process(ctx, bl, br);
      for (int i = 0; i < n && pos < l.size(); ++i, ++pos) {
        l[pos] = bl[i];
        r[pos] = br[i];
      }
      frame += n;
      frames -= n;
    }
  }
  void rewind() { pos = 0; }
};

Command cmd(InstrumentOp op, int index, int value, void* ptr = nullptr, int slot = 0) {
  Command c;
  c.domain = Domain::Synth;
  c.op = uint16_t(op);
  c.index = index;
  c.value = value;
  c.ptr = ptr;
  c.slot = uint8_t(slot);
  return c;
}

void write_wav(const std::string& name, const std::vector<int32_t>& l, const std::vector<int32_t>& r, size_t n) {
  if (!g_render_dir) return;
  const std::string path = std::string(g_render_dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return;
  }
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  const uint32_t data = uint32_t(n * 4);
  std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(2); u32(kSampleRate); u32(kSampleRate * 4); u16(4); u16(16);
  std::fwrite("data", 1, 4, f); u32(data);
  for (size_t i = 0; i < n; ++i) {
    const int16_t a = int16_t(l[i] >> 16), b = int16_t(r[i] >> 16);
    std::fwrite(&a, 2, 1, f);
    std::fwrite(&b, 2, 1, f);
  }
  std::fclose(f);
}

// ---- user-style audio made here (no Teenage Engineering samples anywhere in the tree)
std::vector<int16_t> g_kit, g_tone;
SampleData g_kit_data, g_tone_data;
DrumPatch g_drum_patch;
DrumPatch g_dbox_patch;
SynthPatch g_sampler_patch;

void make_user_audio() {
  // a 12 s "kit": 24 slices of 0.5 s, each a decaying sine (rising pitch) with some noise
  const int slice = kSampleRate / 2;
  g_kit.assign(size_t(slice) * kDrumKeys, 0);
  uint32_t seed = 1;
  for (int k = 0; k < kDrumKeys; ++k) {
    const double f = 50.0 * std::pow(2.0, k / 6.0), noise = (k % 3) * 0.35;
    double ph = 0;
    for (int i = 0; i < slice; ++i) {
      const double env = std::exp(-i / (kSampleRate * (0.05 + 0.02 * (k % 4))));
      seed = seed * 1664525u + 1013904223u;
      const double nz = int32_t(seed) / kFs;
      ph += f * (1.0 + 2.0 * env) / kSampleRate;
      g_kit[size_t(k) * slice + i] = int16_t(20000.0 * env * ((1 - noise) * std::sin(6.2831853 * ph) + noise * nz));
    }
  }
  g_kit_data.frames = g_kit.data();
  g_kit_data.length = int32_t(g_kit.size());
  for (int k = 0; k < kDrumKeys; ++k) {
    // sample index x 4058 (og-behaviour-details.md §4); the last slice's end exceeds int32 and is clamped
    g_drum_patch.start[k] = int32_t(std::min<int64_t>(int64_t(k) * slice * 4058, INT32_MAX));
    g_drum_patch.end[k] = int32_t(std::min<int64_t>((int64_t(k + 1) * slice - 1) * 4058, INT32_MAX));
    g_drum_patch.volume[k] = 8192;
  }
  g_drum_patch.type = InstrumentType::Drum;
  g_drum_patch.drum_version = 2;
  // a 2 s tone at the sampler's default root (C4, 261.63 Hz): a soft saw with a slow decay
  g_tone.assign(size_t(kSampleRate) * 2, 0);
  double ph = 0;
  for (size_t i = 0; i < g_tone.size(); ++i) {
    ph += 261.63 / kSampleRate;
    ph -= std::floor(ph);
    const double env = std::exp(-double(i) / (kSampleRate * 1.5));
    g_tone[i] = int16_t(16000.0 * env * (2 * ph - 1) * 0.8);
  }
  g_tone_data.frames = g_tone.data();
  g_tone_data.length = int32_t(g_tone.size());
  g_sampler_patch.type = InstrumentType::Sampler;
  std::memcpy(g_sampler_patch.knobs, dsp::default_knobs(InstrumentType::Sampler), sizeof g_sampler_patch.knobs);
  std::memcpy(g_sampler_patch.adsr, dsp::kDefaultAdsr, sizeof g_sampler_patch.adsr);
  g_sampler_patch.base_freq = 261.63f;
  // a dbox kit with 24 keys of explicit parameters (pitch rising across the keys)
  g_dbox_patch.type = InstrumentType::Dbox;
  for (int k = 0; k < kDrumKeys; ++k) {
    const Param* d = dsp::default_knobs(InstrumentType::Dbox);
    for (int p = 0; p < kParams; ++p) g_dbox_patch.dbox_params[k][p] = d[p];
    g_dbox_patch.dbox_params[k][0] = Param(k * 1300);
  }
  g_dbox_patch.dbox_keys = kDrumKeys;
}

// Notes used for each instrument: the drum keys start at note 53 (keyboard index 0, og-behaviour-details §2.1).
void play_phrase(Rig& rig, bool drum, double seconds_held) {
  if (drum) {
    const int notes[] = {53, 57, 55, 60, 53, 62, 64, 58};
    const int step = int(seconds_held * kSampleRate / 8);
    for (int n : notes) {
      rig.inst->note_on(n, 100, -1);
      rig.run(step / 2);
      rig.inst->note_off(n, -1);
      rig.run(step - step / 2);
    }
  } else {
    rig.inst->note_on(60, 100, -1);
    rig.run(int(0.15 * kSampleRate));
    rig.inst->note_on(64, 100, 10);  // with a frame offset inside the chunk
    rig.run(int(0.15 * kSampleRate));
    rig.inst->note_on(67, 100, -1);
    rig.run(int((seconds_held - 0.3) * kSampleRate));
    for (int n : {60, 64, 67}) rig.inst->note_off(n, -1);
  }
}

alignas(64) std::byte* g_mem = nullptr;     // instrument storage (4 MB)
alignas(64) std::byte* g_fxmem = nullptr;   // effect storage (2 MB)

Instrument* build(InstrumentType t) {
  std::memset(g_mem, 0, kInstrumentStorage);
  Instrument* inst = create_instrument(t, g_mem, kInstrumentStorage);
  if (!inst) return nullptr;
  if (t == InstrumentType::Drum) {
    inst->command(cmd(InstrumentOp::SetSample, 0, 0, &g_kit_data));
    inst->command(cmd(InstrumentOp::LoadDrumPatch, 0, 0, &g_drum_patch));
  } else if (t == InstrumentType::Sampler) {
    inst->command(cmd(InstrumentOp::SetSample, 0, 0, &g_tone_data));
    inst->command(cmd(InstrumentOp::LoadSynthPatch, 0, 0, &g_sampler_patch));
  }
  return inst;
}

// ---------------------------------------------------------------- engines
void test_engines(Rig& rig) {
  for (int ti = 0; ti < int(InstrumentType::Count); ++ti) {
    const auto t = InstrumentType(ti);
    if (t == InstrumentType::Ls) {
      // open: the factory refuses it (firmware-behaviour.md §11), and the core keeps the old engine
      CHECK(create_instrument(t, g_mem, kInstrumentStorage) == nullptr);
      continue;
    }
    g_counting = true;
    Instrument* inst = build(t);
    g_counting = false;
    if (!inst) {
      std::fprintf(stderr, "engine %s: factory returned nullptr%s\n", kInstNames[ti], g_allow_missing ? " (skipped)" : "");
      if (!g_allow_missing) CHECK(inst != nullptr);
      continue;
    }
    CHECK(inst->type() == t);
    const bool drum = is_drum(t);
    rig.inst = inst;

    // 1. type defaults: a phrase, held, then released
    g_counting = true;
    rig.rewind();
    play_phrase(rig, drum, 1.2);
    const size_t held_end = rig.pos;
    rig.run(int(0.8 * kSampleRate));
    g_counting = false;
    const Stats s = stats(rig.l, size_t(0.05 * kSampleRate), held_end);
    std::printf("engine %-8s default rms %6.1f dBFS peak %5.2f clipped %.4f\n", kInstNames[ti], db(s.rms), s.peak, s.clipped);
    CHECK(s.rms > 0.00316);       // > -50 dBFS
    CHECK(s.clipped < 0.01);
    for (size_t i = 0; i < rig.pos; ++i)
      if (rig.l[i] != rig.r[i]) { CHECK(rig.l[i] == rig.r[i]); break; }  // OG instruments are mono
    write_wav(std::string("engine_") + kInstNames[ti], rig.l, rig.r, rig.pos);

    // 2. every engine slot over its full stored range
    const Param* def = dsp::default_knobs(t);
    for (int k = 0; k < kParams; ++k) {
      for (int v : {-32768, 0, 32767}) {
        g_counting = true;
        inst->command(cmd(InstrumentOp::SetKnob, k, v));
        rig.rewind();
        play_phrase(rig, drum, 0.4);
        const size_t e = rig.pos;
        rig.run(int(0.05 * kSampleRate));
        g_counting = false;
        const Stats x = stats(rig.l, 0, e);
        if ((!(x.rms > 1e-5) && !(t == InstrumentType::Sampler && k == 7 && v <= 0)) || x.clipped >= 0.05)
          std::fprintf(stderr, "engine %s knob %d = %d: rms %.1f dB clipped %.3f\n", kInstNames[ti], k, v, db(x.rms), x.clipped);
        // the sampler's gain is 16 (k7 / 32768)^2 in the firmware (t3's decode of the sampler screen): 0 mutes it
        const bool muted_by_design = t == InstrumentType::Sampler && k == 7 && v <= 0;
        CHECK(x.rms > 1e-5 || muted_by_design);  // still sounding (> -100 dBFS)
        CHECK(x.clipped < 0.05);  // and not stuck at the rails
      }
      inst->command(cmd(InstrumentOp::SetKnob, k, def[k]));
    }
    inst->all_notes_off();
    rig.run(int(0.5 * kSampleRate));

    // 3. stored values out of range are clamped: the view shows what was set, octave stays in -4..+4
    inst->command(cmd(InstrumentOp::SetOctave, 0, 9));
    InstrumentView view;
    inst->view(view);
    CHECK_EQ(view.octave, 4);
    inst->command(cmd(InstrumentOp::SetOctave, 0, 0));

    if (!drum) {
      // 4. play modes: poly / mono / legato / unison voice counts (adsr slot 4 bands of 4096)
      const int expect[4] = {3, 1, 1, 6};
      const int band[4] = {2048, 6144, 10240, 14336};
      for (int m = 0; m < 4; ++m) {
        g_counting = true;
        inst->command(cmd(InstrumentOp::SetAdsr, 4, band[m]));
        rig.rewind();
        rig.run(kBlock);  // a control tick applies the mode
        for (int n : {60, 64, 67}) inst->note_on(n, 100, -1);
        rig.run(4 * kBlock);
        inst->view(view);
        g_counting = false;
        int count = 0;
        for (int i = 0; i < kSynthVoices; ++i) count += (view.voices_active >> i) & 1;
        if (count != expect[m]) std::fprintf(stderr, "engine %s play mode %d: %d voices\n", kInstNames[ti], m, count);
        CHECK_EQ(count, expect[m]);
        CHECK_EQ(view.display[12], m);
        inst->all_notes_off();
        rig.run(int(0.6 * kSampleRate));
      }
      // portamento on (legato): glide flag in the view, still bounded
      inst->command(cmd(InstrumentOp::SetAdsr, 4, 10240));
      inst->command(cmd(InstrumentOp::SetAdsr, 5, 9000));
      rig.rewind();
      rig.run(kBlock);
      inst->note_on(48, 100, -1);
      rig.run(int(0.2 * kSampleRate));
      inst->note_on(72, 100, -1);
      rig.run(int(0.3 * kSampleRate));
      inst->view(view);
      CHECK_EQ(view.display[13], 1);
      inst->all_notes_off();
      inst->command(cmd(InstrumentOp::SetAdsr, 4, 2048));
      inst->command(cmd(InstrumentOp::SetAdsr, 5, 64));
      // envelope extremes: A/D/R 0..32767 and S 0..32767 stay bounded; S = 0 with a short decay goes quiet
      for (int v : {0, 16384, 32767}) {
        for (int a = 0; a < 4; ++a) inst->command(cmd(InstrumentOp::SetAdsr, a, v));
        rig.rewind();
        rig.run(kBlock);
        inst->note_on(60, 100, -1);
        rig.run(int(0.3 * kSampleRate));
        inst->note_off(60, -1);
        rig.run(int(0.1 * kSampleRate));
        CHECK(stats(rig.l, 0, rig.pos).clipped < 0.05);
      }
      for (int a = 0; a < 4; ++a) inst->command(cmd(InstrumentOp::SetAdsr, a, a == 2 ? 0 : 64));
      rig.rewind();
      rig.run(kBlock);
      inst->note_on(60, 100, -1);
      rig.run(int(0.5 * kSampleRate));
      const Stats tail = stats(rig.l, size_t(0.4 * kSampleRate), rig.pos);
      CHECK(tail.rms < 0.001);  // decayed to sustain 0 while held
      inst->all_notes_off();
    }
    inst->~Instrument();
  }
}

// ---------------------------------------------------------------- effects
void test_effects() {
  const int n_in = int(0.4 * kSampleRate), n_all = int(1.6 * kSampleRate);
  std::vector<int32_t> in(size_t(n_all), 0), l(size_t(n_all), 0), r(size_t(n_all), 0);
  double ph = 0;
  for (int i = 0; i < n_in; ++i) {  // a decaying 220 Hz saw burst, the right channel a fifth up
    ph += 220.0 / kSampleRate;
    ph -= std::floor(ph);
    in[size_t(i)] = int32_t((2 * ph - 1) * 0.5 * std::exp(-i / (0.3 * kSampleRate)) * kFs);
  }
  for (int ti = 0; ti < int(FxType::Count); ++ti) {
    for (bool stereo : {false, true}) {
      std::memset(g_fxmem, 0, kEffectStorage);
      g_counting = true;
      Effect* fx = create_effect(FxType(ti), stereo, g_fxmem, stereo ? kEffectStorage : dsp::kMonoFxStorage);
      g_counting = false;
      if (!fx) {
        std::fprintf(stderr, "fx %s (%s): factory returned nullptr%s\n", kFxNames[ti], stereo ? "stereo" : "mono",
                     g_allow_missing ? " (skipped)" : "");
        if (!g_allow_missing) CHECK(fx != nullptr);
        continue;
      }
      auto run = [&](int frames) {
        int64_t frame = 0;
        while (frame < frames) {
          const int n = std::min<int>(frames - int(frame), kBlock);
          for (int i = 0; i < n; ++i) {
            l[size_t(frame + i)] = in[size_t(frame + i)];
            r[size_t(frame + i)] = in[size_t(frame + i)] / 2;
          }
          ProcessContext ctx;
          ctx.frame = frame;
          ctx.frames = n;
          ctx.block_start = true;
          fx->process(ctx, l.data() + frame, stereo ? r.data() + frame : nullptr);
          frame += n;
        }
      };
      const Param* def = dsp::default_fx_params(FxType(ti));
      for (int k = 0; k < kParams; ++k) fx->set_param(k, def[k]);
      fx->reset();
      g_counting = true;
      run(n_all);
      g_counting = false;
      const Stats s = stats(l, 0, size_t(n_in));
      double diff = 0;
      for (int i = 0; i < n_all; ++i) diff += std::fabs((double(l[size_t(i)]) - in[size_t(i)]) / kFs);
      diff /= n_all;
      const Stats tail = stats(l, size_t(n_in + kSampleRate / 10), size_t(n_all));
      std::printf("fx %-6s %-6s rms %6.1f dBFS tail %6.1f dBFS change %.4f clipped %.4f\n", kFxNames[ti],
                  stereo ? "stereo" : "mono", db(s.rms), db(tail.rms), diff, s.clipped);
      CHECK(s.rms > 0.001);       // passes sound (> -60 dBFS)
      CHECK(diff > 1e-4);         // and changes it
      CHECK(s.clipped < 0.01);
      if (stereo) {
        CHECK(stats(r, 0, size_t(n_in)).rms > 0.001);
        if (ti == int(FxType::Delay))  // master delay leaves the right channel its own line
          CHECK(stats(r, size_t(n_in), size_t(n_all)).rms > 0);
        std::vector<int32_t> wl(l), wr(r);
        if (g_render_dir) write_wav(std::string("fx_") + kFxNames[ti], wl, wr, size_t(n_all));
      }
      if (ti == int(FxType::Delay) || ti == int(FxType::Spring) || ti == int(FxType::Grid) || ti == int(FxType::Cwo))
        CHECK(tail.rms > 1e-5);   // echoes / reverb tail after the input stops
      // full stored range of the four encoder slots
      for (int k = 0; k < 4; ++k) {
        for (int v : {-32768, 0, 32767}) {
          g_counting = true;
          fx->set_param(k, Param(v));
          run(int(0.3 * kSampleRate));
          g_counting = false;
          const Stats x = stats(l, 0, size_t(0.3 * kSampleRate));
          if (x.clipped >= 0.05) std::fprintf(stderr, "fx %s p%d=%d clipped %.3f\n", kFxNames[ti], k, v, x.clipped);
          CHECK(x.clipped < 0.05);
        }
        fx->set_param(k, def[k]);
      }
      int32_t d[16];
      fx->view(d);
      CHECK_EQ(d[0], def[0]);
      fx->~Effect();
    }
  }
}

// ---------------------------------------------------------------- LFOs
// Settings with a clear effect for each LFO, and the stimulus each one reacts to.
struct LfoCase {
  Param p[kParams];
};
const LfoCase kLfoCases[int(LfoType::Count)] = {
    {{20000, 32767, 32767, 0, 0, 0, 0, 0}},  // tremolo: fast, full pitch and volume depth
    {{20000, 32767, 0, 0, 0, 0, 0, 0}},      // value: fast, full amount, engine param 1 (fm amount)
    {{26000, 32767, 0, 0, 0, 0, 0, 0}},      // random: fast, full amount, flat target engine 1
    {{12000, 32767, 30000, 0, 0, 0, 0, 0}},  // element: source input level, full amount, main: pitch
    {{2000, 0, 0, 0, 0, 0, 0, 0}},           // midi: CC 1 -> engine 1
    {{0, 0, 32767, 16000, 0, 0, 0, 2048}},   // bend: engine 1, full amount; the bend encoder moves
    {{0, 32767, 0, 0, 0, 0, 0, 0}},          // crank: the crank encoder spins, full amount, engine 1
};

void lfo_render(Rig& rig, LfoType t, bool on) {
  Instrument* inst = rig.inst;
  inst->command(cmd(InstrumentOp::SetLfoType, 0, int(t)));
  for (int k = 0; k < kParams; ++k) inst->command(cmd(InstrumentOp::SetLfoParam, k, kLfoCases[int(t)].p[k]));
  inst->command(cmd(InstrumentOp::SetLfoOn, 0, on ? 1 : 0));
  rig.ctl = ControlInputs{};
  rig.rewind();
  inst->note_on(57, 100, -1);
  const int blocks = int(1.5 * kSampleRate) / kBlock;
  for (int b = 0; b < blocks; ++b) {
    // stimuli: CC 1 sweeps, the input level and accelerometer pulse, the bend/crank encoders turn
    rig.ctl.cc[1] = int16_t((b * 3) % 128);
    rig.ctl.input_level = (b / 40) % 2 ? (1 << 30) : 0;
    rig.ctl.accel[0] = int16_t((b * 97) % 30000);
    if (t == LfoType::Bend && b % 50 == 0)
      inst->command(cmd(InstrumentOp::SetLfoParam, 3, (b * 900) % 32767));
    if (t == LfoType::Crank && b < 300)
      inst->command(cmd(InstrumentOp::SetLfoParam, 0, (b * 700) % 32767));
    rig.run(kBlock);
  }
  inst->note_off(57, -1);
}

void test_lfos(Rig& rig) {
  for (int ti = 0; ti < int(LfoType::Count); ++ti) {
    const auto t = LfoType(ti);
    alignas(64) static std::byte lfomem[dsp::kLfoStorage];
    dsp::Lfo* probe = dsp::create_lfo(t, lfomem, sizeof lfomem);
    if (!probe) {
      std::fprintf(stderr, "lfo %s: factory returned nullptr%s\n", kLfoNames[ti], g_allow_missing ? " (skipped)" : "");
      if (!g_allow_missing) CHECK(probe != nullptr);
      continue;
    }
    probe->~Lfo();
    Instrument* inst = build(InstrumentType::Fm);
    CHECK(inst != nullptr);
    if (!inst) return;
    rig.inst = inst;
    std::vector<int32_t> off;
    g_counting = true;
    lfo_render(rig, t, false);
    g_counting = false;
    off.assign(rig.l.begin(), rig.l.begin() + long(rig.pos));
    inst->all_notes_off();
    rig.run(int(0.5 * kSampleRate));
    g_counting = true;
    lfo_render(rig, t, true);
    g_counting = false;
    double diff = 0;
    for (size_t i = 0; i < off.size(); ++i) diff += std::fabs((double(rig.l[i]) - off[i]) / kFs);
    diff /= double(off.size());
    const Stats s = stats(rig.l, 0, rig.pos);
    std::printf("lfo %-8s change %.4f rms %6.1f dBFS\n", kLfoNames[ti], diff, db(s.rms));
    CHECK(diff > 1e-3);
    CHECK(s.clipped < 0.01);
    write_wav(std::string("lfo_") + kLfoNames[ti], rig.l, rig.r, rig.pos);
    // full stored range of all eight LFO slots stays bounded
    for (int k = 0; k < kParams; ++k)
      for (int v : {-32768, 32767}) {
        g_counting = true;
        inst->command(cmd(InstrumentOp::SetLfoParam, k, v));
        rig.rewind();
        inst->note_on(60, 100, -1);
        rig.run(int(0.2 * kSampleRate));
        inst->note_off(60, -1);
        g_counting = false;
        CHECK(stats(rig.l, 0, rig.pos).clipped < 0.05);
      }
    InstrumentView view;
    inst->view(view);
    CHECK(view.lfo_type == t);
    CHECK(view.lfo_on);
    inst->~Instrument();
  }
}

// Engine and effect switches on a live instrument, as the core does on the audio thread: no allocation.
void test_switching(Rig& rig) {
  Instrument* inst = build(InstrumentType::Fm);
  if (!inst) return;
  rig.inst = inst;
  g_counting = true;
  inst->note_on(60, 100, -1);
  for (int f = 0; f < int(FxType::Count); ++f) {
    inst->command(cmd(InstrumentOp::SetFxType, 0, f));
    inst->command(cmd(InstrumentOp::SetFxOn, 0, 1));
    rig.rewind();
    rig.run(int(0.1 * kSampleRate));
  }
  for (int t = 0; t < int(LfoType::Count); ++t) {
    inst->command(cmd(InstrumentOp::SetLfoType, 0, t));
    inst->command(cmd(InstrumentOp::SetLfoOn, 0, 1));
    rig.rewind();
    rig.run(int(0.1 * kSampleRate));
  }
  inst->~Instrument();
  for (int t = 0; t < int(InstrumentType::Count); ++t) {  // re-create every engine in the same storage
    Instrument* x = create_instrument(InstrumentType(t), g_mem, kInstrumentStorage);
    if (x) x->~Instrument();
  }
  g_counting = false;
}


// CPU cost per engine with every voice sounding (6 synth voices held, or drum hits), measured per 64-frame
// block against the block's real-time budget (1451 us). Printed for the report; checked only against a
// generous bound (an engine must not need more than half the budget on its own).
void bench_engines(Rig& rig) {
  using clk = std::chrono::steady_clock;
  const double budget_us = 1e6 * kBlock / kSampleRate;
  for (int ti = 0; ti < int(InstrumentType::Count); ++ti) {
    const auto t = InstrumentType(ti);
    if (t == InstrumentType::Ls) continue;
    // engine switch cost: the core destroys and re-creates the instrument on the audio thread
    double make_us = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
      const auto m0 = clk::now();
      Instrument* x = create_instrument(t, g_mem, kInstrumentStorage);
      make_us = std::min(make_us, std::chrono::duration<double, std::micro>(clk::now() - m0).count());
      if (x) x->~Instrument();
    }
    Instrument* inst = build(t);
    if (!inst) continue;
    rig.inst = inst;
    rig.rewind();
    const bool drum = is_drum(t);
    if (!drum) for (int n : {48, 55, 60, 64, 67, 72}) inst->note_on(n, 100, -1);
    const int blocks = int(2.0 * kSampleRate) / kBlock;
    double sum = 0, worst = 0;
    std::vector<double> each(size_t(blocks), 0.0);
    for (int b = 0; b < blocks; ++b) {
      if (drum && b % 16 == 0) for (int k = 0; k < 4; ++k) inst->note_on(53 + (b / 16 * 3 + k * 5) % 24, 100, -1);
      if (rig.pos + kBlock > rig.l.size()) rig.rewind();
      const auto t0 = clk::now();
      rig.run(kBlock);
      const double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
      each[size_t(b)] = us;
      sum += us;
      worst = std::max(worst, us);
    }
    std::sort(each.begin(), each.end());
    const double avg = sum / blocks, p99 = each[size_t(blocks * 99 / 100)];
    std::printf("cpu %-8s avg %6.1f us/block (%5.2f %% of %.0f us)  p99 %6.1f us  max %6.1f us  switch %6.1f us\n",
                kInstNames[ti], avg, 100 * avg / budget_us, budget_us, p99, worst, make_us);
    CHECK(avg < budget_us / 2);
    CHECK(make_us < budget_us / 2);  // an engine switch fits in one block
    inst->all_notes_off();
    inst->~Instrument();
  }
}

}  // namespace

int main() {
  g_allow_missing = std::getenv("OP1_DSP_ALLOW_MISSING") != nullptr;
  g_render_dir = std::getenv("OP1_RENDER_DIR");
  dsp_init();
  g_mem = static_cast<std::byte*>(::operator new(kInstrumentStorage, std::align_val_t(64)));
  g_fxmem = static_cast<std::byte*>(::operator new(kEffectStorage, std::align_val_t(64)));
  std::memset(g_mem, 0, kInstrumentStorage);
  std::memset(g_fxmem, 0, kEffectStorage);
  make_user_audio();
  Rig rig(size_t(4 * kSampleRate));

  g_allocs = 0;
  g_frees = 0;
  test_engines(rig);
  test_effects();
  test_lfos(rig);
  test_switching(rig);
  const long allocs = g_allocs.load(), frees = g_frees.load();
  bench_engines(rig);  // outside the allocation count (it keeps timing data)
  g_allocs = allocs;
  g_frees = frees;
  std::printf("allocations while rendering: %ld, frees: %ld\n", g_allocs.load(), g_frees.load());
  CHECK_EQ(g_allocs.load(), 0);
  CHECK_EQ(g_frees.load(), 0);
  return check_result("dsp");
}
