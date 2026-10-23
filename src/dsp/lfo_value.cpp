// value (LFO db id 2, LfoSingle.cpp): a shaped LFO sent to one destination. Designed by ear (the firmware
// routine is not decoded) on the OG facts: defaults 1000/4000/2000/2000; slot 1 is signed (depth with sign).
//
// Params: blue (0) = speed (0.05 .. 20 Hz, or a tempo division when synced), green (1) = amount (signed),
//         white (2) = destination group (engine / envelope / fx / main), orange (3) = parameter in the group.
//         Shift page: (4) fine speed (signed, +-1/4 octave), (5) shape (square, ramp, saw, sine),
//         (6) sync (FREE, NOTE = restart on each note, TEMPO = locked to tempo divisions), (7) phase offset.
//
// lfo_display: [0] phase (Q16 of a cycle), [1] LFO value Q15, [2] destination group 0..3,
//              [3] destination index, [4] shape 0..3, [5] sync 0..2, [6] applied amount Q15.
#include <new>

#include "dsp_internal.h"

namespace op1::dsp {
namespace {

// Waveform at a phase, Q15 (-32767..32767).
s32 shape_q15(int shape, u32 ph) noexcept {
  switch (shape) {
    case 0: return ph < kHalfCycle ? 32767 : -32767;                 // square
    case 1: return s32(ph ^ 0x80000000u) >> 16;                      // ramp (rising)
    case 2: return -(s32(ph ^ 0x80000000u) >> 16);                   // saw (falling)
    default: return sin_q31(ph) >> 16;                               // sine
  }
}

class ValueLfo final : public Lfo {
 public:
  void process(const Param p[kParams], const LfoInputs& in, ModOut& out) noexcept override {
    sync_ = choose(p[6], 3);
    shape_ = choose(p[5], 4);
    if (sync_ == 1 && in.note_ons > 0) ph_ = 0;
    u32 inc;
    if (sync_ == 2) {
      inc = lfo_sync_inc(p[0], in.bpm_tenths);
    } else {
      const u64 fine = pow2_q16((s64(s15(p[4])) << 16) / (4 * 32767));  // +-1/4 octave
      inc = u32((u64(lfo_rate_inc(p[0])) * fine) >> 16);
    }
    ph_ += inc;
    const u32 at = ph_ + (u32(u15(p[7])) << 17);
    value_ = shape_q15(shape_, at);
    target_ = target_of(p[2], p[3]);
    amount_ = s32(s64(value_) * s15(p[1]) / 32767 / 2);  // full amount = half the parameter range
    add_mod(out, target_, amount_);
  }
  void reset() noexcept override { ph_ = 0; }
  void view(int32_t d[16]) const noexcept override {
    d[0] = int32_t(ph_ >> 16);
    d[1] = value_;
    d[2] = int32_t(target_.group);
    d[3] = target_.index;
    d[4] = shape_;
    d[5] = sync_;
    d[6] = amount_;
  }

 private:
  u32 ph_ = 0;
  s32 value_ = 0, amount_ = 0;
  int shape_ = 0, sync_ = 0;
  ModTarget target_;
};

}  // namespace

Lfo* make_lfo_value(void* mem, size_t size) noexcept {
  return sizeof(ValueLfo) <= size ? new (mem) ValueLfo() : nullptr;
}

}  // namespace op1::dsp
