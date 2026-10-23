// nitro (fx id 6, Ftwo.cpp): a resonant high-pass ladder into a resonant low-pass ladder whose cutoffs move with
// an envelope follower. t5 (2026-10-23) rebuilt it on the firmware's structure (Ftwo process 0x019A62A4, decoded
// in the t5 notes, decode-fx.md Q4; F = seen in the code, I = this port's reading where the code is not decoded):
//   - F: stereo = two independent mono instances (ctor 0x019A6200 builds two, 0x019A684C runs one per side):
//     each side has its own follower and filter states.
//   - F: follower per 64-frame block: env = env * 0x7F00 / 0x8000, then env = max(env, x) over the block's
//     samples (a signed max, as the firmware does), in 16-bit sample units.
//   - F: slot 1 (follow, green) is signed and cubed: m = s1^3 * env.
//   - F: slot 0 (blue) -> idxA = clamp((s0 + m) >> 7, 0, 113) and slot 3 (orange) -> idxB = clamp((s3 + m) >> 7,
//     0, 113) index one cutoff table, so both cutoffs move together with the follower, and slot values above
//     14464 (113 x 128) are the top of the range: the factory default s3 = 16000 and the drum kits' 16448 are
//     fully open (before, the port read s3 on an exponential 60 Hz .. 16 kHz curve, a ~1 kHz low-pass, so
//     those kits went nearly silent on some keys: PARITY-AUDIT.md §4.1 note for t5).
//   - F: coefficients are smoothed per block, new = 7/8 old + 1/8 table value.
//   - F: slot 2 (white) -> feedback k1 = s2^2 + s2 * env^2 (high-pass ladder) and k2 = s2^2 (low-pass ladder),
//     both through a saturating table in the loop.
//   - F: ladder 1 = 4 one-pole stages with coef A, output a weighted sum of the stage outputs (I: the high-pass,
//     here the binomial sum x - 4y1 + 6y2 - 4y3 + y4); ladder 2 = 4 one-pole stages with coef B, output = the last
//     stage (the low-pass).
//   - I: the cutoff table (0xFF801F50, filled at boot, values not decoded) is exponential here: index 0 = 20 Hz,
//     113 = 20 kHz; feedback gain 4 k (4 = the ladder's self-oscillation point), saturated by soft_clip; the
//     low-pass output is scaled by (1 + feedback / 2), half the ladder's pass-band loss, so a self-oscillating
//     filter stays below full scale.
//
// display[8..]: [8] low-pass cutoff in Hz, [9] high-pass cutoff in Hz, [10] follower (left side) Q15, [11] follow
//               offset m in slot units, [12] idxA, [13] idxB.
#include <algorithm>
#include <cmath>

#include "fx_base.h"

namespace op1::dsp {
namespace {

constexpr int kNitroIdx = 114;  // 0..113

struct NitroTable {
  s32 c[kNitroIdx];   // one-pole coefficient 1 - exp(-2 pi f / fs), Q31
  int32_t hz[kNitroIdx];
  NitroTable() {
    for (int i = 0; i < kNitroIdx; ++i) {
      const double f = 20.0 * std::pow(1000.0, double(i) / 113.0);
      const double k = 1.0 - std::exp(-2.0 * 3.14159265358979 * f / kSampleRate);
      c[i] = s32(k * 2147483647.0);
      hz[i] = int32_t(f + 0.5);
    }
  }
};
const NitroTable kNitro;  // built at start-up, before any audio

template <int C>
class NitroFx final : public FxBase<C> {
  using B = FxBase<C>;

