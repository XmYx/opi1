// Factory presets and the worker (t2, 2026-10-23; PARITY-AUDIT.md §1.1: drum mode and the sampler were silent
// because nothing loaded a kit or a sample). Needs the user's firmware image (OP1_OG_FIRMWARE, as the gui tests;
// SKIPPED without one). Checks:
//   - the SQLite/JSON readers list every factory preset of op1_factory.db and every one parses;
//   - every drum kit's and sampler preset's audio is in the image, and every one of the 24 keys of every drum
//     kit sounds once the kit is loaded; every sampler preset sounds;
//   - end to end: Core + worker, as op1-og runs them: drum mode and the sampler engine play without any
//     user action.
// With OP1_PRIVATE_RENDER_DIR set, the kit/sampler renders are written there. They contain audio from the user's
// image, so the directory must not be shared.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "op1/core.h"
#include "op1/dsp.h"
#include "op1/io.h"
#include "op1/ui.h"
#include "../../src/state/factory.h"

using namespace op1;
using op1::state::FactoryLibrary;

namespace {

constexpr double kFs = 2147483648.0;
const char* g_dir = nullptr;

double rms_db(const std::vector<int32_t>& x, size_t a, size_t b) {
  b = std::min(b, x.size());
  if (b <= a) return -999;
  double s = 0;
  for (size_t i = a; i < b; ++i) s += (x[i] / kFs) * (x[i] / kFs);
  const double r = std::sqrt(s / double(b - a));
  return r > 0 ? 20 * std::log10(r) : -999;
}

void write_wav(const std::string& name, const std::vector<int32_t>& x) {
  if (!g_dir) return;
  FILE* f = std::fopen((std::string(g_dir) + "/" + name + ".wav").c_str(), "wb");
  if (!f) return;
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  const uint32_t data = uint32_t(x.size() * 2);
  std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(1); u32(kSampleRate); u32(kSampleRate * 2); u16(2); u16(16);
  std::fwrite("data", 1, 4, f); u32(data);
  for (int32_t v : x) {
    const int16_t s = int16_t(v >> 16);
    std::fwrite(&s, 2, 1, f);
  }
  std::fclose(f);
}

struct Host {
  std::byte* mem;
  Instrument* inst;
  int64_t frame = 0;
  ControlInputs ctl;
  int32_t zero[kBlock] = {};
  explicit Host(InstrumentType t) {
    mem = static_cast<std::byte*>(::operator new(kInstrumentStorage, std::align_val_t(64)));
    std::memset(mem, 0, kInstrumentStorage);
    inst = create_instrument(t, mem, kInstrumentStorage);
  }
  ~Host() {
    if (inst) inst->~Instrument();
    ::operator delete(mem, std::align_val_t(64));
  }
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;
  void send(Domain d, InstrumentOp op, void* ptr) {
    Command c;
    c.domain = d;
    c.op = uint16_t(op);
    c.ptr = ptr;
    inst->command(c);
  }
  void run(int frames, std::vector<int32_t>& out) {
    while (frames > 0) {
      const int n = std::min<int>(frames, kBlock - int(frame % kBlock));
      ProcessContext ctx;
      ctx.frame = frame;
      ctx.frames = n;
      ctx.block_start = frame % kBlock == 0;
      ctx.in_l = zero;
      ctx.in_r = zero;
      ctx.controls = &ctl;
      int32_t l[kBlock], r[kBlock];
      inst->process(ctx, l, r);
      out.insert(out.end(), l, l + n);
      frame += n;
      frames -= n;
    }
  }
};

void free_sample(SampleData* s) {
  if (s) delete[] s->frames;
  delete s;
}

void test_library(const AssetProvider& assets) {
  FactoryLibrary lib;
  std::string err;
  CHECK(lib.open(assets, err));
  if (!err.empty()) std::fprintf(stderr, "factory: %s\n", err.c_str());
  std::printf("factory presets: %zu synth, %zu drum\n", lib.synths().size(), lib.drums().size());
  CHECK_EQ(long(lib.synths().size()), 169L);  // R.00246: 16 per engine x 10 + 9 sampler
  CHECK_EQ(long(lib.drums().size()), 24L);    // 14 drum kits + 10 dbox kits
  std::map<std::string, int> types;
  for (const auto& e : lib.synths()) {
    SynthPatch p;
    const bool ok = FactoryLibrary::parse_synth(e.json, p, err);
    if (!ok) std::fprintf(stderr, "synth preset %d %s: %s\n", e.id, e.name.c_str(), err.c_str());
    CHECK(ok);
    types[e.type]++;
  }
  for (const auto& [t, n] : types) std::printf("  %-8s %d\n", t.c_str(), n);

  // every drum kit: all 24 keys sound
  int kits = 0;
  for (const auto& e : lib.drums()) {
    DrumPatch* p = new DrumPatch;
    CHECK(FactoryLibrary::parse_drum(e.json, *p, err));
    Host h(p->type);
    SampleData* s = nullptr;
    if (p->type == InstrumentType::Drum) {
      s = FactoryLibrary::load_raw(assets, "factory_drum/" + e.name + ".raw", 0);
      CHECK(s != nullptr && s->length > kSampleRate);
      h.send(Domain::Drum, InstrumentOp::SetSample, s);
    }
    h.send(Domain::Drum, InstrumentOp::LoadDrumPatch, p);
    std::vector<int32_t> x;
    int silent = 0;
    double worst = 0;
    for (int k = 0; k < kDrumKeys; ++k) {
      const size_t a = x.size();
      h.inst->note_on(53 + k, 100, -1);
      h.run(int(0.25 * kSampleRate), x);
      h.inst->note_off(53 + k, -1);
      h.run(int(0.05 * kSampleRate), x);
      const double d = rms_db(x, a, x.size());
      if (d < -60) {
        ++silent;
        std::printf("  %s key %d: %.1f dBFS (start %d end %d pitch %d volume %d playmode %d reverse %d)\n", e.name.c_str(), k,
                    d, p->start[k] / 4058, p->end[k] / 4058, p->pitch[k], p->volume[k], p->playmode16[k], p->reverse16[k]);
      }
      worst = k == 0 ? d : std::min(worst, d);
    }
    // A key the kit's own effect removes is an effect question (t5), not a loading one: count the keys that
    // stay silent with the effect off. (R.00246: "awa beef" keys 6/19 and "acetate" keys 7/8 go below -60 dBFS
    // through nitro with fx param 0 = 64 in this port; dry they play at -28..-37 dBFS.)
    int silent_dry = silent;
    if (silent && p->fx_active) {
      DrumPatch dry = *p;
      dry.fx_active = false;
      h.send(Domain::Drum, InstrumentOp::LoadDrumPatch, &dry);
      h.run(kSampleRate / 2, x);
      silent_dry = 0;
      for (int k = 0; k < kDrumKeys; ++k) {
        std::vector<int32_t> y;
        h.inst->note_on(53 + k, 100, -1);
        h.run(int(0.25 * kSampleRate), y);
        h.inst->note_off(53 + k, -1);
        h.run(int(0.05 * kSampleRate), y);
        if (rms_db(y, 0, y.size()) < -60) {
          ++silent_dry;
          std::printf("  %s key %d silent even without its effect\n", e.name.c_str(), k);
        }
      }
      if (silent_dry == 0) std::printf("  %s: those keys sound with the kit's effect off (FX parity, t5)\n", e.name.c_str());
    }
    std::printf("kit %-13s %-4s rms %6.1f dBFS, quietest key %6.1f dBFS, silent keys %d\n", e.name.c_str(),
                p->type == InstrumentType::Drum ? "drum" : "dbox", rms_db(x, 0, x.size()), worst, silent);
    write_wav("kit_" + e.name, x);
    // dbox: the two factory kits with all-negative pitch (sub club, vox bass) are t5's open item
    // (PARITY-AUDIT.md §4.2 dbox pitch): only the sample kits are held to "every key sounds" here.
    if (p->type == InstrumentType::Drum) {
      CHECK_EQ(silent_dry, 0);
      ++kits;
    }
    h.inst->~Instrument();  // before the sample it points at is freed
    h.inst = nullptr;
    free_sample(s);
    delete p;
  }
  CHECK_EQ(kits, 14);

  // every sampler preset sounds
  int samplers = 0;
  for (const auto& e : lib.synths()) {
    if (e.type != "sampler") continue;
    SynthPatch* p = new SynthPatch;
    CHECK(FactoryLibrary::parse_synth(e.json, *p, err));
    SampleData* s = FactoryLibrary::load_raw(assets, "factory_synth/" + e.name + ".raw", p->base_freq);
    CHECK(s != nullptr && s->length > 1000);
    Host h(InstrumentType::Sampler);
    h.send(Domain::Synth, InstrumentOp::LoadSynthPatch, p);
    h.send(Domain::Synth, InstrumentOp::SetSample, s);
    std::vector<int32_t> x;
    h.run(2 * kBlock, x);  // the patch takes effect on the next control block (play mode), as in the app
    x.clear();
    // the preset's own octave shifts the keyboard; play its root
    const int note = 60 - 12 * p->octave;
    h.inst->note_on(note, 100, -1);
    h.run(kSampleRate, x);
    h.inst->note_off(note, -1);
    h.run(kSampleRate / 2, x);
    // loudest 50 ms: some presets are one-shot hits (designadrum: decay 1088, sustain 0)
    double d = -999;
    for (size_t w = 0; w + 2205 <= size_t(kSampleRate); w += 1102) d = std::max(d, rms_db(x, w, w + 2205));
    double peak = 0;
    for (size_t i = 0; i < size_t(kSampleRate) && i < x.size(); ++i) peak = std::max(peak, std::fabs(x[i] / kFs));
    std::printf("sampler %-12s root %7.2f Hz, %5.2f s, loudest 50 ms %6.1f dBFS, peak %6.1f dBFS\n", e.name.c_str(),
                p->base_freq, s ? double(s->length) / kSampleRate : 0.0, d, 20 * std::log10(peak + 1e-12));
    write_wav("sampler_" + e.name, x);
    CHECK(peak > 0.03);  // > -30 dBFS peak (designadrum is a ~10 ms hit: decay 1088, sustain 0)
    ++samplers;
    h.inst->~Instrument();
    h.inst = nullptr;
    free_sample(s);
    delete p;
  }
  CHECK_EQ(samplers, 9);
}

// Core + worker exactly as op1-og wires them; no device (the test calls process()).
void test_end_to_end(AssetProvider& assets) {
  Core core;
  auto worker = create_worker();
  std::string err;
  CHECK(worker->start(core, &assets, err));
  std::vector<Sample> l(kBlock), r(kBlock), out;
  auto run = [&](double sec) {
    const int blocks = int(sec * kSampleRate / kBlock);
    for (int b = 0; b < blocks; ++b) {
      core.process(nullptr, nullptr, l.data(), r.data(), kBlock);
      out.insert(out.end(), l.begin(), l.end());
      if (b % 32 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));  // let the worker post
    }
  };
  run(0.3);
  // drum mode, as after pressing 2: four keys
  Command m;
  m.domain = Domain::Core;
  m.op = uint16_t(CoreOp::SetMode);
  m.value = int32_t(Mode::Drum);
  CHECK(core.post_ui(m));
  out.clear();
  for (int k : {53, 55, 58, 60}) {
    core.post_ui(note_on(uint8_t(k), 100, Source::Keyboard, 0));
    run(0.2);
    core.post_ui(note_off(uint8_t(k), Source::Keyboard, 0));
    run(0.05);
  }
  const double drum = rms_db(out, 0, out.size());
  std::printf("end to end: drum mode %.1f dBFS\n", drum);
  CHECK(drum > -45.0);

