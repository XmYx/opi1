// Fixed-point DSP toolkit of the engine track (t4). Everything here is integer arithmetic with defined
// overflow (op1/fract.h semantics): Q1.31 audio, uint32 phase accumulators (one cycle = 2^32), and
// curves read from tables that dsp_init() builds once on the main thread. Nothing allocates.
//
// Formats used throughout src/dsp/:
//   Q31   int32, 1.0 = 2^31 (kOne = 2^31 - 1). Audio and gains <= 1.
//   Q16   fixed 16.16 (uint64/int64 when it can exceed 2^31): Hz, seconds, octaves, ratios.
//   Q8    semitones * 256 (pitch).
//   u15   a stored parameter clamped to 0..32767, read as a fraction 0..1 (Q15).
//   s15   a stored bipolar parameter clamped to -32767..32767.
#pragma once

#include <cstddef>
#include <cstdint>

#include "op1/config.h"
#include "op1/fract.h"

namespace op1::dsp {

using s32 = int32_t;
using u32 = uint32_t;
using s64 = int64_t;
using u64 = uint64_t;

inline constexpr s32 kOne = INT32_MAX;     // ~1.0 in Q31
inline constexpr s32 kHalf = 1 << 30;      // 0.5 in Q31
inline constexpr u32 kHalfCycle = 1u << 31;

// ---------------------------------------------------------------- tables (tables.cpp, built by init_tables)
inline constexpr int kSinBits = 12, kSinSize = 1 << kSinBits;  // 4096 points per cycle
extern s32 g_sin[kSinSize + 1];                 // sin(2 pi i / 4096), Q31
extern u32 g_pow2[1025];                        // 2^(i/1024), Q30
extern u64 g_note_inc[3072];                    // phase increment of MIDI note i/256 (octave 0), Q16
extern s32 g_tan[4097];                         // tan(pi * i / 8192) (0 .. fs/2), Q16, capped
extern s32 g_onepole[4097];                     // 1 - exp(-2 pi f/fs) for f = i/8192 fs, Q31
extern s32 g_tanh[4097];                        // tanh(i / 512) for 0..8, Q31
extern u32 g_env_attack[258];                   // ADSR attack increment per sample, indexed by value >> 6
extern s32 g_env_coef[258];                     // ADSR decay/release coefficient per sample, Q31
extern s32 g_glide[258];                        // portamento coefficient per 64-frame block, Q31
void init_tables() noexcept;

// ---------------------------------------------------------------- basic arithmetic
constexpr s32 sat(s64 x) noexcept { return bf::sat32(x); }
constexpr s32 add(s32 a, s32 b) noexcept { return bf::add_fr1x32(a, b); }
constexpr s32 sub(s32 a, s32 b) noexcept { return bf::sub_fr1x32(a, b); }
// Q31 x Q31 -> Q31, truncated, saturated (only -1 x -1 saturates).
constexpr s32 mulq(s32 a, s32 b) noexcept { return bf::mul_q31_full(a, b); }
// Q31 signal x Q15 gain (0..65535 allowed: gains up to 2.0), saturated.
constexpr s32 mul15(s32 a, s32 g15) noexcept { return sat((s64(a) * g15) >> 15); }
// a + (b - a) * t, t in Q15 (0..32768).
constexpr s32 lerp15(s32 a, s32 b, s32 t15) noexcept { return sat(s64(a) + ((s64(b) - a) * t15 >> 15)); }
constexpr s32 lerp31(s32 a, s32 b, s32 t31) noexcept { return sat(s64(a) + ((s64(b) - a) * t31 >> 31)); }
// Linear interpolation between two samples, f in Q16 (0..65535). Done in 64 bits: the result lies between a
// and b, but the intermediate a + (b - a) f can overflow int32 when a and b are far apart.
constexpr s32 interp16(s32 a, s32 b, s32 f16) noexcept { return s32(s64(a) + (((s64(b) - a) * f16) >> 16)); }

template <class T> constexpr T clampv(T v, T lo, T hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
constexpr s32 u15(s32 v) noexcept { return clampv<s32>(v, 0, 32767); }
constexpr s32 s15(s32 v) noexcept { return clampv<s32>(v, -32767, 32767); }
// Q15 fraction -> Q31.
constexpr s32 q15to31(s32 v15) noexcept { return v15 >= 32767 ? kOne : (v15 <= -32767 ? -kOne : v15 * 65536); }
// Index into `count` equal bins from a u15 value (the LFO/effect "choice" encoders).
constexpr int choose(s32 v, int count) noexcept {
  const int i = int((s64(u15(v)) * count) >> 15);
  return i >= count ? count - 1 : i;
}
// Square of a Q15 fraction (a cheap "more resolution at the bottom" curve), Q15.
constexpr s32 sq15(s32 v15) noexcept { return s32((s64(v15) * v15) >> 15); }

// ---------------------------------------------------------------- oscillator primitives
inline s32 sin_q31(u32 phase) noexcept {
  const u32 i = phase >> (32 - kSinBits);
  const s32 f = s32((phase >> (32 - kSinBits - 16)) & 0xffff);
  const s32 a = g_sin[i], b = g_sin[i + 1];
  return a + s32((s64(b) - a) * f >> 16);
}
inline s32 cos_q31(u32 phase) noexcept { return sin_q31(phase + (1u << 30)); }

// Phase offset for phase modulation: a Q31 signal m times an index in Q16 cycles -> phase (wraps).
inline u32 pm_offset(s64 m, s64 index_q16) noexcept { return u32(u64((m * index_q16) >> 15)); }

// PolyBLEP residual at phase t for increment dt (Q31): 2x - x^2 - 1 just after the edge, x^2 + 2x + 1
// just before it (x = t/dt in cycles).
inline s32 poly_blep(u32 t, u32 dt) noexcept {
  if (dt == 0) return 0;
  if (t < dt) {
    const s64 x = s64((u64(t) << 16) / dt);               // Q16, 0..1
    return sat((2 * x - ((x * x) >> 16) - 65536) << 15);
  }
  const u32 back = u32(0u - t);                           // 2^32 - t
  if (back < dt) {
    const s64 x = -s64((u64(back) << 16) / dt);           // Q16, -1..0
    return sat((((x * x) >> 16) + 2 * x + 65536) << 15);
  }
  return 0;
}
// Band-limited saw (rising, -1..1) and pulse (+1 for phase < width), Q31.
inline s32 saw_blep(u32 ph, u32 dt) noexcept { return sat(s64(s32(ph ^ 0x80000000u)) - poly_blep(ph, dt)); }
inline s32 pulse_blep(u32 ph, u32 dt, u32 width) noexcept {
  s64 s = ph < width ? kOne : -s64(kOne);
  s += poly_blep(ph, dt);
  s -= poly_blep(ph - width, dt);
  return sat(s);
}
inline s32 tri_q31(u32 ph) noexcept {
  const u32 d = ph >= kHalfCycle ? ph - kHalfCycle : kHalfCycle - ph;  // |ph - 0.5| in cycles * 2^32
  return sat(s64(kHalfCycle) - 2 * s64(d));
}

// Deterministic noise (32-bit LCG), full-scale Q31.
struct Rng {
  u32 s = 22222;
  constexpr Rng() noexcept = default;
  constexpr explicit Rng(u32 seed) noexcept : s(seed) {}
  s32 next() noexcept { s = s * 1664525u + 1013904223u; return s32(s); }
  u32 nextu() noexcept { s = s * 1664525u + 1013904223u; return s; }
};

// tanh soft clip: unity slope at 0, bounded to +-1. Input Q31 as int64 (may exceed +-1; saturates at 8).
inline s32 soft_clip(s64 x) noexcept {
  const bool neg = x < 0;
  const u64 a = u64(neg ? -x : x);
  s32 y;
  if (a >= (u64(8) << 31)) {
    y = kOne;
  } else {
    const u32 i = u32(a >> 22);                     // 512 steps per unit
    const s32 f = s32((a >> 6) & 0xffff);
    y = g_tanh[i] + s32((s64(g_tanh[i + 1]) - g_tanh[i]) * f >> 16);
  }
  return neg ? -y : y;
}

// ---------------------------------------------------------------- pitch and curves
// 2^x, x in Q16 (signed, -30..30), result Q16 (u64).
inline u64 pow2_q16(s64 x) noexcept {
  if (x < -(s64(30) << 16)) return 0;
  if (x > (s64(30) << 16)) x = s64(30) << 16;
  const s64 ip = x >> 16;                         // floor
  const u32 fr = u32(x & 0xffff);                 // 0..65535
  const u32 i = fr >> 6, f = fr & 63;
  const u64 v = u64(g_pow2[i]) + ((u64(g_pow2[i + 1] - g_pow2[i]) * f) >> 6);  // Q30
  const s64 sh = ip - 14;                         // Q30 -> Q16, then * 2^ip
  return sh >= 0 ? (v << sh) : (v >> (-sh));
}

inline constexpr u32 kMaxInc = u32(0.45 * 4294967296.0);  // keep oscillators below 0.45 fs

// Phase increment for a pitch in Q8 semitones above MIDI note 0 (8.18 Hz).
inline u32 note_inc(s32 semis_q8) noexcept {
  semis_q8 = clampv<s32>(semis_q8, 0, 140 * 256);
  const s32 oct = semis_q8 / 3072, r = semis_q8 % 3072;
  const u64 inc = (g_note_inc[r] << oct) >> 16;
  return inc > kMaxInc ? kMaxInc : u32(inc);
}
// Phase increment for a frequency in Hz Q16.
inline u32 hz_inc(u64 hz_q16) noexcept {
  constexpr u64 k = u64((281474976710656.0 / kSampleRate) + 0.5);  // 2^48 / fs
  const u64 inc = (hz_q16 * k) >> 32;
  return inc > kMaxInc ? kMaxInc : u32(inc);
}
// Multiply an increment by a ratio in Q16.
inline u32 inc_mul(u32 inc, u64 ratio_q16) noexcept {
  const u64 v = (u64(inc) * ratio_q16) >> 16;
  return v > kMaxInc ? kMaxInc : u32(v);
}
// Ratio of a pitch offset in Q8 semitones (+-), Q16.
inline u64 semis_ratio_q16(s32 semis_q8) noexcept { return pow2_q16((s64(semis_q8) << 16) / 3072); }
// Cents (signed) -> ratio Q16.
inline u64 cents_ratio_q16(s32 cents) noexcept { return pow2_q16((s64(cents) << 16) / 1200); }

namespace detail {
constexpr double ln_c(double x) {  // compile-time natural log (x > 0)
  int e = 0;
  while (x > 2.0) { x *= 0.5; ++e; }
  while (x < 1.0) { x *= 2.0; --e; }
  const double z = (x - 1) / (x + 1), z2 = z * z;
  double term = z, sum = 0;
  for (int k = 1; k < 80; k += 2) { sum += term / k; term *= z2; }
  return 2 * sum + e * 0.69314718055994530942;
}
}  // namespace detail

// lo * (hi/lo)^v for a u15 fraction v: the "exponential knob" curve. Values in Q16 (Hz, seconds, ...).
struct ExpCurve {
  u64 lo_q16;
  s64 oct_q16;
  constexpr ExpCurve(double lo, double hi)
      : lo_q16(u64(lo * 65536.0 + 0.5)), oct_q16(s64(detail::ln_c(hi / lo) / 0.69314718055994530942 * 65536.0)) {}
  u64 at(s32 v15) const noexcept { return (lo_q16 * pow2_q16((oct_q16 * u15(v15)) >> 15)) >> 16; }
  u32 inc(s32 v15) const noexcept { return hz_inc(at(v15)); }  // for frequency curves
};

// ---------------------------------------------------------------- filters
// Topology-preserving state-variable filter (Zavalishin), fixed point. Coefficients come from the cutoff
// as a phase increment and the damping k = 1/Q in Q29 (2.0 .. 1/32). The state runs with 3 bits of
// headroom (Q4.28) and saturates, so self-oscillation clips instead of wrapping.
struct SvfCoef {
  s32 a1 = kOne, a2 = 0, a3 = 0, k = 1 << 29;
};
inline s32 tan_q16(u32 inc) noexcept {
  constexpr u32 kTop = kMaxInc + (kMaxInc >> 6);
  if (inc > kTop) inc = kTop;
  const u32 i = inc >> 19, f = (inc >> 3) & 0xffff;
  return g_tan[i] + s32((s64(g_tan[i + 1]) - g_tan[i]) * s64(f) >> 16);
}
inline SvfCoef svf_coef(u32 cutoff_inc, s32 k_q29) noexcept {
  SvfCoef c;
  const s64 g = tan_q16(cutoff_inc < 4000 ? 4000 : cutoff_inc);
  k_q29 = clampv<s32>(k_q29, 1 << 24, 1 << 30);
  const s64 kq16 = k_q29 >> 13;
  const s64 den = 65536 + ((g * (g + kq16)) >> 16);
  const s64 a1 = (s64(1) << 47) / den;
  c.a1 = s32(a1 > kOne ? kOne : a1);
  c.a2 = s32((g * c.a1) >> 16);
  c.a3 = s32((g * c.a2) >> 16);
  c.k = k_q29;
  return c;
}
// Damping from a u15 "resonance" value: k = 2.0 (Q 0.5) at 0 down to 1/16 (Q 16) at 32767, squared curve.
inline s32 svf_k_from_res(s32 res15) noexcept {
  const s64 r = sq15(u15(res15));
  return s32((s64(1) << 30) - ((r * ((s64(1) << 30) - (s64(1) << 25))) >> 15));
}
// Damping for a fixed Q (compile time).
constexpr s32 svf_k_q(double q) { return s32((1.0 / q) * 536870912.0); }
struct Svf {
  s32 ic1 = 0, ic2 = 0, lp = 0, bp = 0, hp = 0;
  s32 tick(const SvfCoef& c, s32 x) noexcept {
    const s32 x8 = x >> 3;
    const s64 v3 = sat(s64(x8) - ic2);
    const s64 v1 = (s64(c.a1) * ic1 + s64(c.a2) * v3) >> 31;
    const s64 v2 = s64(ic2) + ((s64(c.a2) * ic1 + s64(c.a3) * v3) >> 31);
    ic1 = sat(2 * v1 - ic1);
    ic2 = sat(2 * v2 - ic2);
    lp = sat(v2 * 8);
    bp = sat(v1 * 8);
    hp = sat((s64(x8) - ((s64(c.k) * v1) >> 29) - v2) * 8);
    return lp;
  }
  void reset() noexcept { ic1 = ic2 = lp = bp = hp = 0; }
};

// One-pole low-pass y += (x - y) * a, a from the cutoff increment.
inline s32 onepole_coef(u32 cutoff_inc) noexcept {
  if (cutoff_inc > kMaxInc) cutoff_inc = kMaxInc;
  const u32 i = cutoff_inc >> 19, f = (cutoff_inc >> 3) & 0xffff;
  return g_onepole[i] + s32((s64(g_onepole[i + 1]) - g_onepole[i]) * s64(f) >> 16);
}
struct OnePole {
  s32 y = 0;
  s32 tick(s32 a, s32 x) noexcept { y = sat(s64(y) + (((s64(x) - y) * a) >> 31)); return y; }
};

// DC blocker: y = x - x1 + r y1 (r ~ 0.9965, ~25 Hz).
struct DcBlock {
  s32 x1 = 0, y1 = 0;
  s32 tick(s32 x) noexcept {
    constexpr s32 r = s32(0.9965 * 2147483648.0);
    const s32 y = sat(s64(x) - x1 + ((s64(r) * y1) >> 31));
    x1 = x;
    y1 = y;
    return y;
  }
};

// Envelope follower (peak, separate attack/release coefficients in Q31).
struct Follower {
  s32 v = 0;
  s32 tick(s32 x, s32 ka, s32 kr) noexcept {
    const s32 a = x < 0 ? (x == INT32_MIN ? kOne : -x) : x;
    v = sat(s64(v) + (((s64(a) - v) * (a > v ? ka : kr)) >> 31));
    return v;
  }
};
// Per-sample one-pole coefficient for a time constant in milliseconds (compile time).
constexpr s32 coef_ms(double ms) {
  const double x = 1000.0 / (ms * kSampleRate);  // 1 - e^-x by its series (x small)
  const double v = x - x * x / 2 + x * x * x / 6 - x * x * x * x / 24;
  return s32(v * 2147483648.0);
}

// Circular delay line of N samples (N a power of two), fractional reads in Q16 samples. T = int32_t keeps
// full Q31 samples; T = int16_t stores Q15 (half the memory, like a 16-bit delay RAM) for long lines.
template <int N, class T = s32>
struct DelayLine {
  static_assert((N & (N - 1)) == 0, "power of two");
  T buf[N] = {};
  int w = 0;
  static constexpr s32 load(T v) noexcept {
    if constexpr (sizeof(T) == 2) return s32(v) * 65536;
    else return s32(v);
  }
  static constexpr T store(s32 x) noexcept {
    if constexpr (sizeof(T) == 2) return T(x >> 16);
    else return T(x);
  }
  void write(s32 x) noexcept { buf[w] = store(x); w = (w + 1) & (N - 1); }
  // Integer delay d (1..N-1) behind the last write.
  s32 read(int d) const noexcept { return load(buf[(w - d) & (N - 1)]); }
  // Fractional delay in Q16 samples, clamped to 1..N-2.
  s32 readf(s64 d_q16) const noexcept {
    d_q16 = clampv<s64>(d_q16, s64(1) << 16, s64(N - 2) << 16);
    const int di = int(d_q16 >> 16);
    const s32 f = s32(d_q16 & 0xffff);
    return interp16(load(buf[(w - di) & (N - 1)]), load(buf[(w - di - 1) & (N - 1)]), f);
  }
  void clear() noexcept {
    for (auto& v : buf) v = 0;
    w = 0;
  }
};

// Milliseconds to samples in Q16 (compile time).
constexpr s64 ms_q16(double ms) { return s64(ms * kSampleRate / 1000.0 * 65536.0); }

}  // namespace op1::dsp
