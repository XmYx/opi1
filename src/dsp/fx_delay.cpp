// delay (fx id 0, Dssd.cpp): a tape delay. t5 (2026-10-23) from the firmware (Dssd process, decode-fx.md Q8;
// F = seen in the code, I = this port's reading):
//   blue (0) = range, F: a buffer of 44100 >> (s0 >> 11) samples, 6 bands for s0 0..12287 (1 s, 0.5 s, 250 ms,
//     125 ms, 62 ms, 31 ms); I: s0 >= 12288 stays in the shortest band.
//   green (1) = tape speed, F: max(s1, 33), unsigned (a negative value reads as 33; before t5 the port read it
//     bipolar). I: the echo time = range x 8192 / speed (the default 8000 plays about the range), and changing it
//     glides the read head, so the echoes repitch like tape.
//   white (2) = feedback, F: s2 x 2, unity at 16384 (above, the repeats grow: soft-clipped here).
//   orange (3) = input level into the delay, F. The output is dry + wet (F: no mix control).
//   I: a 4.5 kHz damping in the loop; stereo (master) runs the right line 1.5x longer with a little cross-feed.
//
// display[8..]: [8] delay time in ms, [9] feedback Q15, [10] input level Q15, [11] range band 0..5.
#include "fx_base.h"

namespace op1::dsp {
namespace {

template <int C>
class DelayFx final : public FxBase<C> {
  using B = FxBase<C>;

 public:
  DelayFx() noexcept { reset(); }
  void reset() noexcept override {
    for (auto& d : line_) d.clear();
    for (auto& f : lp_) f = OnePole{};
    update();
    t_q16_ = target_q16_;
  }

  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept override {
    if (B::dirty_) update();
    const int ch = B::channels(r);
    constexpr s32 kGlide = s32(0.0004 * 2147483648.0);  // tape-head glide per sample
    constexpr s64 kMax = s64(kLen - 4) << 16;
    for (int i = 0; i < ctx.frames; ++i) {
      t_q16_ += ((target_q16_ - t_q16_) * kGlide) >> 31;
      if (ch == 1) {
        const s32 y = line_[0].readf(t_q16_);
        line_[0].write(soft_clip(s64(mul15(l[i], lvl15_)) + mul15(lp_[0].tick(damp_, y), fb15_)));
        l[i] = add(l[i], y);
      } else {
        const s64 tr = t_q16_ * 3 / 2 > kMax ? t_q16_ : t_q16_ * 3 / 2;
        const s32 yl = line_[0].readf(t_q16_), yr = line_[1].readf(tr);
        const s32 fl = lp_[0].tick(damp_, sat((s64(yl) * 4 + yr) / 5));  // 0.8 / 0.2 cross-feed
        const s32 fr = lp_[1].tick(damp_, sat((s64(yr) * 4 + yl) / 5));
        line_[0].write(soft_clip(s64(mul15(l[i], lvl15_)) + mul15(fl, fb15_)));
        line_[1].write(soft_clip(s64(mul15(r[i], lvl15_)) + mul15(fr, fb15_)));
        l[i] = add(l[i], yl);
        r[i] = add(r[i], yr);
      }
    }
    B::disp_[0] = int32_t((t_q16_ >> 16) * 1000 / kSampleRate);
  }

 private:
  static constexpr int kLen = 131072;  // 2.97 s of 16-bit line per channel

  void update() noexcept {
    band_ = clampv<s32>(u15(B::p_[0]) >> 11, 0, 5);
    const s64 range = s64(kSampleRate) >> band_;                 // samples
    const s64 speed = B::p_[1] < 33 ? 33 : B::p_[1];
    target_q16_ = clampv<s64>(((range * 8192) << 16) / speed, s64(1) << 16, s64(kLen - 4) << 16);
    fb15_ = clampv<s32>(s32(u15(B::p_[2])) * 2, 0, 65535);    // unity at 16384
    lvl15_ = u15(B::p_[3]);
    damp_ = onepole_coef(hz_inc(u64(4500) << 16));
    B::disp_[1] = fb15_;
    B::disp_[2] = lvl15_;
    B::disp_[3] = band_;
    B::dirty_ = false;
  }

  DelayLine<kLen, int16_t> line_[C];
  OnePole lp_[C];
  s64 t_q16_ = 0, target_q16_ = 0;
  s32 fb15_ = 0, lvl15_ = 0, damp_ = 0, band_ = 0;
};

}  // namespace

Effect* make_fx_delay(bool stereo, void* mem, size_t size) noexcept { return make_fx<DelayFx>(stereo, mem, size); }

}  // namespace op1::dsp
