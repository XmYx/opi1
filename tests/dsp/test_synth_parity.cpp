// Synth sound checks for t2 (2026-10-23, PARITY-AUDIT.md §4.1): every synth engine, rendered offline through the
// public factory exactly as the core hosts it, must
//   - sound with its type defaults (3-note chord: RMS above -40 dBFS) and sit near zero (|DC| < 0.02 FS);
//   - never reach full scale with a 6-note chord (before: 20..40 samples >= 0.999 FS on 5 engines);
// and the firmware facts this task brought in must hold:
//   - fm: white picks 9 algorithms (int(white * 8)), green 3 operator ratios (fields of int(green * 4095)),
//     each audible and different;
//   - sampler: two directions (play_fwd / play_rev), the factory k4 values 9728..12000 play forward;
//   - pulse: no DC from narrow pulses;
//   - play mode: G-BEND (adsr 6) sets the pitch-bend range (4000 -> 2 semitones, 32767 -> 24), DETUNE (adsr 7)
//     scales the unison spread.
// Prints one line per engine (RMS, peak, DC, 6-voice peak). With OP1_RENDER_DIR set, writes
// synth_<engine>_chord.wav and synth_<engine>_chord6.wav there (16-bit stereo, 44.1 kHz).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "op1/dsp.h"
#include "../../src/dsp/dsp_internal.h"
#include "../../src/dsp/fm_algos.h"

using namespace op1;

namespace {

constexpr double kFs = 2147483648.0;
const char* g_dir = nullptr;

struct Host {
  std::byte* mem;
  Instrument* inst = nullptr;
  ControlInputs ctl;
  int64_t frame = 0;
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
  void cmd(InstrumentOp op, int index, int value, void* ptr = nullptr) {
    Command c;
    c.domain = Domain::Synth;
    c.op = uint16_t(op);
    c.index = index;
    c.value = value;
    c.ptr = ptr;
    inst->command(c);
  }
  // Renders `frames` mono samples on the 64-frame grid and appends them to out.
  void run(int frames, std::vector<int32_t>& out) {
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
      int32_t l[kBlock], r[kBlock];
      inst->process(ctx, l, r);
      out.insert(out.end(), l, l + n);
      frame += n;
      frames -= n;
    }
  }
  InstrumentView view() const {
    InstrumentView v{};
    inst->view(v);
    return v;
  }
};

struct Stats {
  double rms = 0, peak = 0, dc = 0;
  long full = 0;  // samples >= 0.999 FS
};
Stats stats(const std::vector<int32_t>& x, size_t a, size_t b) {
  Stats s;
  b = std::min(b, x.size());
  if (b <= a) return s;
  double sum = 0, mean = 0;
  for (size_t i = a; i < b; ++i) {
    const double v = x[i] / kFs;
    sum += v * v;
    mean += v;
    s.peak = std::max(s.peak, std::fabs(v));
    if (std::fabs(v) >= 0.999) ++s.full;
  }
  s.rms = std::sqrt(sum / double(b - a));
  s.dc = mean / double(b - a);
  return s;
}
double db(double v) { return v > 0 ? 20 * std::log10(v) : -999; }
long zero_crossings(const std::vector<int32_t>& x, size_t a, size_t b) {
  long n = 0;
  for (size_t i = std::max<size_t>(a, 1); i < std::min(b, x.size()); ++i)
    if ((x[i - 1] < 0) != (x[i] < 0)) ++n;
  return n;
}

