// grid (fx id 1, Grid.cpp): a 4 x 4 feedback delay network tuned like a resonant box. Designed by ear (the
// firmware routine is not decoded) on the OG facts: defaults 8000/8000/18000/18000.
//
// Params (t5, 2026-10-23, the firmware's reads in Grid 0x019AF338, decode-fx.md; F = seen, I = this port's
//         reading): blue (0) = x size and green (1) = y size, F (s >> 7) << 1 = 0..508, I: read as milliseconds
//         (before t5 the port used 1 .. 60 ms on a squared curve); I: lines 1/2 follow x, lines 3/4 follow y, at ratios 1, 1.37,
//         1, 1.61, at least 1 ms; white (2) = z feedback, F s2^2 (I: up to 0.98); orange (3) = mix (I:
//         dry .. wet).
// The network mixes its four lines with a Hadamard matrix (orthogonal, energy preserving) and damps each
// return with a 7 kHz one-pole. Mono: the input feeds all four lines, the wet signal is their sum.
// Stereo (master): left feeds lines 1/3, right lines 2/4; the left output reads lines 1+3, the right 2+4.
//
// display[8..]: [8] x size in 0.1 ms, [9] y size in 0.1 ms, [10] feedback Q15, [11] mix Q15.
#include <algorithm>

#include "fx_base.h"

namespace op1::dsp {
namespace {

template <int C>
class GridFx final : public FxBase<C> {
  using B = FxBase<C>;

 public:
  GridFx() noexcept { reset(); }
  void reset() noexcept override {
    for (auto& d : line_) d.clear();
    for (auto& f : damp_) f = OnePole{};
    update();
  }

  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept override {
    if (B::dirty_) update();
    const bool st = B::channels(r) == 2;
    for (int i = 0; i < ctx.frames; ++i) {
      s64 y[4];
      for (int k = 0; k < 4; ++k) y[k] = line_[k].readf(len_q16_[k]);
      // Hadamard mix, scaled by 1/2 (orthonormal for 4 x 4)
      const s64 a = y[0] + y[1], b = y[0] - y[1], c = y[2] + y[3], d = y[2] - y[3];
      const s32 m[4] = {sat((a + c) / 2), sat((b + d) / 2), sat((a - c) / 2), sat((b - d) / 2)};
      const s64 xl = l[i], xr = st ? s64(r[i]) : xl;
      const s64 in[4] = {xl, xr, xl / 2, xr / 2};
      for (int k = 0; k < 4; ++k) line_[k].write(soft_clip(in[k] + mul15(damp_[k].tick(dampk_, m[k]), fb15_)));
      if (st) {
        const s32 wl = sat((y[0] + y[2]) * 3 / 5), wr = sat((y[1] + y[3]) * 3 / 5);
        l[i] = lerp15(l[i], wl, mix15_);
        r[i] = lerp15(r[i], wr, mix15_);
      } else {
        const s32 w = sat((y[0] + y[1] + y[2] + y[3]) * 3 / 10);
        l[i] = lerp15(l[i], w, mix15_);
      }
    }
  }

 private:
  static constexpr int kLen = 32768;  // 743 ms of 16-bit line per line

  void update() noexcept {
    static constexpr s64 kRatio[4] = {65536, 89784, 65536, 105513};  // 1, 1.37, 1, 1.61 (Q16)
    // (s >> 7) << 1, read as milliseconds (I: the unit is not decoded; 0..508 ms, the default 8000 = 124 ms)
    const s64 xs = ms_q16(double(std::max<s32>(1, (u15(B::p_[0]) >> 7) << 1)));  // samples Q16
    const s64 ys = ms_q16(double(std::max<s32>(1, (u15(B::p_[1]) >> 7) << 1)));
    for (int k = 0; k < 4; ++k) len_q16_[k] = std::min<s64>(((k < 2 ? xs : ys) * kRatio[k]) >> 16, s64(kLen - 4) << 16);
    const s64 p = u15(B::p_[2]);
    fb15_ = s32((((p * p) >> 15) * 32113) >> 15);  // 0.98 s2^2
    mix15_ = u15(B::p_[3]);
    dampk_ = onepole_coef(hz_inc(u64(7000) << 16));
    B::disp_[0] = int32_t((xs >> 16) * 10000 / kSampleRate);
    B::disp_[1] = int32_t((ys >> 16) * 10000 / kSampleRate);
    B::disp_[2] = fb15_;
    B::disp_[3] = mix15_;
    B::dirty_ = false;
  }

  DelayLine<kLen, int16_t> line_[4];
  OnePole damp_[4];
  s64 len_q16_[4] = {};
  s32 fb15_ = 0, mix15_ = 0, dampk_ = 0;
};

}  // namespace

Effect* make_fx_grid(bool stereo, void* mem, size_t size) noexcept { return make_fx<GridFx>(stereo, mem, size); }

}  // namespace op1::dsp
