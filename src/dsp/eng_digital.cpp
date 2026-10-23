// digital (id 1, T10.cpp in the firmware): a wave-folded sine with a second oscillator, ring modulation and
// bit/sample-rate reduction. Designed by ear (the firmware routine is not decoded) on the OG facts:
// defaults 2000/5000/-1000/2000, slot 2 is bipolar in the factory data (-32768..32767).
//
// Knobs: blue (0) = wave shaper (the sine is overdriven up to 6x and folded back by a second sine),
//        green (1) = octave of oscillator 2 (-2..+2), white (2) = bipolar: below 0 detunes oscillator 2 up
//        to 40 cents, above 0 ring-modulates the two, orange (3) = digitalness (16 .. 3 bits and a
//        sample-rate reduction up to 25x).
//
// display[16..]: [16] osc 2 octave -2..+2, [17] detune cents, [18] ring Q15, [19] bit depth 3..16,
//                [20] sample-hold factor 1..25, [21] fold factor Q16.
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

struct DigitalEngine {
  static constexpr InstrumentType kType = InstrumentType::Digital;
  struct Voice {
    u32 ph1 = 0, ph2 = 1u << 30;
    s32 hold = 0;
    int count = 0;
  };

  s64 fold_q16 = 65536;
  int oct = 0, bits = 16, hold = 1;
  s32 cents = 0, ring15 = 0;
  u64 ratio2 = 65536;

  void control(const s32 k[kParams]) noexcept {
    fold_q16 = 65536 + ((s64(u15(k[0])) * 327680) >> 15);  // 1 .. 6
    oct = int((s64(u15(k[1])) * 4 + 16383) >> 15) - 2;     // rounded -2..+2
    const s32 b = s15(k[2]);
    cents = b < 0 ? s32(s64(-b) * 40 / 32767) : 0;
    ring15 = b > 0 ? b : 0;
    const s32 dig = u15(k[3]);
    bits = 16 - int((s64(dig) * 13) >> 15);
    hold = 1 + int((s64(sq15(dig)) * 24) >> 15);
    ratio2 = (cents_ratio_q16(cents) * pow2_q16(s64(oct) << 16)) >> 16;
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (legato) return;
    v.ph1 = 0;
    v.ph2 = 1u << 30;
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    const u32 inc2 = inc_mul(inc, ratio2);
    const s64 fq = fold_q16 >> 2;  // sin(x * fold * pi/2): phase = x * fold / 4 cycles
    const u32 mask = bits >= 16 ? 0xffff0000u : ~((1u << (32 - bits)) - 1);
    const s32 half = s32(~mask >> 1);
    for (int i = 0; i < n; ++i) {
      const s32 s1 = sin_q31(pm_offset(sin_q31(v.ph1), fq));
      const s32 s2 = sin_q31(pm_offset(sin_q31(v.ph2), fq));
      v.ph1 += inc;
      v.ph2 += inc2;
      const s64 mix = (s64(s1) + s2) / 2;
      const s64 rm = (s64(s1) * s2) >> 31;
      const s64 x = mix + (((rm - mix) * ring15) >> 15);
      if (--v.count <= 0) {
        // quantise to `bits` bits, rounded
        const s32 q = sat(x + half);
        v.hold = s32(u32(q) & mask);
        v.count = hold;
      }
      out[i] = soft_clip((s64(v.hold) * 3) >> 2);
    }
  }

  void view(int32_t* d) const noexcept {
    d[0] = oct;
    d[1] = cents;
    d[2] = ring15;
    d[3] = bits;
    d[4] = hold;
    d[5] = int32_t(fold_q16);
  }
};

}  // namespace

Instrument* make_digital(void* mem, size_t size) noexcept { return make_synth<DigitalEngine>(mem, size); }

}  // namespace op1::dsp
