// random (LFO db id 3, LfoRnd.cpp): smoothed sample-and-hold noise to one destination. Designed by ear on
// the OG facts: defaults 2000/4000/2000/24000.
//
// Params: blue (0) = speed (0.05 .. 20 new values per second), green (1) = amount (signed),
//         white (2) = destination (any of the 18 flat targets), orange (3) = envelope (0 = constant depth,
//         up = the depth follows the newest voice's amplitude envelope).
//
// lfo_display: [0] phase (Q16), [1] current smoothed value Q15, [2] next value Q15, [3] flat target 0..17,
//              [4] applied amount Q15.
#include <new>

#include "dsp_internal.h"

namespace op1::dsp {
namespace {

class RandomLfo final : public Lfo {
 public:
  void process(const Param p[kParams], const LfoInputs& in, ModOut& out) noexcept override {
    const u32 next_ph = ph_ + lfo_rate_inc(p[0]);
    if (next_ph < ph_ || first_) {  // wrapped: step to a new random value
      cur_ = next_;
      next_ = rng_.next() >> 16;
      first_ = false;
    }
    ph_ = next_ph;
    // glide a little toward each new value (reaches it after 1/6 of the period)
    const s64 t = clampv<s64>((s64(ph_) * 6) >> 17, 0, 32768);  // ph * 6 in Q15
    const s32 target = s32(cur_ + ((s64(next_) - cur_) * t >> 15));
    out_ += (target - out_) / 2;
    const s32 env = u15(p[3]);
    const s32 e15 = in.envelope >> 16;
    const s32 depth = s32((s64(32767 - env) * 32767 + s64(env) * e15) >> 15);  // 1 - env + env * envelope
    target_ = flat_target(p[2]);
    amount_ = s32(s64(out_) * s15(p[1]) / 32767 * depth / 32767);
    add_mod(out, target_, amount_);
  }
  void reset() noexcept override {
    ph_ = 0;
    out_ = 0;
    first_ = true;
  }
  void view(int32_t d[16]) const noexcept override {
    d[0] = int32_t(ph_ >> 16);
    d[1] = out_;
    d[2] = next_;
    d[3] = int32_t(target_.group) == 0 ? target_.index
           : int32_t(target_.group) == 1 ? 8 + target_.index
           : int32_t(target_.group) == 2 ? 12 + target_.index
                                         : 16 + target_.index;
    d[4] = amount_;
  }

 private:
  u32 ph_ = 0;
  bool first_ = true;
  s32 cur_ = 0, next_ = 0, out_ = 0, amount_ = 0;
  Rng rng_{1234567};
  ModTarget target_;
};

}  // namespace

Lfo* make_lfo_random(void* mem, size_t size) noexcept {
  return sizeof(RandomLfo) <= size ? new (mem) RandomLfo() : nullptr;
}

}  // namespace op1::dsp
