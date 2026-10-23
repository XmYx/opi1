// spring (fx id 4, Rymd.cpp): spring-tank reverb. Designed by ear (the firmware routine is not decoded) on
// the OG facts: defaults 8000/15000/2000/20000.
//
// Each spring is a feedback delay with a chain of 10 first-order allpasses inside (the dispersion that gives
// the "boing" chirp), a one-pole tone filter and a soft clip in the loop. The input is high-passed at 120 Hz.
// Params (t5, 2026-10-23, the firmware's reads in Rymd 0x01A326E8, decode-fx.md; F = seen, I = this port's
// reading): blue (0) = tone, F a = s0 >> 7 (an index into the cutoff table), green (1) = turns, F b = s1 >> 7,
// and F: when a + b > 220, a = 220 - b (a long spring cannot be bright); white (2) = damping, F 1 - s2^3 (more =
// a shorter ring: before t5 the port ran it the other way round); orange (3) = mix, F s3^2. I: tone = the loop
// low-pass 900 Hz .. 9 kHz over a, turns = spring length 25 .. 95 ms and dispersion 0.55 .. 0.90 over b, loop
// feedback 0.95 x (1 - s2^3) (at least 0.3).
// Mono: one spring. Stereo (master): the two springs (the second 1.27x longer) are fed the mono sum and
// return left and right.
//
// display[8..]: [8] spring length in ms, [9] feedback Q15, [10] tone in Hz, [11] mix Q15.
#include "fx_base.h"

namespace op1::dsp {
namespace {

constexpr int kStages = 10;

// First-order allpass y = c x + x1 - c y1 (Q31 coefficient), saturating.
struct Allpass1 {
  s32 x1 = 0, y1 = 0;
  s32 tick(s32 c, s32 x) noexcept {
    const s32 y = sat(((s64(c) * x) >> 31) + x1 - ((s64(c) * y1) >> 31));
    x1 = x;
    y1 = y;
    return y;
  }
};

struct SpringLine {
  Allpass1 ap[kStages];
  DelayLine<8192> dl;  // 186 ms, more than 95 ms x 1.27
  OnePole lp;
  void clear() noexcept {
    for (auto& a : ap) a = Allpass1{};
    dl.clear();
    lp = OnePole{};
  }
  s32 tick(s32 x, s64 len_q16, s32 fb15, s32 c, s32 lpk) noexcept {
    s32 y = sat(s64(x) + mul15(dl.readf(len_q16), fb15));
    for (auto& a : ap) y = a.tick(c, y);
    const s32 out = soft_clip(lp.tick(lpk, y));
    dl.write(out);
    return out;
  }
};

template <int C>
class SpringFx final : public FxBase<C> {
  using B = FxBase<C>;

 public:
  SpringFx() noexcept { reset(); }
  void reset() noexcept override {
    for (auto& s : spring_) s.clear();
    hp_.reset();
    update();
  }

  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept override {
    if (B::dirty_) update();
    const bool st = B::channels(r) == 2;
    const s32 dry15 = 32767 - mix15_ / 2;              // 1 - mix/2
    const s32 wet15 = s32((s64(mix15_) * 42598) >> 15);  // mix * 1.3
    for (int i = 0; i < ctx.frames; ++i) {
      const s64 sum = st ? (s64(l[i]) + r[i]) / 2 : s64(l[i]);
      hp_.tick(hpk_, sat(sum));
      const s32 x = mul15(hp_.hp, 19661);  // * 0.6
      const s32 ya = spring_[0].tick(x, len_q16_, fb15_, apc_, lpk_);
      l[i] = sat(s64(mul15(l[i], dry15)) + mul15(ya, wet15));
      if (st) {
        const s32 yb = spring_[1].tick(x, len_q16_ * 127 / 100, fb15_, apc_, lpk_);
        r[i] = sat(s64(mul15(r[i], dry15)) + mul15(yb, wet15));
      }
    }
  }

 private:
  void update() noexcept {
    static constexpr ExpCurve kTone(900.0, 9000.0);
    const s32 b = u15(B::p_[1]) >> 7;
    s32 a = u15(B::p_[0]) >> 7;
    if (a + b > 220) a = 220 - b;
    const s32 turns = s32(s64(b) * 32767 / 255);
    len_q16_ = ms_q16(25.0) + ((s64(turns) * ms_q16(70.0)) >> 15);
    // allpass coefficient c = -(0.55 + 0.35 turns)
    apc_ = -s32(s64(s32(0.55 * 2147483648.0)) + ((s64(turns) * s32(0.35 * 2147483648.0)) >> 15));
    const s64 d = u15(B::p_[2]), d3 = (((d * d) >> 15) * d) >> 15;
    fb15_ = clampv<s32>(s32(((32767 - d3) * 31130) >> 15), 9830, 31130);  // 0.95 (1 - s2^3), at least 0.3
    const u64 tone = kTone.at(s32(s64(a) * 32767 / 220));
    lpk_ = onepole_coef(hz_inc(tone));
    hpk_ = svf_coef(hz_inc(u64(120) << 16), svf_k_q(0.7));
    const s64 m = u15(B::p_[3]);
    mix15_ = s32((m * m) >> 15);
    B::disp_[0] = int32_t((len_q16_ >> 16) * 1000 / kSampleRate);
    B::disp_[1] = fb15_;
    B::disp_[2] = int32_t(tone >> 16);
    B::disp_[3] = mix15_;
    B::dirty_ = false;
  }

  SpringLine spring_[C];
  Svf hp_;
  SvfCoef hpk_;
  s64 len_q16_ = 0;
  s32 apc_ = 0, fb15_ = 0, lpk_ = 0, mix15_ = 0;
};

}  // namespace

Effect* make_fx_spring(bool stereo, void* mem, size_t size) noexcept { return make_fx<SpringFx>(stereo, mem, size); }

}  // namespace op1::dsp
