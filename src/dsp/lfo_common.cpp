// Helpers shared by the seven LFOs: destinations, amounts and rates (see dsp_internal.h).
#include "dsp_internal.h"

namespace op1::dsp {

ModTarget target_of(s32 group_v, s32 param_v) noexcept {
  static constexpr int kSize[4] = {kParams, 4, 4, 2};
  ModTarget t;
  const int g = choose(group_v, 4);
  t.group = ModGroup(g);
  t.index = uint8_t(choose(param_v, kSize[g]));
  return t;
}

ModTarget flat_target(s32 v) noexcept {
  const int i = choose(v, kFlatTargets);
  ModTarget t;
  if (i < 8) { t.group = ModGroup::Engine; t.index = uint8_t(i); }
  else if (i < 12) { t.group = ModGroup::Envelope; t.index = uint8_t(i - 8); }
  else if (i < 16) { t.group = ModGroup::Fx; t.index = uint8_t(i - 12); }
  else { t.group = ModGroup::Main; t.index = uint8_t(i - 16); }
  return t;
}

void add_mod(ModOut& m, const ModTarget& t, s32 amount15) noexcept {
  if (t.off) return;
  amount15 = clampv<s32>(amount15, -65534, 65534);
  switch (t.group) {
    case ModGroup::Engine: m.knob[t.index & 7] += amount15; break;
    case ModGroup::Envelope: m.adsr[t.index & 3] += amount15; break;
    case ModGroup::Fx: m.fx[t.index & 3] += amount15; break;
    case ModGroup::Main:
      if (t.index == 0) m.pitch_q8 += s32(s64(amount15) * (12 * 256) / 32767);  // +-12 semitones
      else m.volume15 += amount15;                                             // +-100 %
      break;
  }
}

u32 lfo_rate_inc(s32 v15) noexcept {
  static constexpr ExpCurve kRate(0.05, 20.0);
  constexpr u64 k = u64(double(kBlock) * 281474976710656.0 / kSampleRate);  // blocks: 64 * 2^48 / fs
  return u32((kRate.at(v15) * k) >> 32);
}

u32 lfo_sync_inc(s32 v15, int bpm_tenths) noexcept {
  static constexpr int kEighthBeats[8] = {128, 64, 32, 16, 8, 4, 2, 1};  // 4 bars .. 1/32, in 1/8 beats
  const int b8 = kEighthBeats[choose(v15, 8)];
  // Hz = bpm / 60 / beats; increment per block = Hz * 64 / fs * 2^32
  const u64 num = u64(clampv(bpm_tenths, 1, 9999)) * (u64(kBlock) << 32) * 8;
  return u32(num / (u64(600) * kSampleRate * b8));
}

}  // namespace op1::dsp
