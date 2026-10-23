// Drum and effect behaviour decoded from the firmware (t5, 2026-10-23; the facts are in the header comments of
// src/dsp/drum.cpp, dbox.cpp, fx_*.cpp and src/tape/mixer.cpp). Each check is one decoded rule:
//   drum sampler: play mode = value >> 13 (gate stops at release, one-shot plays on, loop runs while held),
//                 reverse >= 16384, gain (volume / 8192)^2, start/end as Q31 of the 12 s buffer
//   dbox:         pitch word signed (a negative pitch is lower, not the bottom of the range), 6208 = an octave (I)
//   nitro:        the cutoff index saturates at 113 (the drum kits' [64, 1024, 10500, 16448] is near transparent)
//   phone:        no noise source (silence in, silence out)
//   punch:        slot 3 = dry/wet (0 = the input unchanged), no make-up gain
//   delay:        slot 1 (speed) unsigned: a negative value acts as 33
//   master:       a type switch loads the new type's fx_types defaults
//   filter:       12 types, the name index follows slot 2 over 0..22527
//   metronome:    one click per quarter note, nothing between, nothing at level 0
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "check.h"
#include "op1/dsp.h"
#include "op1/tape.h"

using namespace op1;

namespace {

constexpr double kFs = 2147483648.0;

double rms(const std::vector<int32_t>& x, size_t a, size_t b) {
  b = std::min(b, x.size());
  if (b <= a) return 0;
  double s = 0;
  for (size_t i = a; i < b; ++i) s += (x[i] / kFs) * (x[i] / kFs);
  return std::sqrt(s / double(b - a));
}

Command cmd(Domain d, InstrumentOp op, int32_t index, int32_t value, uint8_t slot = 0, void* ptr = nullptr) {
  Command c;
  c.domain = d;
  c.op = uint16_t(op);
  c.index = index;
  c.value = value;
  c.slot = slot;
  c.ptr = ptr;
  return c;
}

struct Inst {
  std::byte* mem;
  Instrument* inst;
  int64_t frame = 0;
  explicit Inst(InstrumentType t) {
    mem = static_cast<std::byte*>(::operator new(kInstrumentStorage, std::align_val_t(64)));
    inst = create_instrument(t, mem, kInstrumentStorage);
  }
  ~Inst() {
    inst->~Instrument();
    ::operator delete(mem, std::align_val_t(64));
  }
  std::vector<int32_t> run(int frames) {
    std::vector<int32_t> out;
    Sample l[kBlock], r[kBlock];
    ControlInputs ctl;
    while (frames > 0) {
      const int n = std::min(frames, kBlock - int(frame % kBlock));
      ProcessContext ctx;
      ctx.frame = frame;
      ctx.frames = n;
      ctx.block_start = frame % kBlock == 0;
      ctx.controls = &ctl;
      inst->process(ctx, l, r);
      out.insert(out.end(), l, l + n);
      frame += n;
      frames -= n;
    }
    return out;
  }
};

// a 2 s synthetic kit: key k = frames k x 22050 .. +22049 (a 441 Hz sine) with a level rising through the slice,
// so the direction of play shows
std::vector<int16_t> g_kit;
SampleData g_sd;

void make_kit() {
  g_kit.resize(88200);
  for (size_t i = 0; i < g_kit.size(); ++i) {
    const double lvl = 0.1 + 0.8 * double(i % 22050) / 22050.0;
    g_kit[i] = int16_t(16000.0 * lvl * std::sin(2 * 3.14159265358979 * 441.0 * double(i) / 44100.0));
  }
  g_sd.frames = g_kit.data();
  g_sd.length = int32_t(g_kit.size());
}

std::unique_ptr<Inst> drum_with(int16_t pm, int16_t rev, int16_t vol) {
  auto d = std::make_unique<Inst>(InstrumentType::Drum);
  static DrumPatch p;
  p = DrumPatch{};
  p.type = InstrumentType::Drum;
  for (int k = 0; k < kDrumKeys; ++k) {
    p.start[k] = int32_t(std::min<int64_t>(int64_t(k) * 22050 * 4058, 0x7FFFFFFE));
    p.end[k] = int32_t(std::min<int64_t>((int64_t(k) * 22050 + 22049) * 4058, 0x7FFFFFFE));
    p.volume[k] = vol;
    p.playmode16[k] = pm;
    p.reverse16[k] = rev;
  }
  d->inst->command(cmd(Domain::Drum, InstrumentOp::LoadDrumPatch, 0, 0, 0, &p));
  d->inst->command(cmd(Domain::Drum, InstrumentOp::SetSample, 0, 0, 0, &g_sd));
  return d;
}

void test_drum_sampler() {
  make_kit();
  // gate: stops at release; one-shot: plays on to the slice end
  for (int16_t pm : {int16_t(4096), int16_t(8192)}) {
    auto d = drum_with(pm, 8192, 8192);
    d->inst->note_on(53, 100, -1);
    d->run(4410);
    d->inst->note_off(53, -1);
    auto out = d->run(4410);
    const double after = rms(out, 441, 4410);  // 10 ms after the release
    std::printf("drum playmode %5d: rms 10..100 ms after release %.5f\n", pm, after);
    if (pm == 4096) CHECK(after < 1e-4);
    else CHECK(after > 0.05);
  }
  // loop (16384): runs past the slice end while held, stops at release
  {
    auto d = drum_with(16384, 8192, 8192);
    d->inst->note_on(53, 100, -1);
    auto held = d->run(44100);  // 1 s > the 0.5 s slice
    CHECK(rms(held, 30000, 44100) > 0.05);
    d->inst->note_off(53, -1);
    auto out = d->run(4410);
    CHECK(rms(out, 441, 4410) < 1e-4);
  }
  // reverse (>= 16384): starts at the slice end, where the synthetic kit is loud
  {
    auto f = drum_with(8192, 8192, 8192), r = drum_with(8192, 16384, 8192);
    f->inst->note_on(53, 100, -1);
    r->inst->note_on(53, 100, -1);
    auto a = f->run(2205), b = r->run(2205);
    std::printf("drum reverse: first 50 ms rms fwd %.4f rev %.4f\n", rms(a, 64, 2205), rms(b, 64, 2205));
    CHECK(rms(b, 64, 2205) > 3 * rms(a, 64, 2205));
    auto c = drum_with(8192, 12800, 8192);  // 12800 >> 14 == 0: still forward (the port used > 12288 before)
    c->inst->note_on(53, 100, -1);
    auto e = c->run(2205);
    CHECK(std::fabs(rms(e, 64, 2205) - rms(a, 64, 2205)) < 1e-6);
  }
  // volume: (vol / 8192)^2, so 4096 = -12 dB
  {
    auto u = drum_with(8192, 8192, 8192), h = drum_with(8192, 8192, 4096);
    u->inst->note_on(53, 100, -1);
    h->inst->note_on(53, 100, -1);
    const double ru = rms(u->run(11025), 64, 11025), rh = rms(h->run(11025), 64, 11025);
    std::printf("drum volume 4096 vs 8192: %.2f dB\n", 20 * std::log10(rh / ru));
    CHECK(std::fabs(20 * std::log10(rh / ru) + 12.04) < 0.3);
  }
  // dyna_env: the envelope page's SetAdsr reaches dyna_env and the view shows it; a long attack fades the hit in
  {
    auto d = drum_with(8192, 8192, 8192);
    d->inst->command(cmd(Domain::Drum, InstrumentOp::SetAdsr, 0, 16384));
    InstrumentView v;
    d->run(64);
    d->inst->view(v);
    CHECK_EQ(v.adsr[0], 16384);
    d->inst->note_on(53, 100, -1);
    auto out = d->run(22050);
    auto plain = drum_with(8192, 8192, 8192);
    plain->inst->note_on(53, 100, -1);
    auto ref = plain->run(22050);
    CHECK(rms(out, 0, 2205) < 0.5 * rms(ref, 0, 2205));
  }
}

void test_dbox_signed_pitch() {
  auto crossings = [](int16_t pitch) {
    Inst d(InstrumentType::Dbox);
    static DrumPatch p;
    p = DrumPatch{};
    p.type = InstrumentType::Dbox;
    p.dbox_keys = 24;
    for (int k = 0; k < kDrumKeys; ++k) {
      const Param w[8] = {pitch, 0, 20000, 0, 0, 0, 0, 0};  // sine, a long decay, mode 0 without sweep, filter open
      for (int i = 0; i < 8; ++i) p.dbox_params[k][i] = w[i];
    }
    d.inst->command(cmd(Domain::Drum, InstrumentOp::LoadDrumPatch, 0, 0, 0, &p));
    d.inst->note_on(53, 100, -1);
    auto out = d.run(44100);
    int cross = 0;
    for (size_t i = 4410; i + 1 < out.size(); ++i) cross += out[i] < 0 && out[i + 1] >= 0;
    return cross;  // rising zero crossings in 0.9 s
  };
  const int c0 = crossings(0), cm = crossings(-6208), cmm = crossings(-12416);
  std::printf("dbox pitch 0 / -6208 / -12416: %d / %d / %d rising crossings in 0.9 s\n", c0, cm, cmm);
  CHECK(cm > 0 && cmm > 0);
  CHECK(std::abs(c0 - 2 * cm) <= 3);  // one octave down per 6208 (before t5 every negative value = 30 Hz)
  CHECK(std::abs(cm - 2 * cmm) <= 3);
}

struct Fx {
  std::byte* mem;
  Effect* fx;
  int64_t frame = 0;
  Fx(FxType t, bool stereo) {
    mem = static_cast<std::byte*>(::operator new(kEffectStorage, std::align_val_t(64)));
    fx = create_effect(t, stereo, mem, kEffectStorage);
    const Param* d = fx_type_defaults(t);
    for (int i = 0; i < kParams; ++i) fx->set_param(i, d[i]);
  }
  ~Fx() {
    fx->~Effect();
    ::operator delete(mem, std::align_val_t(64));
  }
  std::vector<int32_t> run(const std::vector<int32_t>& in) {
    std::vector<int32_t> out(in);
    for (size_t at = 0; at < out.size();) {
      const int n = int(std::min<size_t>(out.size() - at, size_t(kBlock - frame % kBlock)));
      ProcessContext ctx;
      ctx.frame = frame;
      ctx.frames = n;
      ctx.block_start = frame % kBlock == 0;
      fx->process(ctx, out.data() + at, nullptr);
      at += size_t(n);
      frame += n;
    }
    return out;
  }
};

std::vector<int32_t> tone(double hz, double level, int n) {
  std::vector<int32_t> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) x[size_t(i)] = int32_t(level * kFs * std::sin(2 * 3.14159265358979 * hz * i / 44100.0));
  return x;
}

