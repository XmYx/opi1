// phase (id 7, Pd.cpp in the firmware: phase distortion): two phase-distorted cosine oscillators, CZ style.
//
// The firmware's DSP routine is hand-written Blackfin code that has not been decoded (og-behaviour-details.md
// §7), so the sound is designed by ear on the OG facts: defaults 8000/0/0/0 (so distortion, filter and tilt
// at 0 must still give a usable tone: a plain dark cosine pair).
//
// Knobs: blue (0) = phase shift of oscillator B against A (0 .. 0.4 cycle; their sum thins and hollows the
//        tone), green (1) = distortion (moves the PD breakpoint: sine -> saw-like), white (2) = phase filter
//        (crossfades to a windowed "resonance" wave whose inner frequency rises 1x .. 13x), orange (3) = tilt
//        (dark at 0 .. bright at the top; a one-pole split at 700 Hz).
// Segments are kept at least two samples long so the breakpoint stays band-limited enough.
//
// display[16..]: [16] breakpoint (Q16 of a cycle, 32768 = no distortion), [17] B phase shift (Q16 of a cycle),
//                [18] resonance mix Q15, [19] tilt Q15,
//                [20] activity 1: per block, saturating sum over sounding voices of (top 16 bits of the oscillator
//                     phase x top 16 bits of the resonance phase) as a Q15 multiply,
//                [21] activity 2: per block, saturating sum over voices of the voice output at the block's last
//                     sample (Q15). Both 0 when idle; the pd screen reads the firmware's equivalents at 0x01DE4B8C
//                     (t3, 2026-10-23). Published one block late (latched at the next control tick).
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

struct PhaseEngine {
  static constexpr InstrumentType kType = InstrumentType::Phase;
  struct Voice {
    u32 ph = 0;
    OnePole lp;
  };

  u32 shift = 0;        // phase offset of B, Q32
  s32 dist = 0;         // Q15
  s32 filt = 0;         // Q15
  s32 tilt = 0;         // Q15
  s32 lo15 = 0, hi15 = 0;  // tilt gains Q15 (up to 1.5)
  s32 lp_coef = 0;
  s64 acc_act[2] = {};  // this block's sums (chunks weighted by length, so a 2 x 32 block counts once)
  s32 act[2] = {};      // the last complete block's, Q15 saturated

  void control(const s32 k[kParams]) noexcept {
    for (int j = 0; j < 2; ++j) {
      act[j] = s32(clampv<s64>(acc_act[j], -32768, 32767));
      acc_act[j] = 0;
    }
    shift = u32((u64(u15(k[0])) * u64(0.4 * 4294967296.0)) >> 15);
    dist = u15(k[1]);
    filt = u15(k[2]);
    tilt = u15(k[3]);
    lo15 = 49152 - tilt;  // 1.5 - tilt
    hi15 = 16384 + tilt;  // 0.5 + tilt
    lp_coef = onepole_coef(hz_inc(u64(700) << 16));
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (!legato) v.ph = 0;
  }

  // Phase-distorted -cos at phase p: breakpoint b (Q32, <= 0.5), with ka = 2^63 / b and kb = 2^63 / (1 - b).
  static s32 pd(u32 p, u32 b, u64 ka, u64 kb) noexcept {
    u32 q;
    if (p < b) q = u32((u64(p) * ka) >> 32);
    else q = u32(kHalfCycle + ((u64(p - b) * kb) >> 32));
    return -cos_q31(q);
  }
  // CZ resonance wave: cosine at ratio rf (Q16) inside a triangle window (zero at the cycle ends).
  static s32 res(u32 p, u64 rf) noexcept {
    const u32 d = p >= kHalfCycle ? p - kHalfCycle : kHalfCycle - p;
    const s32 w = s32(kHalfCycle - d - (d == 0 ? 1 : 0));  // Q31 window 0..1
    const u32 q = u32((u64(p) * rf) >> 16);
    return sat((s64(mulq(w, cos_q31(q))) * 45875) >> 15);  // x 1.4
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    // breakpoint 0.5 - 0.49 dist, kept >= 2 samples
    s64 b = s64(kHalfCycle) - ((s64(dist) * s64(0.49 * 4294967296.0)) >> 15);
    b = clampv<s64>(b, s64(inc) * 2 + 1, s64(kHalfCycle));
    const u32 bu = u32(b);
    const u64 ka = (u64(1) << 63) / bu, kb = (u64(1) << 63) / ((u64(1) << 32) - bu);
    // resonance ratio 1 .. 13, kept below 0.4 fs
    u64 rf = 65536 + ((u64(sq15(filt)) * 12 * 65536) >> 15);
    if (inc > 0) {
      const u64 top = (u64(0.4 * 4294967296.0) << 16) / inc;  // 0.4 fs / f, Q16
      if (rf > top) rf = top < 65536 ? 65536 : top;
    }
    for (int i = 0; i < n; ++i) {
      const u32 pa = v.ph, pb = v.ph + shift;
      const s32 a = pd(pa, bu, ka, kb), bb = pd(pb, bu, ka, kb);
      const s32 xa = lerp15(a, res(pa, rf), filt), xb = lerp15(bb, res(pb, rf), filt);
      v.ph += inc;
      const s32 x = sat((s64(xa) + xb) >> 1);
      const s32 l = v.lp.tick(lp_coef, x);
      const s64 y = ((s64(l) * lo15) >> 15) + (((s64(x) - l) * hi15) >> 15);
      out[i] = soft_clip(y * 3 / 4);
    }
    if (n > 0) {
      const s64 po = s64(v.ph >> 16), pr = s64(u32((u64(v.ph) * rf) >> 16) >> 16);
      acc_act[0] += ((po * pr) >> 15) * n / kBlock;
      acc_act[1] += s64(out[n - 1] >> 16) * n / kBlock;
    }
  }

  void view(int32_t* d) const noexcept {
    d[0] = 32768 - s32((s64(dist) * 32112) >> 15);
    d[1] = s32(shift >> 16);
    d[2] = filt;
    d[3] = tilt;
    d[4] = act[0];
    d[5] = act[1];
  }
};

}  // namespace

Instrument* make_phase(void* mem, size_t size) noexcept { return make_synth<PhaseEngine>(mem, size); }

}  // namespace op1::dsp
