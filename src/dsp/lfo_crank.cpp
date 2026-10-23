// crank (LFO db id 7, LfoCrank.cpp): made for the Crank accessory. Spinning the blue encoder like a hand
// crank drives a sine LFO: the faster it turns, the faster and deeper the swing, and it winds down by
// itself. Designed by ear on the OG facts: defaults 0/16384/2000/2000.
//
// Params: blue (0) = the crank (only its movement counts, wrapping), green (1) = amount, white (2) =
//         destination (any of the 18 flat targets). Orange is not used. Wind-down time constant 0.8 s,
//         top speed 12 Hz, full depth from 3 Hz.
//
// lfo_display: [0] phase (Q16), [1] speed (Hz, Q16), [2] LFO value Q15, [3] depth Q15, [4] applied amount Q15.
#include <new>

#include "dsp_internal.h"

namespace op1::dsp {
namespace {

class CrankLfo final : public Lfo {
 public:
  void process(const Param p[kParams], const LfoInputs&, ModOut& out) noexcept override {
    const s32 now = u15(p[0]);
    if (prev_ < 0) prev_ = now;
    s32 d = now - prev_;
    if (d > 16384) d -= 32768;
    else if (d < -16384) d += 32768;
    prev_ = now;
    // a full turn of the encoder range adds 40 Hz (d / 32768 * 40 in Q16 = d * 80)
    s64 sp = s64(speed_q16_) + s64(d) * 80;
    if (sp < 0) sp = -sp;
    if (sp > (s64(12) << 16)) sp = s64(12) << 16;
    constexpr s64 kFriction = s64(0.998188 * 65536);  // exp(-64 / (0.8 fs)) per block
    speed_q16_ = s32((sp * kFriction) >> 16);
    ph_ += u32((u64(speed_q16_) * ((u64(kBlock) << 32) / kSampleRate)) >> 16);
    value_ = sin_q31(ph_) >> 16;
    depth_ = s32(clampv<s64>(s64(speed_q16_) * 32767 / (s64(3) << 16), 0, 32767));
    amount_ = s32(s64(value_) * depth_ / 32767 * u15(p[1]) / 32767);
    add_mod(out, flat_target(p[2]), amount_);
  }
  void reset() noexcept override {
    speed_q16_ = 0;
    prev_ = -1;
    ph_ = 0;
  }
  void view(int32_t d[16]) const noexcept override {
    d[0] = int32_t(ph_ >> 16);
    d[1] = speed_q16_;
    d[2] = value_;
    d[3] = depth_;
    d[4] = amount_;
  }

 private:
  s32 prev_ = -1, speed_q16_ = 0, value_ = 0, depth_ = 0, amount_ = 0;
  u32 ph_ = 0;
};

}  // namespace

Lfo* make_lfo_crank(void* mem, size_t size) noexcept {
  return sizeof(CrankLfo) <= size ? new (mem) CrankLfo() : nullptr;
}

}  // namespace op1::dsp