void test_effects() {
  const auto in = tone(1000, 0.1, 44100);  // -20 dBFS
  const double rin = rms(in, 4410, 44100);
  {  // nitro with the drum kits' values is near transparent (before t5: a ~1 kHz low-pass)
    Fx f(FxType::Nitro, false);
    const Param p[4] = {64, 1024, 10500, 16448};
    for (int i = 0; i < 4; ++i) f.fx->set_param(i, p[i]);
    const double r = rms(f.run(in), 4410, 44100);
    std::printf("nitro [64 1024 10500 16448]: %.2f dB vs the input\n", 20 * std::log10(r / rin));
    CHECK(std::fabs(20 * std::log10(r / rin)) < 4.0);
    int32_t d[16] = {};
    f.fx->view(d);
    CHECK_EQ(d[13], 113);  // idxB saturates at 113
  }
  {  // phone: silence in, silence out
    Fx f(FxType::Phone, true);
    std::vector<int32_t> zero(44100, 0);
    const auto out = f.run(zero);
    double peak = 0;
    for (int32_t v : out) peak = std::max(peak, std::fabs(double(v)));
    CHECK(peak == 0);
  }
  {  // punch: slot 3 = 0 leaves the input as it is; the defaults add no big make-up gain
    Fx f(FxType::Punch, false);
    f.fx->set_param(3, 0);
    const auto out = f.run(in);
    CHECK(out == in);
    Fx g(FxType::Punch, false);
    const double r = rms(g.run(in), 4410, 44100);
    std::printf("punch defaults: %.2f dB vs the input\n", 20 * std::log10(r / rin));
    CHECK(20 * std::log10(r / rin) < 6.0);
  }
  {  // delay: a negative speed reads as 33 (unsigned)
    Fx a(FxType::Delay, false), b(FxType::Delay, false);
    a.fx->set_param(1, -20000);
    b.fx->set_param(1, 33);
    CHECK(a.run(in) == b.run(in));
  }
  {  // filter: the name index follows slot 2 over 0..22527 (12 types)
    for (int s2 : {0, 11264, 22527, 32767}) {
      Fx f(FxType::Filter, false);
      f.fx->set_param(2, Param(s2));
      f.run(tone(500, 0.1, 256));
      int32_t d[16] = {};
      f.fx->view(d);
      const int want = std::min(11, (std::min(s2, 22527) * 11 + 11264) / 22528);
      CHECK_EQ(d[10], want);
    }
  }
}