void write_wav(const std::string& name, const std::vector<int32_t>& x) {
  if (!g_dir) return;
  const std::string path = std::string(g_dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  const uint32_t data = uint32_t(x.size() * 4);
  std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(2); u32(kSampleRate); u32(kSampleRate * 4); u16(4); u16(16);
  std::fwrite("data", 1, 4, f); u32(data);
  for (int32_t v : x) {
    const int16_t s = int16_t(v >> 16);
    std::fwrite(&s, 2, 1, f);
    std::fwrite(&s, 2, 1, f);
  }
  std::fclose(f);
}

// A 2 s soft saw at C4 for the sampler (made here: no Teenage Engineering audio in the tree).
std::vector<int16_t> g_tone;
SampleData g_tone_data;
void make_tone() {
  g_tone.assign(size_t(kSampleRate) * 2, 0);
  double ph = 0;
  for (size_t i = 0; i < g_tone.size(); ++i) {
    ph += 261.63 / kSampleRate;
    ph -= std::floor(ph);
    g_tone[i] = int16_t(14000.0 * (2 * ph - 1));
  }
  g_tone_data.frames = g_tone.data();
  g_tone_data.length = int32_t(g_tone.size());
  g_tone_data.root_hz = 261.63f;
}

const std::pair<InstrumentType, const char*> kSynths[] = {
    {InstrumentType::Fm, "fm"},           {InstrumentType::Digital, "digital"}, {InstrumentType::DrWave, "drwave"},
    {InstrumentType::String, "string"},   {InstrumentType::Cluster, "cluster"}, {InstrumentType::Pulse, "pulse"},
    {InstrumentType::Sampler, "sampler"}, {InstrumentType::Phase, "phase"},     {InstrumentType::Dna, "dna"},
    {InstrumentType::DSynth, "dsynth"},   {InstrumentType::Voltage, "voltage"}, {InstrumentType::Iter, "iter"},
};

// Notes held for `held` seconds, then `rel` seconds of release.
std::vector<int32_t> chord(Host& h, std::initializer_list<int> notes, double held = 1.0, double rel = 1.0) {
  std::vector<int32_t> out;
  for (int n : notes) h.inst->note_on(n, 100, -1);
  h.run(int(held * kSampleRate), out);
  for (int n : notes) h.inst->note_off(n, -1);
  h.run(int(rel * kSampleRate), out);
  return out;
}

void test_engines() {
  std::printf("%-8s %9s %6s %8s %10s %8s\n", "engine", "rms dBFS", "peak", "dc", "6v peak", "6v>=FS");
  for (const auto& [t, name] : kSynths) {
    Host h(t);
    CHECK(h.inst != nullptr);
    if (!h.inst) continue;
    if (t == InstrumentType::Sampler) h.cmd(InstrumentOp::SetSample, 0, 0, &g_tone_data);
    const std::vector<int32_t> x = chord(h, {60, 64, 67});
    const Stats s = stats(x, size_t(0.05 * kSampleRate), size_t(1.0 * kSampleRate));
    write_wav(std::string("synth_") + name + "_chord", x);

    Host h6(t);
    if (t == InstrumentType::Sampler) h6.cmd(InstrumentOp::SetSample, 0, 0, &g_tone_data);
    const std::vector<int32_t> y = chord(h6, {48, 55, 60, 64, 67, 72}, 1.0, 0.5);
    const Stats s6 = stats(y, 0, y.size());
    write_wav(std::string("synth_") + name + "_chord6", y);

    std::printf("%-8s %9.1f %6.3f %8.4f %10.3f %8ld\n", name, db(s.rms), s.peak, s.dc, s6.peak, s6.full);
    CHECK(db(s.rms) > -40.0);         // sounds
    CHECK(std::fabs(s.dc) < 0.02);    // centred (pulse sat at -0.14 FS before)
    CHECK(s6.full == 0);              // a 6-note chord stays below full scale
    CHECK(std::fabs(s6.dc) < 0.03);
  }
}

void test_fm() {
  // 9 algorithms, one per white band; all sound, all differ
  std::set<long long> sums;
  for (int a = 0; a < dsp::kFmAlgos; ++a) {
    const int white = (a * 32767 + 7) / 8;
    CHECK_EQ(dsp::fm_layout(white), a);
    Host h(InstrumentType::Fm);
    h.cmd(InstrumentOp::SetKnob, 2, white);
    const std::vector<int32_t> x = chord(h, {60}, 0.5, 0.0);
    CHECK_EQ(h.view().display[16], a);
    const Stats s = stats(x, 2205, x.size());
    CHECK(db(s.rms) > -40.0);
    long long sum = 0;
    for (size_t i = 0; i < x.size(); i += 7) sum = sum * 31 + (x[i] >> 12);
    sums.insert(sum);
    std::printf("fm algorithm %c: rms %.1f dBFS\n", 'A' + a, db(s.rms));
  }
  CHECK_EQ(long(sums.size()), long(dsp::kFmAlgos));
  CHECK_EQ(dsp::fm_layout(32767), 8);
  CHECK_EQ(dsp::fm_layout(0), 0);
  CHECK_EQ(dsp::fm_layout(3000), 1);  // the firmware rounds: the default white shows layout B (t3, 0x019A4D8A)
  // green: the default 4500 -> code 562 -> fields 1/0/6 (3/32, 2, 3.5); other codes change the sound
  CHECK_EQ(dsp::fm_code(4500), 562);
  {
    Host h(InstrumentType::Fm);
    std::vector<int32_t> x;
    h.run(128, x);
    const InstrumentView v = h.view();
    CHECK_EQ(v.display[17], (1 << 6) | (0 << 3) | 6);
    CHECK_EQ(v.display[20], 93);
    CHECK_EQ(v.display[21], 2000);
    CHECK_EQ(v.display[22], 3500);
  }
  std::set<long long> gs;
  for (int g : {0, 4500, 16384, 26000, 32767}) {
    Host h(InstrumentType::Fm);
    h.cmd(InstrumentOp::SetKnob, 1, g);
    const std::vector<int32_t> x = chord(h, {60}, 0.3, 0.0);
    long long sum = 0;
    for (size_t i = 0; i < x.size(); i += 5) sum = sum * 31 + (x[i] >> 12);
    gs.insert(sum);
  }
  CHECK_EQ(long(gs.size()), 5L);
}

void test_sampler_direction() {
  // a rising ramp, no loop: forward plays it rising, reverse falling
  std::vector<int16_t> ramp(size_t(kSampleRate), 0);
  for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = int16_t(-16000 + int(32000.0 * double(i) / double(ramp.size())));
  SampleData sd;
  sd.frames = ramp.data();
  sd.length = int32_t(ramp.size());
  sd.root_hz = 261.63f;
  for (int k4 : {9728, 12000, 20000, 32767}) {
    Host h(InstrumentType::Sampler);
    h.cmd(InstrumentOp::SetSample, 0, 0, &sd);
    h.cmd(InstrumentOp::SetKnob, 0, 0);
    h.cmd(InstrumentOp::SetKnob, 1, 0);
    h.cmd(InstrumentOp::SetKnob, 2, 0);  // loop in = loop out: no loop
    h.cmd(InstrumentOp::SetKnob, 3, 32767);
    h.cmd(InstrumentOp::SetKnob, 4, k4);
    std::vector<int32_t> x;
    h.inst->note_on(60, 100, -1);
    h.run(int(0.4 * kSampleRate), x);
    const double a = stats(x, size_t(0.05 * kSampleRate), size_t(0.15 * kSampleRate)).dc;
    const double b = stats(x, size_t(0.25 * kSampleRate), size_t(0.35 * kSampleRate)).dc;
    const bool forward = k4 < 16384;
    std::printf("sampler k4 %5d: %s (mean %.3f -> %.3f), display[21] %d\n", k4, forward ? "fwd" : "rev", a, b,
                int(h.view().display[21]));
    CHECK(forward ? b > a + 0.01 : b < a - 0.01);
    CHECK_EQ(h.view().display[21], forward ? 0 : 1);
  }
}

void test_bend_and_detune() {
  // fm with blue 0 = a pure sine carrier: the zero-crossing rate is the pitch
  auto crossings = [](int bend_slot, int16_t bend) {
    Host h(InstrumentType::Fm);
    h.cmd(InstrumentOp::SetKnob, 0, 0);
    h.cmd(InstrumentOp::SetAdsr, 6, bend_slot);
    h.ctl.pitch_bend = bend;
    std::vector<int32_t> x;
    h.inst->note_on(60, 100, -1);
    h.run(kSampleRate, x);
    return zero_crossings(x, size_t(0.2 * kSampleRate), x.size());
  };
  const double base = double(crossings(4000, 0));
  const double r2 = double(crossings(4000, 8191)) / base;
  const double r24 = double(crossings(32767, 8191)) / base;
  std::printf("bend: range 4000 -> x%.3f (2 st = 1.122), 32767 -> x%.3f (24 st = 4.0)\n", r2, r24);
  CHECK(r2 > 1.10 && r2 < 1.15);
  CHECK(r24 > 3.8 && r24 < 4.2);
  // unison spread follows DETUNE
  auto unison = [](int det) {
    Host h(InstrumentType::Digital);
    h.cmd(InstrumentOp::SetAdsr, 4, 3 * 4096 + 2048);
    h.cmd(InstrumentOp::SetAdsr, 7, det);
    std::vector<int32_t> x;
    h.run(128, x);
    h.inst->note_on(60, 100, -1);
    h.run(kSampleRate / 2, x);
    return x;
  };
  const auto u0 = unison(0), u4 = unison(4000), u32 = unison(32767);
  long d04 = 0, d4m = 0;
  for (size_t i = 0; i < u0.size(); ++i) {
    d04 += (u0[i] != u4[i]);
    d4m += (u4[i] != u32[i]);
  }
  CHECK(d04 > 1000 && d4m > 1000);
}

// cluster: display[20..23] = level of oscillators 0, 1, 3, 6 while sounding, 0 when idle
void test_cluster_levels() {
  Host h(InstrumentType::Cluster);
  h.cmd(InstrumentOp::SetKnob, 0, 32767);  // 12 waves: all four shown oscillators sound
  std::vector<int32_t> x;
  h.inst->note_on(60, 100, -1);
  h.run(kSampleRate / 4, x);
  InstrumentView v = h.view();
  std::printf("cluster levels while sounding: %d %d %d %d\n", int(v.display[20]), int(v.display[21]),
              int(v.display[22]), int(v.display[23]));
  for (int j = 20; j < 24; ++j) CHECK(v.display[j] > 0);
  // the levels move from block to block while the note is held (no peak hold)
  std::set<int> seen;
  for (int b = 0; b < 20; ++b) {
    h.run(kBlock, x);
    seen.insert(int(h.view().display[20]));
  }
  CHECK(seen.size() > 10);
  h.inst->note_off(60, -1);
  h.run(kSampleRate * 3, x);
  v = h.view();
  for (int j = 20; j < 24; ++j) CHECK_EQ(v.display[j], 0);
  Host one(InstrumentType::Cluster);
  one.cmd(InstrumentOp::SetKnob, 0, 0);  // 1 wave: only oscillator 0
  one.inst->note_on(60, 100, -1);
  one.run(kSampleRate / 4, x);
  v = one.view();
  CHECK(v.display[20] > 0);
  CHECK_EQ(v.display[21], 0);
}

// phase: display[20] (phase activity) non-zero while sounding, both 0 when idle
void test_phase_activity() {
  Host h(InstrumentType::Phase);
  std::vector<int32_t> x;
  h.inst->note_on(60, 100, -1);
  h.run(kSampleRate / 4, x);
  InstrumentView v = h.view();
  std::printf("phase activity while sounding: %d %d\n", int(v.display[20]), int(v.display[21]));
  CHECK(v.display[20] != 0);
  h.inst->note_off(60, -1);
  h.run(kSampleRate * 3, x);
  v = h.view();
  CHECK_EQ(v.display[20], 0);
  CHECK_EQ(v.display[21], 0);
}

// voltage: display[24..39] = 32 scope samples (int16 pairs) while sounding, 0 when idle
void test_voltage_scope() {
  Host h(InstrumentType::Voltage);
  std::vector<int32_t> x;
  h.inst->note_on(60, 100, -1);
  h.run(kSampleRate / 4, x);
  InstrumentView v = h.view();
  int nonzero = 0, peak = 0;
  for (int j = 24; j < 40; ++j)
    for (int16_t s16 : {int16_t(v.display[j] & 0xFFFF), int16_t(uint32_t(v.display[j]) >> 16)}) {
      nonzero += s16 != 0;
      peak = std::max(peak, std::abs(int(s16)));
    }
  std::printf("voltage scope while sounding: %d/32 non-zero, peak %d\n", nonzero, peak);
  CHECK(nonzero > 16 && peak > 1000);
  h.inst->note_off(60, -1);
  h.run(kSampleRate * 3, x);
  v = h.view();
  for (int j = 24; j < 40; ++j) CHECK_EQ(v.display[j], 0);
}

// string [24..39], dna [22], dsynth [22..23] and [24..54]: set while the newest voice sounds, 0 when idle
void test_newest_voice_views() {
  struct Case { InstrumentType t; const char* name; int from, to; };
  for (const Case& c : {Case{InstrumentType::String, "string", 24, 40}, Case{InstrumentType::Dna, "dna", 22, 23},
                        Case{InstrumentType::DSynth, "dsynth", 22, 55}}) {
    Host h(c.t);
    std::vector<int32_t> x;
    h.inst->note_on(60, 100, -1);
    h.run(kSampleRate / 4, x);
    InstrumentView v = h.view();
    int nonzero = 0;
    for (int j = c.from; j < c.to; ++j) nonzero += v.display[j] != 0;
    std::printf("%s newest-voice view: %d/%d slots non-zero while sounding\n", c.name, nonzero, c.to - c.from);
    CHECK(nonzero > (c.to - c.from) / 2);
    h.inst->note_off(60, -1);
    h.run(kSampleRate * 4, x);
    v = h.view();
    for (int j = c.from; j < c.to; ++j) CHECK_EQ(v.display[j], 0);
  }
}

// sampler: gain 16 (k7/32768)^2 (unity at 8192) and loop in/out fine-tuned by k5/k6 / 32768; the view
// publishes the sample. drwave: the view publishes its cycle.
void test_sampler_and_drwave_views() {
  struct G { int k7, db10; };
  for (const G g : {G{8192, 0}, G{16384, 120}, G{32767, 241}, G{4096, -120}}) {
    Host h(InstrumentType::Sampler);
    h.cmd(InstrumentOp::SetSample, 0, 0, &g_tone_data);
    h.cmd(InstrumentOp::SetKnob, 7, g.k7);
    std::vector<int32_t> x;
    h.run(128, x);
    const InstrumentView v = h.view();
    std::printf("sampler k7 %5d: gain %+.1f dB\n", g.k7, v.display[23] / 10.0);
    CHECK(std::abs(int(v.display[23]) - g.db10) <= 2);
    CHECK(v.sample == &g_tone_data);
  }
  {
    Host h(InstrumentType::Sampler);
    h.cmd(InstrumentOp::SetSample, 0, 0, &g_tone_data);
    h.cmd(InstrumentOp::SetKnob, 0, 0);
    h.cmd(InstrumentOp::SetKnob, 1, 16384);
    h.cmd(InstrumentOp::SetKnob, 2, 16384);
    h.cmd(InstrumentOp::SetKnob, 5, 0);
    h.cmd(InstrumentOp::SetKnob, 6, 32767);  // loop out = loop in + 1/32768 of the length (~2.7 frames of 2 s)
    std::vector<int32_t> x;
    h.run(128, x);
    InstrumentView v = h.view();
    const int lo = v.display[18], hi = v.display[19];
    std::printf("sampler loop fine: in %d out %d (length %d)\n", lo, hi, int(v.display[16]));
    CHECK(hi > lo && hi - lo <= 4);
    h.cmd(InstrumentOp::SetKnob, 2, 0);  // coarse loop out before loop in: loop out = loop in
    h.run(128, x);
    v = h.view();
    CHECK_EQ(v.display[19], v.display[18]);
    Host none(InstrumentType::Sampler);
    none.run(128, x);
    CHECK(none.view().sample == nullptr);
  }
  {
    Host h(InstrumentType::DrWave);
    std::vector<int32_t> x;
    h.run(128, x);
    const InstrumentView v = h.view();
    int peak = 0, nonzero = 0;
    for (int i = 0; i < 256; ++i) {
      peak = std::max(peak, std::abs(int(v.wave[i])));
      nonzero += v.wave[i] != 0;
    }
    std::printf("drwave cycle: valid %d, peak %d (Q16), %d/256 non-zero\n", int(v.wave_valid), peak, nonzero);
    CHECK(v.wave_valid);
    CHECK(peak > 16384 && peak <= 65536);
    CHECK(nonzero > 64);
  }
}

void test_pulse_dc() {
  for (int g : {0, 16000, 32767}) {
    Host h(InstrumentType::Pulse);
    h.cmd(InstrumentOp::SetKnob, 1, g);  // pulse width: wide .. narrow
    const std::vector<int32_t> x = chord(h, {48, 55, 60, 64, 67, 72}, 1.0, 0.0);
    const Stats s = stats(x, size_t(0.1 * kSampleRate), x.size());
    std::printf("pulse green %5d: dc %.4f FS, rms %.1f dBFS\n", g, s.dc, db(s.rms));
    CHECK(std::fabs(s.dc) < 0.02);
  }
}

}  // namespace

int main() {
  g_dir = std::getenv("OP1_RENDER_DIR");
  dsp_init();
  make_tone();
  test_engines();
  test_fm();
  test_sampler_direction();
  test_bend_and_detune();
  test_pulse_dc();
  test_cluster_levels();
  test_phase_activity();
  test_voltage_scope();
  test_newest_voice_views();
  test_sampler_and_drwave_views();
  return check_result("synth parity");
}
