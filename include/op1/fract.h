// Blackfin fractional arithmetic with exact integer semantics.
//
// The OG's engines are hand-written ADSP-BF524 code with no FPU, so a 1:1 port is integer code that
// reproduces each instruction's result bit for bit. Function names follow Analog Devices' fract.h /
// fract_math.h built-ins so translated code reads like the original; each comment states the exact
// integer behaviour, which is what counts. All functions are constexpr, branch-light and noexcept, and
// never rely on signed overflow (undefined in C++): wrapping ops go through unsigned arithmetic.
//
// Owner: t3. t4 adds the instruction sequences it meets in the firmware (for example the 3-multiply
// 32x32 fractional product and the ASTAT.RND_MOD rounding mode) here, each with a parity test.
#pragma once

#include <cstdint>

namespace op1::bf {

using fract16 = int16_t;  // Q1.15
using fract32 = int32_t;  // Q1.31

constexpr fract16 sat16(int32_t x) noexcept { return x > 32767 ? 32767 : (x < -32768 ? -32768 : fract16(x)); }
constexpr fract32 sat32(int64_t x) noexcept {
  return x > INT32_MAX ? INT32_MAX : (x < INT32_MIN ? INT32_MIN : fract32(x));
}

// ---- 16-bit
constexpr fract16 add_fr1x16(fract16 a, fract16 b) noexcept { return sat16(int32_t(a) + b); }
constexpr fract16 sub_fr1x16(fract16 a, fract16 b) noexcept { return sat16(int32_t(a) - b); }
constexpr fract16 negate_fr1x16(fract16 a) noexcept { return sat16(-int32_t(a)); }
constexpr fract16 abs_fr1x16(fract16 a) noexcept { return sat16(a < 0 ? -int32_t(a) : a); }
// (a*b) << 1 saturated to Q1.31 (only -1 * -1 overflows), then the high half, truncated.
constexpr fract16 mult_fr1x16(fract16 a, fract16 b) noexcept { return sat16((int32_t(a) * b) >> 15); }
// As mult_fr1x16 but rounded half up (Blackfin biased rounding, ASTAT.RND_MOD = 1).
constexpr fract16 multr_fr1x16(fract16 a, fract16 b) noexcept { return sat16((int32_t(a) * b + 0x4000) >> 15); }
// Unbiased (convergent, round-half-even) rounding, the Blackfin reset default (ASTAT.RND_MOD = 0).
constexpr fract16 multr_fr1x16_unbiased(fract16 a, fract16 b) noexcept {
  const int32_t p = int32_t(a) * b;                  // Q2.30
  int32_t q = p >> 15;
  const int32_t rem = p & 0x7fff;
  if (rem > 0x4000 || (rem == 0x4000 && (q & 1))) ++q;
  return sat16(q);
}

// ---- 32-bit
constexpr fract32 add_fr1x32(fract32 a, fract32 b) noexcept { return sat32(int64_t(a) + b); }
constexpr fract32 sub_fr1x32(fract32 a, fract32 b) noexcept { return sat32(int64_t(a) - b); }
constexpr fract32 negate_fr1x32(fract32 a) noexcept { return sat32(-int64_t(a)); }
constexpr fract32 abs_fr1x32(fract32 a) noexcept { return sat32(a < 0 ? -int64_t(a) : a); }
// Non-saturating (NS) add/sub wrap modulo 2^32 like the hardware.
constexpr fract32 add_ns32(fract32 a, fract32 b) noexcept { return fract32(uint32_t(a) + uint32_t(b)); }
constexpr fract32 sub_ns32(fract32 a, fract32 b) noexcept { return fract32(uint32_t(a) - uint32_t(b)); }
// 16 x 16 -> Q1.31: (a*b) << 1, saturated.
constexpr fract32 mult_fr1x32(fract16 a, fract16 b) noexcept { return sat32(int64_t(int32_t(a) * b) * 2); }
// Full-precision Q1.31 product, truncated and saturated. Not the same as the Blackfin's 3-multiply
// mult_fr1x32x32 sequence, which drops the low x low partial product: t4 adds that one when needed.
constexpr fract32 mul_q31_full(fract32 a, fract32 b) noexcept { return sat32((int64_t(a) * b) >> 31); }

// Shifts: positive n shifts left with saturation, negative n shifts right arithmetically.
constexpr fract32 shl_fr1x32(fract32 a, int n) noexcept {
  if (n >= 0) return sat32(int64_t(a) * (int64_t(1) << (n > 32 ? 32 : n)));
  return a >> (-n > 31 ? 31 : -n);
}
constexpr fract16 shl_fr1x16(fract16 a, int n) noexcept {
  if (n >= 0) return sat16(int32_t(a) * (1 << (n > 16 ? 16 : n)));
  return fract16(a >> (-n > 15 ? 15 : -n));
}
constexpr fract32 shr_fr1x32(fract32 a, int n) noexcept { return shl_fr1x32(a, -n); }

// Q1.31 -> Q1.15
constexpr fract16 extract_hi(fract32 a) noexcept { return fract16(a >> 16); }
constexpr fract16 round_fr1x32(fract32 a) noexcept { return sat16(int32_t((int64_t(a) + 0x8000) >> 16)); }
constexpr fract32 compose(fract16 hi, uint16_t lo) noexcept { return fract32((uint32_t(uint16_t(hi)) << 16) | lo); }

// Redundant sign bits (Blackfin SIGNBITS): leading bits equal to the sign bit, minus one.
constexpr int norm_fr1x32(fract32 a) noexcept {
  uint32_t v = uint32_t(a < 0 ? ~a : a);
  int n = 0;
  for (uint32_t m = 0x40000000u; m && !(v & m); m >>= 1) ++n;
  return n;
}

// 40-bit accumulator (A0/A1) in fractional mode: MACs add (a*b) << 1 and saturate at 40 bits.
struct Acc40 {
  int64_t v = 0;
  static constexpr int64_t kMax = (int64_t(1) << 39) - 1, kMin = -(int64_t(1) << 39);
  constexpr void sat() noexcept { v = v > kMax ? kMax : (v < kMin ? kMin : v); }
  constexpr void mac(fract16 a, fract16 b) noexcept { v += int64_t(int32_t(a) * b) * 2; sat(); }
  constexpr void msu(fract16 a, fract16 b) noexcept { v -= int64_t(int32_t(a) * b) * 2; sat(); }
  constexpr fract32 to32() const noexcept { return sat32(v); }                        // Rn = A0
  constexpr fract16 to16_trunc() const noexcept { return sat16(int32_t(sat32(v) >> 16)); }  // Rn.L = A0 (T)
  constexpr fract16 to16_round() const noexcept { return sat16(int32_t((v + 0x8000) >> 16)); }  // biased
};

// Test and tooling helpers (not for the audio path of a bit-exact engine).
constexpr double to_double(fract32 a) noexcept { return double(a) / 2147483648.0; }
constexpr double to_double(fract16 a) noexcept { return double(a) / 32768.0; }
constexpr fract32 from_double32(double x) noexcept { return sat32(int64_t(x * 2147483648.0)); }
constexpr fract16 from_double16(double x) noexcept { return sat16(int32_t(x * 32768.0)); }

}  // namespace op1::bf