void test_mixer() {
  auto m = create_mixer();
  Command c;
  c.domain = Domain::Mixer;
  // a master type switch loads the new type's defaults (before t5 cwo kept punch's values)
  c.op = uint16_t(MixerOp::SetMasterFxParam);
  c.index = 3;
  c.value = 1234;
  m->command(c);
  c.op = uint16_t(MixerOp::SetMasterFx);
  c.value = int32_t(FxType::Cwo);
  m->command(c);
  MixerView v;
  m->view(v);
  const Param* d = fx_type_defaults(FxType::Cwo);
  for (int i = 0; i < kParams; ++i) CHECK_EQ(v.master_fx_params[i], d[i]);
  // metronome: a click on every beat, nothing between and nothing at level 0
  auto run = [&](int level) {
    auto mx = create_mixer();
    Command k;
    k.domain = Domain::Mixer;
    k.op = uint16_t(MixerOp::SetMetronome);
    k.value = level;
    mx->command(k);
    std::vector<int32_t> out;
    Sample zero[kBlock] = {}, tl[kBlock], tr[kBlock];
    const Sample* tracks[kTapeTracks] = {zero, zero, zero, zero};
    for (int64_t f = 0; f < 44100 * 2; f += kBlock) {
      ProcessContext ctx;
      ctx.frame = f;
      ctx.frames = kBlock;
      ctx.block_start = true;
      ctx.bpm_tenths = 1200;
      mx->process(ctx, tracks, zero, zero, tl, tr);
      out.insert(out.end(), tl, tl + kBlock);
    }
    return out;
  };
  const auto on = run(32704), off = run(0);
  CHECK(rms(off, 0, off.size()) == 0);
  // clicks start at 0, 22050, 44100, 66150 (120 BPM); the click is over well before the next beat
  for (size_t beat : {size_t(0), size_t(22050), size_t(44100), size_t(66150)}) CHECK(rms(on, beat, beat + 441) > 0.01);
  CHECK(rms(on, 11025, 11025 + 4410) < 1e-4);
}

}  // namespace

int main() {
  dsp_init();
  test_drum_sampler();
  test_dbox_signed_pitch();
  test_effects();
  test_mixer();
  return check_result("drum/fx parity");
}