 public:
  NitroFx() noexcept { reset(); }
  void reset() noexcept override {
    for (auto& s : side_) {
      s = Side{};
      s.ca = kNitro.c[0];
      s.cb = kNitro.c[kNitroIdx - 1];
    }
  }

  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept override {
    const int ch = B::channels(r);
    const s32 s0 = B::p_[0], s1 = B::p_[1], s2 = B::p_[2], s3 = B::p_[3];
    const s64 s1c = (((s64(s1) * s1) >> 15) * s1) >> 15;  // s1^3, slot units (signed)
    const s64 k2 = (s64(s2) * s2) >> 15;                   // Q15
    for (int k = 0; k < ch; ++k) {
      Side& sd = side_[k];
      Sample* buf = k ? r : l;
      // follower (16-bit sample units)
      sd.env = s32((s64(sd.env) * 0x7F00) >> 15);
      for (int i = 0; i < ctx.frames; ++i) sd.env = std::max(sd.env, s32(buf[i] >> 16));
      const s64 m = (s1c * sd.env) >> 15;
      const int ia = int(std::clamp<s64>((s0 + m) >> 7, 0, kNitroIdx - 1));
      const int ib = int(std::clamp<s64>((s3 + m) >> 7, 0, kNitroIdx - 1));
      sd.ca = s32((s64(sd.ca) * 7 + kNitro.c[ia]) >> 3);
      sd.cb = s32((s64(sd.cb) * 7 + kNitro.c[ib]) >> 3);
      const s64 env2 = (s64(sd.env) * sd.env) >> 15;
      const s64 k1 = k2 + ((s64(s2) * env2) >> 15);  // may go negative with a negative s2 (as the firmware)
      const s64 fb1 = std::clamp<s64>(k1 * 4, -4 * 32767, 4 * 32767) * 31 / 32;  // Q15, < 4
      const s64 fb2 = std::clamp<s64>(k2 * 4, 0, 4 * 32767) * 31 / 32;
      for (int i = 0; i < ctx.frames; ++i) {
        const s64 x = buf[i] >> 1;  // one bit of headroom inside the ladders
        // high-pass ladder (coef A) with saturated feedback from its last stage
        const s64 u = x - ((s64(soft_clip(sd.y[3])) * fb1) >> 15);
        sd.y[0] += (s64(sd.ca >> 1) * std::clamp<s64>(u - sd.y[0], -(s64(1) << 32), s64(1) << 32)) >> 30;
        sd.y[1] += (s64(sd.ca >> 1) * std::clamp<s64>(sd.y[0] - sd.y[1], -(s64(1) << 32), s64(1) << 32)) >> 30;
        sd.y[2] += (s64(sd.ca >> 1) * std::clamp<s64>(sd.y[1] - sd.y[2], -(s64(1) << 32), s64(1) << 32)) >> 30;
        sd.y[3] += (s64(sd.ca >> 1) * std::clamp<s64>(sd.y[2] - sd.y[3], -(s64(1) << 32), s64(1) << 32)) >> 30;
        const s64 hp = u - 4 * sd.y[0] + 6 * sd.y[1] - 4 * sd.y[2] + sd.y[3];
        // low-pass ladder (coef B)
        const s64 v = hp - ((s64(soft_clip(sd.z[3])) * fb2) >> 15);
        sd.z[0] += (s64(sd.cb >> 1) * std::clamp<s64>(v - sd.z[0], -(s64(1) << 32), s64(1) << 32)) >> 30;
        sd.z[1] += (s64(sd.cb >> 1) * std::clamp<s64>(sd.z[0] - sd.z[1], -(s64(1) << 32), s64(1) << 32)) >> 30;
        sd.z[2] += (s64(sd.cb >> 1) * std::clamp<s64>(sd.z[1] - sd.z[2], -(s64(1) << 32), s64(1) << 32)) >> 30;
        sd.z[3] += (s64(sd.cb >> 1) * std::clamp<s64>(sd.z[2] - sd.z[3], -(s64(1) << 32), s64(1) << 32)) >> 30;
        for (s64& s : sd.y) s = std::clamp<s64>(s, -(s64(1) << 33), s64(1) << 33);
        for (s64& s : sd.z) s = std::clamp<s64>(s, -(s64(1) << 33), s64(1) << 33);
        buf[i] = soft_clip(((sd.z[3] * (32768 + fb2 / 2)) >> 15) * 2);
      }
      if (k == 0) {
        B::disp_[0] = kNitro.hz[ib];
        B::disp_[1] = kNitro.hz[ia];
        B::disp_[2] = std::clamp<int32_t>(sd.env, 0, 32767);
        B::disp_[3] = int32_t(m);
        B::disp_[4] = ia;
        B::disp_[5] = ib;
      }
    }
  }

 private:
  struct Side {
    s32 env = 0;
    s32 ca = 0, cb = 0;
    s64 y[4] = {}, z[4] = {};
  };
  Side side_[C];
};

}  // namespace

Effect* make_fx_nitro(bool stereo, void* mem, size_t size) noexcept { return make_fx<NitroFx>(stereo, mem, size); }

}  // namespace op1::dsp
