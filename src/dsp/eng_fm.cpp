// fm (id 0, Fm.cpp in the firmware): four sine operators.
//
// The firmware's DSP routine is hand-written Blackfin code that has not been decoded (og-behaviour-details.md
// §7). The operator model follows what the firmware's fm screen decodes from the knobs (fm_algos.h, t2
// 2026-10-23): white picks one of nine algorithms (layouts A..I), green picks three operator ratios from the
// firmware's own ratio tables (512 combinations). Defaults 20000/4500/3000/18000; slots 4..7 hold the constants
// 15000/0/100/1500 in every factory preset (kept, not used by the sound).
//
// Knobs: blue (0) = FM amount (modulation index up to ~8 rad, squared curve, with a short extra bite at the
//        note start), green (1) = ratios of operators 2/3/4 (fields A/B/C of round(green * 4095)), white (2) =
//        algorithm round(white * 8) = 0..8, orange (3) = detune (operator detune up to 25 cents).
// Carriers play at the note (fanned out by the detune); modulators at their ratio. The index is limited so the
// sidebands stay below 0.45 fs.
//
// display[16..]: [16] algorithm 0..8 (= fm.svg group A..I), [17] ratio fields (A << 6) | (B << 3) | C,
//                [18] index (Q16 cycles), [19] detune cents, [20] [21] [22] ratio of operator 2 / 3 / 4 x 1000.
#include "fm_algos.h"
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

struct FmEngine {
  static constexpr InstrumentType kType = InstrumentType::Fm;
  struct Voice {
    u32 ph[4] = {};
    s32 out[4] = {};
    s32 bite = 0;  // extra index at note start, Q31, decays over ~0.6 s
  };

  int algo = 0, fields = 0, cents = 0;
  s32 ratio1000[4] = {1000, 1000, 1000, 1000};
  s64 index_q16 = 0;              // modulation index in cycles (Q16)
  u64 rq[4] = {65536, 65536, 65536, 65536};  // frequency ratio per operator (Q16); carriers: see carrier_rq
  u64 carrier_rq[4] = {65536, 65536, 65536, 65536};
  u64 max_mod_rq = 65536;
  s32 level = kHalf;

  void control(const s32 k[kParams]) noexcept {
    algo = fm_layout(k[2]);
    const int code = fm_code(k[1]);
    const int fa = (code >> 9) & 7, fb = (code >> 6) & 7, fc = (code >> 3) & 7;
    fields = (fa << 6) | (fb << 3) | fc;
    const s32 micro[4] = {1000000, kFmRatioA[fa], kFmRatioB[fb], kFmRatioC[fc]};
    cents = s32((s64(u15(k[3])) * 25) >> 15);
    // index: (k0)^2 * 8 radians -> cycles (8 / 2pi = 1.2732)
    index_q16 = (s64(sq15(u15(k[0]))) * 83443) >> 15;
    const s64 dq = s64(cents) * 65536 / 10000;  // cents * 1e-4 in Q16
    static constexpr int kDetuneSign[4] = {0, 4, -3, 2};  // per-operator detune, +0.04 % / -0.03 % / +0.02 % per cent
    for (int op = 0; op < 4; ++op) {
      ratio1000[op] = micro[op] / 1000;
      const u64 r = u64(micro[op]) * 65536 / 1000000;
      rq[op] = (r * u64(65536 + dq * kDetuneSign[op])) >> 16;
    }
    const FmAlgo& a = kFmAlgos9[algo];
    const u64 spread = cents_ratio_q16(cents) - 65536;  // carriers fan out around the note
    int c = 0, nc = 0;
    max_mod_rq = 65536;
    for (int op = 0; op < 4; ++op) {
      if (a.carriers & (1 << op)) {
        carrier_rq[op] = 65536 + spread * u64(c + 1);
        ++c;
        ++nc;
      } else if (rq[op] > max_mod_rq) {
        max_mod_rq = rq[op];
      }
    }
    static constexpr s32 kLevel[5] = {0, s32(0.62 * 2147483648.0), s32(0.438 * 2147483648.0),
                                      s32(0.358 * 2147483648.0), s32(0.31 * 2147483648.0)};
    level = kLevel[nc];
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (legato) return;
    v = Voice{};
    v.bite = kOne;
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    const FmAlgo& a = kFmAlgos9[algo];
    // index with the note-start bite (0.6 + 0.4 * bite), limited against aliasing
    s64 idx = (index_q16 * (s64(19661) + ((s64(13107) * v.bite) >> 31))) >> 15;
    if (inc > 0) {
      const u64 top = (u64(kMaxInc) << 16) / ((u64(inc) * max_mod_rq) >> 16 | 1);  // 0.45 fs / (f r), Q16
      const s64 lim_rad = s64(top) - 65536;
      const s64 lim = lim_rad > 0 ? (lim_rad * 10430) >> 16 : 0;  // radians -> cycles
      if (idx > lim) idx = lim;
    }
    const s64 fb_idx = a.fb >= 0 ? idx / 2 : 0;
    // bite decays with a 0.6 s time constant
    v.bite = s32(s64(v.bite) - ((s64(v.bite) * (s64(n) * coef_ms(600.0))) >> 31));
    if (v.bite < 0) v.bite = 0;

    u32 incs[4];
    for (int op = 0; op < 4; ++op)
      incs[op] = inc_mul(inc, (a.carriers & (1 << op)) ? carrier_rq[op] : rq[op]);

    for (int i = 0; i < n; ++i) {
      s64 sum = 0;
      for (int op = 3; op >= 0; --op) {
        s64 m = 0;
        const uint8_t mods = a.mods[op];
        for (int j = op + 1; j < 4; ++j)
          if (mods & (1 << j)) m += v.out[j];
        u32 phase = v.ph[op] + pm_offset(m, idx);
        if (op == a.fb) phase += pm_offset(v.out[op], fb_idx);
        v.out[op] = sin_q31(phase);
        v.ph[op] += incs[op];
        if (a.carriers & (1 << op)) sum += v.out[op];
      }
      out[i] = soft_clip((sum * level) >> 31);
    }
  }

  void view(int32_t* d) const noexcept {
    d[0] = algo;
    d[1] = fields;
    d[2] = int32_t(index_q16);
    d[3] = cents;
    d[4] = ratio1000[1];
    d[5] = ratio1000[2];
    d[6] = ratio1000[3];
  }
};

}  // namespace

Instrument* make_fm(void* mem, size_t size) noexcept { return make_synth<FmEngine>(mem, size); }

}  // namespace op1::dsp
