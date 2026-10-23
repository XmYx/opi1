// midi (LFO db id 5, LfoMidi.cpp): incoming MIDI CC 1-4 drive up to four destinations (the firmware
// asserts i in 0..3, LfoMidi.cpp:241). Designed by ear on the OG facts: defaults 2000 x 8.
//
// Params: blue/green/white/orange (0..3) = destination of CC 1..4: OFF or one of the 18 flat targets
//         (19 equal bins, the first is OFF). The CC value 0..127 adds 0..+full scale to its destination.
//         Slots 4..7 are not used.
//
// lfo_display: [0..3] destination of CC 1..4 (-1 = OFF, else flat target 0..17), [4..7] CC values 0..127.
#include <new>

#include "dsp_internal.h"

namespace op1::dsp {
namespace {

class MidiLfo final : public Lfo {
 public:
  void process(const Param p[kParams], const LfoInputs& in, ModOut& out) noexcept override {
    for (int k = 0; k < 4; ++k) {
      const int i = choose(p[k], kFlatTargets + 1);
      dest_[k] = i - 1;
      cc_[k] = in.controls ? clampv<s32>(in.controls->cc[k + 1], 0, 127) : 0;
      if (i == 0) continue;
      // the middle of flat bin i-1, so flat_target() picks exactly that target
      const s32 v = s32((s64(i - 1) * 32768 + 16384) / kFlatTargets);
      add_mod(out, flat_target(v), cc_[k] * 32767 / 127);
    }
  }
  void reset() noexcept override {}
  void view(int32_t d[16]) const noexcept override {
    for (int k = 0; k < 4; ++k) {
      d[k] = dest_[k];
      d[4 + k] = cc_[k];
    }
  }

 private:
  int dest_[4] = {-1, -1, -1, -1};
  s32 cc_[4] = {};
};

}  // namespace

Lfo* make_lfo_midi(void* mem, size_t size) noexcept {
  return sizeof(MidiLfo) <= size ? new (mem) MidiLfo() : nullptr;
}

}  // namespace op1::dsp
