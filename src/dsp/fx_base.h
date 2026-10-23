// Common part of the eight effects (t4). The firmware has a separate mono (instrument) and stereo (master)
// class per effect (firmware-behaviour.md §5); here one template per effect is instantiated for C = 1 and
// C = 2 channels. A stereo instance called with r == nullptr processes l only.
//
// Effect::view(display[16]) for every effect: [0..7] the eight parameters as set (after LFO offsets),
// [8..15] effect-specific values documented in each fx_<name>.cpp.
#pragma once

#include <new>

#include "dsp_internal.h"

namespace op1::dsp {

template <int C>
class FxBase : public Effect {
 public:
  static_assert(C == 1 || C == 2);
  void set_param(int index, Param value) noexcept override {
    const int i = index & 7;
    if (p_[i] != value) {
      p_[i] = value;
      dirty_ = true;
    }
  }
  void view(int32_t display[16]) const noexcept override {
    for (int i = 0; i < kParams; ++i) display[i] = p_[i];
    for (int i = 0; i < 8; ++i) display[8 + i] = disp_[i];
  }

 protected:
  static int channels(const Sample* r) noexcept { return (C == 2 && r) ? 2 : 1; }
  Param p_[kParams] = {};
  bool dirty_ = true;
  int32_t disp_[8] = {};
};

// Construct FX<1> (mono) or FX<2> (stereo) into `mem`; nullptr if it does not fit.
template <template <int> class FX>
Effect* make_fx(bool stereo, void* mem, size_t size) noexcept {
  if (stereo) {
    if (sizeof(FX<2>) > size) return nullptr;
    return new (mem) FX<2>();
  }
  static_assert(sizeof(FX<1>) <= kMonoFxStorage, "mono effect must fit the instrument's effect slot");
  if (sizeof(FX<1>) > size) return nullptr;
  return new (mem) FX<1>();
}

}  // namespace op1::dsp
