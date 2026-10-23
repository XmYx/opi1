// iter (id 14, Iter.cpp in the firmware): the OG's hidden engine. R.246 has the full engine and screen but no
// synth_types row, so the UI never offers it (firmware-behaviour.md §11); here it is a normal engine.
//
// Teenage Engineering never documented it and its DSP routine (hand-written Blackfin) has not been decoded
// (og-behaviour-details.md §7), so the sound is designed by ear on the only clues: the name, community
// reports that it is feedback-based, and the community-mod default row 1516/16704/0/15168 (used as the type
// default here; it equals string's).
//
// Model: an iterated sine map. A harmonic oscillator phase-modulates a sine carrier at the note; feedback
// feeds the previous output back into the harmonic oscillator; on the upper half of the iteration knob the
// result runs through the carrier once more. A slightly detuned twin is mixed in by the spread knob (mono).
//
// Knobs: blue (0) = iteration (modulation index 0.05 .. 0.95 cycles; third pass fades in above 50 %),
//        green (1) = harmonic (ratio of the modulating pass, 0.5 .. 4, squared curve), white (2) = feedback
//        (0 .. 0.42 cycles of the last output), orange (3) = spread (twin detuned up to +1.2 %, mixed to 50 %).
//
// display[16..]: [16] index (Q16 cycles), [17] harmonic ratio x 100, [18] feedback (Q16 cycles),
//                [19] spread Q15, [20] third-pass mix Q15.
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

struct IterEngine {
  static constexpr InstrumentType kType = InstrumentType::Iter;
  struct Voice {
    u32 pa = 0, ha = 0, pb = 1u << 30, hb = 1u << 30;
    s32 ya = 0, yb = 0;
  };

  s64 depth = 0, fb = 0;   // Q16 cycles
  u64 ratio = 65536;       // Q16
  u64 twin = 65536;        // Q16 detune of the twin
  s32 spread = 0, third = 0;

  void control(const s32 k[kParams]) noexcept {
    const s32 it = u15(k[0]);
    depth = 3277 + ((s64(it) * 58982) >> 15);                 // 0.05 + 0.9 it
    third = it > 16384 ? clampv<s32>((it - 16384) * 2, 0, 32767) : 0;
    ratio = 32768 + ((u64(sq15(u15(k[1]))) * 229376) >> 15);  // 0.5 + 3.5 h^2
    fb = (s64(u15(k[2])) * 27525) >> 15;                      // 0.42 cycles
    spread = u15(k[3]);
    twin = 65536 + ((u64(spread) * 786) >> 15);               // + 1.2 %
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (legato) return;
    v = Voice{};
  }

  s32 pass(u32& ph, u32& h, s32& y, u32 inc, u32 hinc) const noexcept {
    const s32 m = sin_q31(h + pm_offset(y, fb));
    s32 a = sin_q31(ph + pm_offset(m, depth));
    if (third) a = sin_q31(ph + pm_offset(lerp15(m, a, third), depth));
    y = a;
    ph += inc;
    h += hinc;
    return a;
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    const u32 incb = inc_mul(inc, twin);
    const u32 ha = inc_mul(inc, ratio), hb = inc_mul(incb, ratio);
    const s32 wa = 32767 - spread / 2, wb = spread / 2;
    for (int i = 0; i < n; ++i) {
      const s32 a = pass(v.pa, v.ha, v.ya, inc, ha);
      const s32 b = pass(v.pb, v.hb, v.yb, incb, hb);
      out[i] = soft_clip((((s64(a) * wa) >> 15) + ((s64(b) * wb) >> 15)) * 7 / 8);
    }
  }

  void view(int32_t* d) const noexcept {
    d[0] = int32_t(depth);
    d[1] = int32_t(ratio * 100 >> 16);
    d[2] = int32_t(fb);
    d[3] = spread;
    d[4] = third;
  }
};

}  // namespace

Instrument* make_iter(void* mem, size_t size) noexcept { return make_synth<IterEngine>(mem, size); }

}  // namespace op1::dsp
