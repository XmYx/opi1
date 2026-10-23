// pulse (id 5, Pls.cpp in the firmware): two band-limited pulse trains through a resonant low-pass.
//
// The firmware's DSP routine is hand-written Blackfin code that has not been decoded (og-behaviour-details.md
// §7), so the sound is designed by ear on the OG facts: defaults 20000/16000/4000/3000, and slot 3 is
// bipolar in the factory data (-9288..6141).
//
// Knobs: blue (0) = filter (low-pass cutoff 120 Hz .. 14 kHz), green (1) = amplitude (pulse 1 width: wide
//        and full at 0, narrow and nasal at the top), white (2) = second pulse (level of pulse 2 and its
//        pitch offset, up to a fifth above), orange (3) = modulation, bipolar: |value| = depth; positive =
//        pulse-width modulation by an internal LFO (0.3 .. 6 Hz), negative = pulse 1 -> pulse 2 cross
//        modulation (pulse 1 moves pulse 2's width).
//
// display[16..]: [16] pulse 1 width Q15 (0.5 = 16384), [17] pulse 2 level Q15, [18] pulse 2 ratio Q16,
//                [19] modulation (signed Q15), [20] cutoff in Hz.
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

u64 isqrt64(u64 x) noexcept {
  u64 r = 0, bit = u64(1) << 62;
  while (bit > x) bit >>= 2;
  while (bit) {
    if (x >= r + bit) {
      x -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

struct PulseEngine {
  static constexpr InstrumentType kType = InstrumentType::Pulse;
  struct Voice {
    u32 ph1 = 0, ph2 = 1u << 30, lfo = 0;
    Svf f;
    s32 dc_x = 0;
    s64 dc_y = 0;
  };

  SvfCoef coef;
  s64 pw_base = s64(1) << 31;  // pulse 1 width, Q32 of a cycle
  s64 pwm_depth = 0;           // Q32 of a cycle
  s64 xmod = 0;                // Q32 of a cycle per unit of pulse 1
  u64 ratio2 = 65536;          // Q16
  s32 lvl2 = 0;                // Q15
  s32 comp1 = 32768;           // Q15 energy compensation of pulse 1
  s32 norm = 0;                // Q15
  s32 mod = 0;
  u32 lfo_inc = 0;
  u32 cutoff_hz = 0;

  void control(const s32 k[kParams]) noexcept {
    static constexpr ExpCurve kCut(120.0, 14000.0);
    static constexpr ExpCurve kLfo(0.3, 6.0);
    const u64 hz = kCut.at(k[0]);
    cutoff_hz = u32(hz >> 16);
    coef = svf_coef(hz_inc(hz), svf_k_q(0.9));
    const s64 a = u15(k[1]);
    pw_base = (s64(1) << 31) - ((a * s64(0.44 * 4294967296.0)) >> 15);
    lvl2 = u15(k[2]);
    ratio2 = ((65536 + ((u64(lvl2) * 32768) >> 15)) * 65733) >> 16;  // 1 .. 1.5, x 1.003
    mod = s15(k[3]);
    const s64 d = mod < 0 ? -mod : mod;
    if (mod >= 0) {
      pwm_depth = (d * s64(0.4 * 4294967296.0)) >> 15;
      xmod = (d * s64(0.04 * 4294967296.0)) >> 15;
    } else {
      pwm_depth = (d * s64(0.08 * 4294967296.0)) >> 15;
      xmod = (d * s64(0.3 * 4294967296.0)) >> 15;
    }
    lfo_inc = hz_inc(kLfo.at(s32(d)));
    // 1 / sqrt(4 pw (1 - pw) + 0.15): narrow pulses carry less energy
    const u64 pw = u64(pw_base);  // Q32
    const u64 x = ((4 * ((pw >> 16) * ((u64(1) << 32) - pw) >> 16)) >> 2) + u64(0.15 * 1073741824.0);  // Q30
    const u64 sq = isqrt64(x);    // Q15
    comp1 = s32(clampv<u64>((u64(1) << 30) / (sq ? sq : 1), 0, 65535));
    norm = s32(32768 * 100 / (100 + ((lvl2 * 60) >> 15)));  // 1 / (1 + 0.6 lvl2)
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (legato) return;
    v.ph1 = 0;
    v.ph2 = 1u << 30;
    v.lfo = 0;
  }

  static constexpr s64 kDcPole = s64((1.0 - 2.0 * 3.14159265358979 * 8.0 / 44100.0) * 2147483648.0);

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    constexpr s64 kLo = s64(0.04 * 4294967296.0), kHi = s64(0.96 * 4294967296.0);
    const u32 inc2 = inc_mul(inc, ratio2);
    for (int i = 0; i < n; ++i) {
      const s32 l = sin_q31(v.lfo);
      v.lfo += lfo_inc;
      const s64 pw1 = clampv<s64>(pw_base + ((s64(l) * pwm_depth) >> 31), kLo, kHi);
      // A pulse of width w averages 2w - 1 (pulse_blep is +1 while ph < w): remove it, so narrow pulses do not
      // push the voice off centre (t2, 2026-10-23: the default patch sat at -0.14 FS DC with 3 voices, -0.28 with
      // 6, which ate headroom and clicked at note on/off). In Q31, 2w - 1 = w - 2^31 for w in Q32. Kept in 64 bits:
      // the high part of a narrow pulse minus its DC exceeds full scale.
      const s64 s1 = ((s64(pulse_blep(v.ph1, inc, u32(pw1))) - (pw1 - (s64(1) << 31))) * comp1) >> 15;
      const s64 pw2 = clampv<s64>((s64(1) << 31) - ((s64(l) * pwm_depth) >> 31) + ((s64(sat(s1)) * xmod) >> 31), kLo, kHi);
      const s64 s2 = ((s64(pulse_blep(v.ph2, inc2, u32(pw2))) - (pw2 - (s64(1) << 31))) * lvl2) >> 15;
      v.ph1 += inc;
      v.ph2 += inc2;
      const s32 x = mul15(sat((s1 * 5 + s2 * 3) >> 5), norm);  // 0.62 / 0.38 mix, quartered (gain restored below)
      const s32 y = v.f.tick(coef, x);
      const s32 z = soft_clip(s64(y) * 4);
      // DC blocker (one pole at ~8 Hz): the soft clip of a narrow, spiky pulse is asymmetric
      v.dc_y = s64(z) - v.dc_x + ((v.dc_y * kDcPole) >> 31);
      v.dc_x = z;
      out[i] = sat(v.dc_y);
    }
  }

  void view(int32_t* d) const noexcept {
    d[0] = int32_t(pw_base >> 17);
    d[1] = lvl2;
    d[2] = int32_t(ratio2);
    d[3] = mod;
    d[4] = int32_t(cutoff_hz);
  }
};

}  // namespace

Instrument* make_pulse(void* mem, size_t size) noexcept { return make_synth<PulseEngine>(mem, size); }

}  // namespace op1::dsp
