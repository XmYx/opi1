// cluster (id 4, Cls.cpp): a stack of 1-12 detuned band-limited saws. Designed by ear (the firmware routine
// is not decoded) on the OG facts: defaults 4000/4000/8000/100 (unitor near 0 must sound open and bright).
//
// Knobs: blue (0) = number of waves 1..12, green (1) = wave envelope (extra spread up to +300 cents at the
//        note start, settling in 0.05 .. 1.45 s), white (2) = spread (detune up to +-70 cents, squared
//        curve), orange (3) = unitor: 0 leaves the stack open and wide, up fuses it through a low-pass
//        (16 kHz .. 250 Hz) with a little more resonance.
//
// display[16..]: [16] wave count 1..12, [17] wave envelope Q15, [18] spread cents, [19] low-pass Hz,
//                [20..23] output level of oscillators 0, 1, 3, 6 (Q15, 0 when idle): the cls screen reads the
//                firmware's per-oscillator level array (0x1baa93c) for these four, rewritten every block (t3,
//                2026-10-23). Here: |oscillator sample at the block's end| x its mix gain, plus an LCG term of up
//                to 1/8 of the gain (the firmware's random term is not decoded: ASSUMED), so the bumps roll as
//                the detuned phases drift. Newest rendering voice wins; decays to 0 when no voice renders.
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

constexpr int kMaxWaves = 12;
// Position of each wave in the spread, in sixths: 0, -1, +1, -2, +2, ... (wave 0 sits on the note).
constexpr int kOffs[kMaxWaves] = {0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6};
// 0.6 / sqrt(n), Q15.
constexpr s32 kNorm[kMaxWaves + 1] = {0, 19661, 13902, 11351, 9830, 8793, 8027, 7431, 6951, 6554, 6217, 5928, 5676};

struct ClusterEngine {
  static constexpr InstrumentType kType = InstrumentType::Cluster;
  struct Voice {
    u32 ph[kMaxWaves] = {};
    bool seeded = false;
    s32 env = 0;       // wave envelope, Q31, 1 at note start
    s32 cents_q8 = 0;  // smoothed spread, cents * 256
    Svf svf;
  };

  int waves = 1;
  s32 wenv15 = 0, spread_cents = 0, unitor15 = 0, env_coef = 0;
  u32 cutoff_hz = 16000;
  SvfCoef coef;
  static constexpr int kShown[4] = {0, 1, 3, 6};  // the oscillators the screen shows
  s32 lvl[4] = {};                                 // their level, Q15: rewritten per block, decays when idle
  bool lvl_rendered = false;
  u32 lvl_seed = 12345;

  void control(const s32 k[kParams]) noexcept {
    static constexpr ExpCurve kCut(250.0, 16000.0);
    waves = 1 + int((s64(u15(k[0])) * 11 + 16383) >> 15);
    wenv15 = u15(k[1]);
    spread_cents = s32(s64(sq15(u15(k[2]))) * 70 / 32767);
    unitor15 = u15(k[3]);
    const u64 hz = kCut.at(32767 - unitor15);
    cutoff_hz = u32(hz >> 16);
    coef = svf_coef(hz_inc(hz), svf_k_from_res(4915 + s32((s64(unitor15) * 8192) >> 15)));  // 0.15 .. 0.4
    // envelope time constant 0.05 + 1.4 wenv seconds, as a per-sample coefficient
    const s64 tau_samples = s64(kSampleRate) * (3277 + ((s64(wenv15) * 91750) >> 15)) >> 16;
    env_coef = s32((s64(1) << 31) / (tau_samples > 0 ? tau_samples : 1));
    if (!lvl_rendered)
      for (s32& l : lvl) l = s32((s64(l) * 13) >> 4);  // ~-1.8 dB per block: gone ~40 ms after the last voice
    lvl_rendered = false;
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (!v.seeded) {  // free-running phases, spread so the waves do not start in step
      for (int w = 0; w < kMaxWaves; ++w) v.ph[w] = u32(w) * 0x9E3779B9u;
      v.seeded = true;
    }
    if (!legato) v.env = kOne;
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    // wave envelope: extra spread at note start, decaying over the block
    const s32 env15 = v.env >> 16;
    const s32 extra = s32(((s64(wenv15) * env15 >> 15) * 300) >> 15);  // up to +300 cents
    v.env = s32(clampv<s64>(s64(v.env) - ((s64(v.env) * env_coef >> 31) * n), 0, kOne));
    const s32 target_q8 = (spread_cents + extra) * 256;
    v.cents_q8 += s32((s64(target_q8) - v.cents_q8) * 9830 >> 15);  // smooth 0.3 per block
    u32 incs[kMaxWaves];
    for (int w = 0; w < waves; ++w)
      incs[w] = inc_mul(inc, cents_ratio_q16(s32(s64(kOffs[w]) * v.cents_q8 / (6 * 256))));
    const s32 norm = kNorm[waves];
    for (int i = 0; i < n; ++i) {
      s64 sum = 0;
      for (int w = 0; w < waves; ++w) {
        sum += saw_blep(v.ph[w], incs[w]);
        v.ph[w] += incs[w];
      }
      const s32 x = sat((sum * norm) >> 15);
      out[i] = soft_clip((s64(v.svf.tick(coef, x)) * 5) >> 2);
    }
    for (int w = waves; w < kMaxWaves; ++w) v.ph[w] += inc * u32(n);  // idle waves keep running
    // screen levels: each shown oscillator's |saw| at the block's end times its mix gain, plus a random term
    for (int j = 0; j < 4; ++j) {
      if (kShown[j] >= waves) { lvl[j] = 0; continue; }
      const s64 saw = s32(v.ph[kShown[j]] ^ 0x80000000u);
      lvl_seed = lvl_seed * 1664525u + 1013904223u;
      const s32 rnd = s32((s64(lvl_seed >> 17) * norm) >> 18);  // 0 .. norm / 8
      lvl[j] = clampv<s32>(s32(((saw < 0 ? -saw : saw) * norm) >> 31) + rnd, 0, 32767);
    }
    lvl_rendered = true;
  }

  void view(int32_t* d) const noexcept {
    d[0] = waves;
    d[1] = wenv15;
    d[2] = spread_cents;
    d[3] = int32_t(cutoff_hz);
    for (int j = 0; j < 4; ++j) d[4 + j] = lvl[j];
  }
};

}  // namespace

Instrument* make_cluster(void* mem, size_t size) noexcept { return make_synth<ClusterEngine>(mem, size); }

}  // namespace op1::dsp