  // synth mode, sampler engine picked (the worker handed the synth slot the first factory sample at start)
  m.value = int32_t(Mode::Synth);
  CHECK(core.post_ui(m));
  Command t;
  t.domain = Domain::Synth;
  t.op = uint16_t(InstrumentOp::SetType);
  t.value = int32_t(InstrumentType::Sampler);
  CHECK(core.post_ui(t));
  run(0.05);
  out.clear();
  core.post_ui(note_on(60, 100, Source::Keyboard, 0));
  run(0.6);
  core.post_ui(note_off(60, Source::Keyboard, 0));
  run(0.2);
  const double smp = rms_db(out, 0, out.size());
  std::printf("end to end: sampler %.1f dBFS (sample length %d frames on screen)\n", smp,
              int(core.snapshot().synth.display[16]));
  CHECK(smp > -50.0);
  CHECK(core.snapshot().synth.display[16] > 0);

  // a factory preset by request (synth preset 0 = the first row of synth_presets)
  Command req;
  req.domain = Domain::Synth;
  req.op = uint16_t(WorkerOp::LoadFactoryPreset);
  req.index = 0;
  CHECK(worker->request(req));
  run(0.2);
  out.clear();
  core.post_ui(note_on(60, 100, Source::Keyboard, 0));
  run(0.5);
  core.post_ui(note_off(60, Source::Keyboard, 0));
  run(0.1);
  std::printf("end to end: factory synth preset 0: %.1f dBFS\n", rms_db(out, 0, out.size()));
  CHECK(rms_db(out, 0, out.size()) > -50.0);
  worker->stop();
}

}  // namespace

int main() {
  g_dir = std::getenv("OP1_PRIVATE_RENDER_DIR");
  std::string fw = std::getenv("OP1_OG_FIRMWARE") ? std::getenv("OP1_OG_FIRMWARE") : "";
  if (fw.empty()) {
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const std::string def = home + "/rigs-work/op1-og-fw/fw/op1_246.op1";
    if (FILE* f = std::fopen(def.c_str(), "rb")) { std::fclose(f); fw = def; }
  }
  auto assets = create_asset_provider();
  std::string err;
  if (fw.empty() || !assets->open(fw, err)) {
    std::printf("factory: SKIPPED (no firmware image: %s)\n", err.empty() ? "set OP1_OG_FIRMWARE" : err.c_str());
    return 0;
  }
  dsp_init();
  test_library(*assets);
  test_end_to_end(*assets);
  return check_result("factory");
}
