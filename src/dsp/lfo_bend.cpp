// bend (LFO db id 6, LfoBend.cpp): made for the Bender accessory. Turning the orange encoder bends the
// destination; it springs back to rest when the encoder stops. Designed by ear on the OG facts: defaults
// 7200/7100/32767/0/0/0/0/2048.
//
// Params: blue (0) = destination group, green (1) = parameter in the group, white (2) = amount,
//         orange (3) = the bender (only its movement counts: the distance turned since the previous block,
//         the short way around the wrap), shift + orange (7) = direction (below 16384 UP, else DOWN).
// The bend springs back with a 0.25 s time constant.
//
// lfo_display: [0] bend Q15 (0..32767), [1] direction (+1 / -1), [2] destination group, [3] destination
//              index, [4] applied amount Q15.
#include <new>

#include "dsp_internal.h"

namespace op1::dsp {
namespace {

class BendLfo final : public Lfo {
 public:
  void process(const Param p[kParams], const LfoInputs&, ModOut& out) noexcept override {
    const s32 now = u15(p[3]);
    if (prev_ < 0) prev_ = now;
    s32 d = now > prev_ ? now - prev_ : prev_ - now;
    if (d > 16384) d = 32768 - d;  // wrapped: the short way
    prev_ = now;
    bend_ = s32(clampv<s64>(s64(bend_) + s64(d) * 4, 0, 32767));
    // spring back: exp(-64 / (0.25 fs)) per block
    constexpr s32 kSpring = s32(0.994212 * 32768);
    bend_ = s32((s64(bend_) * kSpring) >> 15);
    dir_ = p[7] >= 16384 ? -1 : 1;
    target_ = target_of(p[0], p[1]);
    amount_ = s32(s64(bend_) * dir_ * u15(p[2]) / 32767);
    add_mod(out, target_, amount_);
  }
  void reset() noexcept override {
    bend_ = 0;
    prev_ = -1;
  }
  void view(int32_t d[16]) const noexcept override {
    d[0] = bend_;
    d[1] = dir_;
    d[2] = int32_t(target_.group);
    d[3] = target_.index;
    d[4] = amount_;
  }

 private:
  s32 prev_ = -1, bend_ = 0, amount_ = 0;
  int dir_ = 1;
  ModTarget target_;
};

}  // namespace

Lfo* make_lfo_bend(void* mem, size_t size) noexcept {
  return sizeof(BendLfo) <= size ? new (mem) BendLfo() : nullptr;
}

}  // namespace op1::dsp
