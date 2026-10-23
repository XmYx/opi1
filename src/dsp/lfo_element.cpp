// element (LFO db id 4, LfoReRoute.cpp): an outside signal becomes the modulation ("re-route"). Designed by
// ear on the OG facts: defaults 2000/4000/2000/2000; slot 1 is signed. The firmware reads the device's
// accelerometer (src/bf/xyz.cpp) and inputs; here they come from ControlInputs.
//
// Params: blue (0) = source: G-FORCE (accelerometer x), INPUT (live input level), ENVELOPE (newest voice's
//         amplitude envelope), SUM (the instrument's own output level); green (1) = amount (signed);
//         white (2) = destination group; orange (3) = parameter in the group.
//
// lfo_display: [0] source 0..3, [1] raw source value Q15, [2] smoothed value Q15, [3] destination group,
//              [4] destination index, [5] applied amount Q15.
#include <new>

#include "dsp_internal.h"

namespace op1::dsp {
namespace {

class ElementLfo final : public Lfo {
 public:
  void process(const Param p[kParams], const LfoInputs& in, ModOut& out) noexcept override {
    src_ = choose(p[0], 4);
    s32 x = 0;
    switch (src_) {
      case 0: x = in.controls ? s15(in.controls->accel[0]) : 0; break;
      case 1: x = in.controls ? s32(clampv<s64>(s64(in.controls->input_level) >> 15, 0, 32767)) : 0; break;
      case 2: x = in.envelope >> 16; break;
      default: x = s32(clampv<s64>((s64(in.out_level) * 3) >> 17, 0, 32767)); break;  // level * 1.5
    }
    raw_ = x;
    // ~10 ms smoothing: 64 / (0.01 fs) = 0.145 per block
    constexpr s32 kSmooth = s32(0.145 * 32768);
    y_ += s32((s64(x) - y_) * kSmooth >> 15);
    target_ = target_of(p[2], p[3]);
    amount_ = s32(s64(y_) * s15(p[1]) / 32767);
    add_mod(out, target_, amount_);
  }
  void reset() noexcept override { y_ = 0; }
  void view(int32_t d[16]) const noexcept override {
    d[0] = src_;
    d[1] = raw_;
    d[2] = y_;
    d[3] = int32_t(target_.group);
    d[4] = target_.index;
    d[5] = amount_;
  }

 private:
  int src_ = 0;
  s32 raw_ = 0, y_ = 0, amount_ = 0;
  ModTarget target_;
};

}  // namespace

Lfo* make_lfo_element(void* mem, size_t size) noexcept {
  return sizeof(ElementLfo) <= size ? new (mem) ElementLfo() : nullptr;
}

}  // namespace op1::dsp
