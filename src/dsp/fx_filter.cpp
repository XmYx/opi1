// filter (fx id 5, Mmmf.cpp; screen mmmf): the OG's hidden effect (no fx_types row on R.246, so the menu never
// offers it; firmware-behaviour.md §11). t5 (2026-10-23) rebuilt it on the firmware's structure (Mmmf process
// 0x019E7934, decode-fx.md Q3; F = seen in the code, I = this port's reading):
//   - F: 12 types, names lp iv, lp iii, lp ii, lp i, ap, ap iii, notch ii, bp ii, hp i, hp ii, hp iii, hp iv
//     (0x018E4958, 10-byte entries; before t5 the port had 5).
//   - F: white (2) morphs the type continuously: record i = s2 >> 11 and i + 1, fraction s2 & 0x7FF, between 12
//     five-tap weight records (0x01DD8A10, built at boot); s2 0..22527 is the useful range; the screen names the
//     nearest record, round(11 x s2 / 22528). I: the taps are the input and the four stages of a one-pole ladder,
//     and each record is the textbook mix for its name (lp n = stage n, hp n = (1 - L)^n, ap = 1 - 2L, ap iii =
//     (1 - 2L)^3, notch ii = 1 - 2L + 2L^2, bp ii = 2L - 2L^2).
//   - F: blue (0) = cutoff, index s0 >> 7 into the cutoff table, smoothed 7/8 + 1/8 per block. I: 20 Hz .. 18 kHz
//     exponential over the 256 indices.
//   - F: green (1) enters as s1^2 and s1^3 (I: resonance, ladder feedback up to just below self-oscillation);
//     orange (3) as s3^2 (I: drive, input gain 1 .. 8 into soft_clip). All four slots are unsigned.
// No db default; the port's 23000/10000/0/5000 stays (lp iv, open, mild resonance).
//
// display[8..]: [8] cutoff in Hz, [9] feedback Q15, [10] type name index 0..11, [11] drive x 100.
#include <algorithm>
#include <cmath>

#include "fx_base.h"

namespace op1::dsp {
namespace {

struct FilterTable {
  s32 c[256];
  int32_t hz[256];
  FilterTable() {
    for (int i = 0; i < 256; ++i) {
      const double f = 20.0 * std::pow(900.0, double(i) / 255.0);
      c[i] = s32((1.0 - std::exp(-2.0 * 3.14159265358979 * f / kSampleRate)) * 2147483647.0);
      hz[i] = int32_t(f + 0.5);
    }
  }
};
const FilterTable kFilt;

// tap weights (x, y1, y2, y3, y4) per type, Q12
constexpr int32_t kRec[12][5] = {
    {0, 0, 0, 0, 4096},            // lp iv
    {0, 0, 0, 4096, 0},            // lp iii
    {0, 0, 4096, 0, 0},            // lp ii
    {0, 4096, 0, 0, 0},            // lp i
    {4096, -8192, 0, 0, 0},        // ap
    {4096, -24576, 49152, -32768, 0},  // ap iii
    {4096, -8192, 8192, 0, 0},     // notch ii
    {0, 8192, -8192, 0, 0},        // bp ii
    {4096, -4096, 0, 0, 0},        // hp i
    {4096, -8192, 4096, 0, 0},     // hp ii
    {4096, -12288, 12288, -4096, 0},   // hp iii
    {4096, -16384, 24576, -16384, 4096},  // hp iv
};

template <int C>
class FilterFx final : public FxBase<C> {
  using B = FxBase<C>;

 public:
  FilterFx() noexcept { reset(); }
  void reset() noexcept override {
    for (auto& s : st_)
      for (auto& y : s) y = 0;
    coef_ = kFilt.c[u15(B::p_[0]) >> 7];
  }

  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept override {
    const int ch = B::channels(r);
    const s32 s0 = u15(B::p_[0]), s1 = u15(B::p_[1]), s2 = std::min<s32>(u15(B::p_[2]), 22527), s3 = u15(B::p_[3]);
    const int idx = s0 >> 7;
    coef_ = s32((s64(coef_) * 7 + kFilt.c[idx]) >> 3);
    // the type: crossfade of records i and i + 1
    const int ri = s2 >> 11;
    const s32 rf = s2 & 0x7FF;  // Q11
    s64 w[5];
    for (int k = 0; k < 5; ++k) {
      const s64 a = kRec[ri][k], b = kRec[ri < 11 ? ri + 1 : 11][k];
      w[k] = a + (((b - a) * rf) >> 11);  // Q12
    }
    // resonance from s1^2 and s1^3, just below the 4-pole ladder's self-oscillation (4)
    const s64 sq = (s64(s1) * s1) >> 15, cu = (sq * s1) >> 15;
    const s64 fb = std::min<s64>(((sq + cu) / 2) * 4 * 15 / 16, 4 * 32767 * 15 / 16);  // Q15
    const s64 drive_q12 = 4096 + ((((s64(s3) * s3) >> 15) * 7 * 4096) >> 15);         // 1 .. 8
    for (int k = 0; k < ch; ++k) {
      Sample* buf = k ? r : l;
      s64* y = st_[k];
      for (int i = 0; i < ctx.frames; ++i) {
        const s64 x = soft_clip((s64(buf[i]) * drive_q12) >> 12) >> 1;  // one bit of headroom in the ladder
        const s64 u = x - ((s64(soft_clip(y[3])) * fb) >> 15);
        y[0] += (s64(coef_ >> 1) * std::clamp<s64>(u - y[0], -(s64(1) << 32), s64(1) << 32)) >> 30;
        y[1] += (s64(coef_ >> 1) * std::clamp<s64>(y[0] - y[1], -(s64(1) << 32), s64(1) << 32)) >> 30;
        y[2] += (s64(coef_ >> 1) * std::clamp<s64>(y[1] - y[2], -(s64(1) << 32), s64(1) << 32)) >> 30;
        y[3] += (s64(coef_ >> 1) * std::clamp<s64>(y[2] - y[3], -(s64(1) << 32), s64(1) << 32)) >> 30;
        for (int s = 0; s < 4; ++s) y[s] = std::clamp<s64>(y[s], -(s64(1) << 33), s64(1) << 33);
        const s64 out = (w[0] * u + w[1] * y[0] + w[2] * y[1] + w[3] * y[2] + w[4] * y[3]) >> 12;
        buf[i] = soft_clip(((out * (32768 + fb / 2)) >> 15) * 2);
      }
    }
    B::disp_[0] = kFilt.hz[idx];
    B::disp_[1] = int32_t(fb);
    B::disp_[2] = std::clamp(int((s64(s2) * 11 + 11264) / 22528), 0, 11);
    B::disp_[3] = int32_t(drive_q12 * 100 / 4096);
  }

 private:
  s64 st_[C][4] = {};
  s32 coef_ = 0;
};

}  // namespace

Effect* make_fx_filter(bool stereo, void* mem, size_t size) noexcept { return make_fx<FilterFx>(stereo, mem, size); }

}  // namespace op1::dsp
