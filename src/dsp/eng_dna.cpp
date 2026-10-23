// dna (id 11, Id.cpp in the firmware): single-cycle "DNA" wavetables, each grown deterministically from its
// wave number (a random harmonic recipe), through a bipolar filter, with noise.
//
// The firmware's DSP routine is hand-written Blackfin code that has not been decoded (og-behaviour-details.md
// §7), so the sound is designed by ear on the OG facts: defaults -16000/5000/0/1024, and slot 0 is bipolar in
// the factory data (-21299..32767).
//
// Knobs: blue (0) = filter, bipolar: negative closes a low-pass from open (0) down to 150 Hz (-32768),
//        positive opens a high-pass from 20 Hz up to 8 kHz (32767); 0 = open, green (1) = wave number 1..32,
//        white (2) = modifier (first half morphs toward the next wave, second half scrambles the table index
//        bits), orange (3) = noise (mixes in noise and pitch jitter).
// The 33 tables (32 waves + 1 to morph toward) are computed once in dsp_init(): no Teenage Engineering data.
//
// display[16..]: [16] wave number 1..32, [17] morph Q15, [18] scramble mask 0..31, [19] noise Q15,
//                [20] filter mode (0 open, 1 low-pass, 2 high-pass), [21] cutoff in Hz,
//                [22] last output sample of the newest voice, int16 (0 when idle; seeds the id screen's tile
//                rotation, t3 2026-10-23).
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

constexpr int kWaves = 32, kN = 256;

constexpr double kPi = 3.14159265358979323846;
constexpr double csin(double x) {
  const double tau = 2 * kPi;
  x -= tau * double(static_cast<long long>(x / tau));
  if (x > kPi) x -= tau;
  if (x < -kPi) x += tau;
  double term = x, sum = x;
  for (int k = 1; k < 12; ++k) {
    term *= -x * x / double((2 * k) * (2 * k + 1));
    sum += term;
  }
  return sum;
}
constexpr double cexp(double x) {  // x in about -3..0
  int halvings = 0;
  while (x < -0.5 || x > 0.5) { x *= 0.5; ++halvings; }
  double term = 1, sum = 1;
  for (int k = 1; k < 16; ++k) { term *= x / k; sum += term; }
  for (int i = 0; i < halvings; ++i) sum *= sum;
  return sum;
}

struct DnaTables {
  s32 t[kWaves + 1][kN];
};
DnaTables make_tables() {
  DnaTables out{};
  for (int w = 0; w <= kWaves; ++w) {
    u32 s = u32(9001 + w * 7919);
    auto unit = [&s]() {
      s = s * 1664525u + 1013904223u;
      return double(s >> 8) / 16777216.0;
    };
    double acc[kN] = {};
    const int harmonics = 3 + (w % 6) + 2;
    for (int h = 1; h <= harmonics; ++h) {
      const double r1 = unit();
      const double r2 = unit();
      const double ph = unit();
      const double amp = r1 * cexp(-(0.6 + r2 * 0.6) * detail::ln_c(double(h)));
      for (int i = 0; i < kN; ++i) acc[i] += amp * csin(2 * kPi * (h * double(i) / kN + ph));
    }
    double m = 0;
    for (double v : acc) m = v > m ? v : (-v > m ? -v : m);
    if (m <= 0) m = 1;
    for (int i = 0; i < kN; ++i) out.t[w][i] = s32(acc[i] / m * 0.98 * 2147483648.0);
  }
  return out;
}
DnaTables g_dna;  // filled once by init_dna_tables() from dsp_init() (main thread)

struct DnaEngine {
  static constexpr InstrumentType kType = InstrumentType::Dna;
  struct Voice {
    u32 ph = 0;
    Rng rng{4242};
    Svf f;
  };

  int wave = 0;          // 0..31
  s32 morph = 0;         // Q15
  u32 scramble = 0;      // xor mask on the table index
  s32 noise = 0;         // Q15
  int mode = 0;          // 0 open, 1 low-pass, 2 high-pass
  SvfCoef coef;
  u32 cutoff_hz = 0;

  void control(const s32 k[kParams]) noexcept {
    shown_out = newest_rendered ? last_out : 0;
    newest_rendered = false;
    static constexpr ExpCurve kLp(150.0, 18000.0), kHp(20.0, 8000.0);
    const s32 f = s15(k[0]);
    u64 hz = 0;
    if (f < 0) {
      mode = 1;
      hz = kLp.at(32767 + f);
    } else if (f > 0) {
      mode = 2;
      hz = kHp.at(f);
    } else {
      mode = 0;
    }
    if (mode) coef = svf_coef(hz_inc(hz), svf_k_q(0.8));
    cutoff_hz = u32(hz >> 16);
    wave = choose(k[1], kWaves);
    const s32 m = u15(k[2]);
    morph = m >= 16384 ? 32767 : m * 2;
    scramble = m > 16384 ? u32(((m - 16384) * 31) >> 14) : 0;
    noise = u15(k[3]);
  }

  const Voice* newest = nullptr;
  s32 last_out = 0, shown_out = 0;
  bool newest_rendered = false;

  void note_on(Voice& v, bool legato) noexcept {
    if (!legato) v.ph = 0;
    newest = &v;
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    const s32* ta = g_dna.t[wave];
    const s32* tb = g_dna.t[wave + 1];
    const s32 dry = 32767 - noise / 2;
    const s32 wet = s32((s64(noise) * 26214) >> 15);  // 0.8 noise
    for (int i = 0; i < n; ++i) {
      const s32 nz = v.rng.next();
      // pitch jitter: up to 0.01 cycle
      const u32 p = v.ph + u32(u64(((s64(nz) * noise) >> 15) * 1311 >> 16));
      const u32 i0 = ((p >> 24) ^ scramble) & (kN - 1), i1 = (((p >> 24) + 1) ^ scramble) & (kN - 1);
      const s32 fr = s32((p >> 8) & 0xffff);
      const s32 a = interp16(ta[i0], ta[i1], fr);  // i0/i1 need not be neighbours once scrambled
      const s32 b = interp16(tb[i0], tb[i1], fr);
      v.ph += inc;
      const s32 s = lerp15(a, b, morph);
      s32 x = sat(((s64(s) * dry) >> 15) + ((s64(nz) * wet) >> 15));
      if (mode) {
        v.f.tick(coef, x);
        x = mode == 1 ? v.f.lp : v.f.hp;
      }
      out[i] = soft_clip((s64(x) * 7) >> 3);
    }
    if (&v == newest && n > 0) {
      last_out = out[n - 1] >> 16;
      newest_rendered = true;
    }
  }

  void view(int32_t* d) const noexcept {
    d[0] = wave + 1;
    d[1] = morph;
    d[2] = int32_t(scramble);
    d[3] = noise;
    d[4] = mode;
    d[5] = int32_t(cutoff_hz);
    d[6] = shown_out;
  }
};

}  // namespace

void init_dna_tables() noexcept { g_dna = make_tables(); }

Instrument* make_dna(void* mem, size_t size) noexcept { return make_synth<DnaEngine>(mem, size); }

}  // namespace op1::dsp
