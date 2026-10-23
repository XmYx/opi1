// punch (fx id 2, Mllp.cpp; the screen is a boxer): a transient shaper. t5 (2026-10-23) rebuilt it on the
// firmware's structure (Mllp process 0x019E6260, decode-fx.md Q7; F = seen in the code, I = this port's reading):
//   - F: slot 3 (orange) = dry/wet mix: out = x (1 - s3) + wet s3, with no make-up stage after it (before, the port
//     applied a 1..4x make-up into a tanh, so the defaults pushed a -20 dBFS input up by ~10 dB into saturation).
//   - F: slot 0 (blue) = the filter frequency, index (s0 >> 7) + (s2 >> 12) into the shared cutoff table, smoothed
//     7/8 old + 1/8 new per block.
//   - F: slot 2 (white) = the detector window: n = s2 >> 10 (0..31) blocks of history; the detector is the sum of
//     the last n block peaks |x|.
//   - F: slot 1 (green) = the threshold: g = max(0, s1 - detector) drives the wet path, so punch boosts while the
//     recent level is below s1 (a new hit) and lets go as the sound sustains.
//   - F: the wet path ends in the shared saturation table (bounded).
//   - I: the wet path is the input plus its low band (a one-pole low-pass at the indexed frequency) boosted by up to
//     x4 with g, into soft_clip; the cutoff table is the one nitro uses (exponential, index 0 = 20 Hz, clamped at
//     113); the detector compares the window's mean peak with s1. Stereo uses one detector on the louder side.
//
// display[8..]: [8] filter frequency in Hz, [9] 32767 - the applied boost (Q15), [10] the applied boost Q15 (g
//               scaled by the block's level: 0 in silence), [11] input peak (16-bit units).
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "fx_base.h"

namespace op1::dsp {
namespace {

constexpr int kPunchIdx = 114;

struct PunchTable {
  s32 c[kPunchIdx];
  int32_t hz[kPunchIdx];
  PunchTable() {
    for (int i = 0; i < kPunchIdx; ++i) {
      const double f = 20.0 * std::pow(1000.0, double(i) / 113.0);
      c[i] = s32((1.0 - std::exp(-2.0 * 3.14159265358979 * f / kSampleRate)) * 2147483647.0);
      hz[i] = int32_t(f + 0.5);
    }
  }
};
const PunchTable kPunch;

template <int C>
class PunchFx final : public FxBase<C> {
  using B = FxBase<C>;

 public:
  PunchFx() noexcept { reset(); }
  void reset() noexcept override {
    for (auto& s : lp_) s = 0;
    for (auto& h : hist_) h = 0;
    pos_ = idx_ = 0;
    coef_ = kPunch.c[0];
    g_ = peak_ = blk_ = shown_ = 0;
  }

  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept override {
    const int ch = B::channels(r);
    const s32 s0 = u15(B::p_[0]), s1 = u15(B::p_[1]), s2 = u15(B::p_[2]), s3 = u15(B::p_[3]);
    const s64 boost_q15 = s64(g_) * 3;  // 0 .. 3 (gain 1 .. 4 on the low band)
    for (int k = 0; k < ch; ++k) {
      Sample* buf = k ? r : l;
      for (int i = 0; i < ctx.frames; ++i) {
        const s64 x = buf[i];
        const s32 a = s32(std::llabs(x >> 16));
        if (a > blk_) blk_ = a;
        lp_[k] += (s64(coef_) * (x - lp_[k])) >> 31;
        const s32 wet = soft_clip(x + ((lp_[k] * boost_q15) >> 15));
        buf[i] = sat((x * (32768 - s3) + s64(wet) * s3) >> 15);
      }
    }
    if ((ctx.frame + ctx.frames) % kBlock == 0) {  // end of a 64-frame block: the detector and the filter step
      hist_[pos_] = blk_;
      pos_ = (pos_ + 1) & 31;
      const int n = std::max(1, s2 >> 10);
      s64 sum = 0;
      for (int k = 1; k <= n; ++k) sum += hist_[(pos_ - k) & 31];
      g_ = s32(std::max<s64>(0, s1 - sum / n));
      idx_ = std::clamp((s0 >> 7) + (s2 >> 12), 0, kPunchIdx - 1);
      coef_ = s32((s64(coef_) * 7 + kPunch.c[idx_]) >> 3);
      peak_ = blk_;
      shown_ = s32((s64(g_) * std::min<s32>(32767, blk_ * 8)) >> 15);
      blk_ = 0;
    }
    B::disp_[0] = kPunch.hz[idx_];
    B::disp_[1] = 32767 - shown_;
    B::disp_[2] = shown_;
    B::disp_[3] = peak_;
  }

 private:
  s64 lp_[C] = {};
  s32 hist_[32] = {};
  int pos_ = 0, idx_ = 0;
  s32 coef_ = 0, g_ = 0, peak_ = 0, blk_ = 0, shown_ = 0;
};

}  // namespace

Effect* make_fx_punch(bool stereo, void* mem, size_t size) noexcept { return make_fx<PunchFx>(stereo, mem, size); }

}  // namespace op1::dsp
