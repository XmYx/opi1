// tremolo (LFO db id 1, LfoDual.cpp): one sine LFO on pitch and volume at once (the "dual" target).
// Designed by ear on the OG facts: defaults 16000/0/0/16000; slot 1 is signed (depth with sign).
//
// Params: blue (0) = speed (0.05 .. 20 Hz), green (1) = pitch depth (signed, +-2 semitones),
//         white (2) = volume depth (signed: negative inverts the shape), orange (3) = envelope (fade-in
//         after each note, 0 .. 3 s).
//
// lfo_display: [0] phase (Q16 of a cycle), [1] LFO value Q15, [2] pitch offset (semitones * 256),
//              [3] volume gain Q15, [4] fade Q15.
#include <new>

#include "dsp_internal.h"

namespace op1::dsp {
namespace {

class Tremolo final : public Lfo {
 public:
  void process(const Param p[kParams], const LfoInputs& in, ModOut& out) noexcept override {
    if (in.note_ons > 0) since_ = 0;
    ph_ += lfo_rate_inc(p[0]);
    if (since_ < (1 << 30)) since_ += kBlock;
    // fade-in: p3^2 * 3 s
    const s64 fade_len = (s64(sq15(u15(p[3]))) * 3 * kSampleRate) >> 15;
    fade15_ = fade_len < kBlock ? 32767 : s32(clampv<s64>(s64(since_) * 32767 / fade_len, 0, 32767));
    const s32 s = sin_q31(ph_);
    const s32 sv = mul15(s, fade15_);                                     // Q31
    pitch_ = s32((s64(sv >> 16) * s15(p[1]) / 32767) * 512 / 32767);       // +-2 semitones
    const s32 vol = s15(p[2]);
    const s32 shaped = vol < 0 ? -sv : sv;                                 // Q31
    // dip: (shaped * 0.5 - 0.5) * |vol| -> 0 .. -|vol|
    const s64 dip = ((s64(shaped >> 16) - 32767) / 2) * (vol < 0 ? -vol : vol) / 32767;
    out.pitch_q8 += pitch_;
    out.volume15 += s32(dip);
    gain_ = out.volume15;
    value_ = sv >> 16;
  }
  void reset() noexcept override {
    ph_ = 0;
    since_ = 1 << 30;
  }
  void view(int32_t d[16]) const noexcept override {
    d[0] = int32_t(ph_ >> 16);
    d[1] = value_;
    d[2] = pitch_;
    d[3] = gain_;
    d[4] = fade15_;
  }

 private:
  u32 ph_ = 0;
  int since_ = 1 << 30;
  s32 fade15_ = 32767, pitch_ = 0, gain_ = 32768, value_ = 0;
};

}  // namespace

Lfo* make_lfo_tremolo(void* mem, size_t size) noexcept {
  return sizeof(Tremolo) <= size ? new (mem) Tremolo() : nullptr;
}

}  // namespace op1::dsp
