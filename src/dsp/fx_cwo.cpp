// cwo (fx id 7, BodeAudioEngine.cpp): a Bode-style frequency shifter inside a delay feedback loop, so each
// echo climbs (or falls) by the shift. Designed by ear (the firmware routine is not decoded; t1 found a
// delay line plus sine/rate tables) on the OG facts: defaults 5000/8192/16384/32767.
//
// t5 (2026-10-23), from the firmware (decode-fx.md Q1/Q6; F = seen, I = this port's reading): every slot is stored
// 0..32767 (before t5 the port read slot 3 as signed, which the firmware never does).
// Params: blue (0) = shift, F: an offset from a centre value (s0 - C; C is computed at run time, I: 16384): below
//         the centre shifts down, above it up, I: 0.5 Hz .. 1.2 kHz exponential with the distance; green (1) =
//         delay time (I: 5 ms .. 1 s, squared curve, glides when turned), white (2) = feedback (I: 0 .. 0.95),
//         orange (3) = the echo level (I: 0 .. 0.8).
// The shifter is an analytic-signal (Hilbert) pair of 4 second-order allpasses per branch, multiplied by a
// quadrature oscillator. Output = dry x 0.75 + echoes.
// Mono: one loop. Stereo (master): F separate left / right oscillator phases with the rates rate +- o, the sign
// of o flipping every few blocks (I: o = 1 % of the rate, flipping every 64 blocks); the right delay 1.3 % longer.
//
// display[8..]: [8] shift in 0.1 Hz (signed: negative = down), [9] delay in ms, [10] feedback Q15, [11] echo
//               level Q15.
#include "fx_base.h"

namespace op1::dsp {
namespace {

// Niemitalo's Hilbert pair: two chains of 4 second-order allpasses y = c (x + y[n-2]) - x[n-2], c = a^2.
constexpr double kA[4] = {0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
constexpr double kB[4] = {0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};
constexpr s32 q31c(double a) { return s32(a * a * 2147483648.0); }
constexpr s32 kC[8] = {q31c(kA[0]), q31c(kA[1]), q31c(kA[2]), q31c(kA[3]),
                       q31c(kB[0]), q31c(kB[1]), q31c(kB[2]), q31c(kB[3])};

struct Hilbert {
  s32 s[8][4] = {};  // per stage: x1, x2, y1, y2
  s32 bdelay = 0;
  s32 re = 0, im = 0;
  void tick(s32 x) noexcept {
    s32 y = x;
    for (int i = 0; i < 4; ++i) {
      s32* st = s[i];
      const s32 v = sat(((s64(kC[i]) * (s64(y) + st[3])) >> 31) - st[1]);
      st[1] = st[0]; st[0] = y; st[3] = st[2]; st[2] = v;
      y = v;
    }
    s32 z = x;
    for (int i = 4; i < 8; ++i) {
      s32* st = s[i];
      const s32 v = sat(((s64(kC[i]) * (s64(z) + st[3])) >> 31) - st[1]);
      st[1] = st[0]; st[0] = z; st[3] = st[2]; st[2] = v;
      z = v;
    }
    re = y;
    im = bdelay;
    bdelay = z;
  }
};

template <int C>
class CwoFx final : public FxBase<C> {
  using B = FxBase<C>;

 public:
  CwoFx() noexcept { reset(); }
  void reset() noexcept override {
    for (auto& d : line_) d.clear();
    for (auto& h : hil_) h = Hilbert{};
    update();
    t_q16_ = target_q16_;
  }

  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept override {
    if (B::dirty_) update();
    const int ch = B::channels(r);
    constexpr s32 kGlide = s32(0.002 * 2147483648.0);
    for (int i = 0; i < ctx.frames; ++i) {
      t_q16_ += ((target_q16_ - t_q16_) * kGlide) >> 31;
      const u32 dinc = inc_ / 100;  // the stereo spread o
      for (int k = 0; k < ch; ++k) {
        const s32 c = cos_q31(phase_[k]), s = sin_q31(phase_[k]);
        phase_[k] += ch == 2 ? (k == (flip_ ? 1 : 0) ? inc_ + dinc : inc_ - dinc) : inc_;
        Sample* buf = k ? r : l;
        const s32 y = line_[k].readf(k ? t_q16_ * 1013 / 1000 : t_q16_);
        Hilbert& h = hil_[k];
        h.tick(sat(s64(buf[i]) + mul15(y, fb15_)));
        // up-shift = re c - im s, down-shift = re c + im s; mix = re c - im s * side
        const s64 rc = (s64(h.re) * c) >> 31, is = (s64(h.im) * s) >> 31;
        line_[k].write(soft_clip(down_ ? rc + is : rc - is));
        buf[i] = sat(s64(mul15(buf[i], 24576)) + mul15(y, wet15_));  // 0.75 dry + echoes
      }
    }
    if ((ctx.frame + ctx.frames) % kBlock == 0 && ++blocks_ % 64 == 0) flip_ = !flip_;
  }

 private:
  static constexpr int kLen = 65536;  // 1.49 s of 16-bit line per side

  void update() noexcept {
    static constexpr ExpCurve kShift(0.5, 1200.0);
    const s32 off = u15(B::p_[0]) - 16384;  // the centre (I)
    down_ = off < 0;
    const u64 hz = kShift.at(s32(clampv<s64>(s64(off < 0 ? -off : off) * 2, 0, 32767)));
    inc_ = hz_inc(hz);
    const s64 d = ms_q16(5.0) + ((s64(sq15(u15(B::p_[1]))) * ms_q16(1000.0)) >> 15);
    target_q16_ = clampv<s64>(d, s64(1) << 16, s64(kLen - 1024) << 16);
    fb15_ = s32((s64(u15(B::p_[2])) * 31130) >> 15);  // up to 0.95
    wet15_ = s32((s64(u15(B::p_[3])) * 26214) >> 15);  // up to 0.8
    B::disp_[0] = int32_t((hz * 10) >> 16) * (down_ ? -1 : 1);
    B::disp_[1] = int32_t((target_q16_ >> 16) * 1000 / kSampleRate);
    B::disp_[2] = fb15_;
    B::disp_[3] = wet15_;
    B::dirty_ = false;
  }

  DelayLine<kLen, int16_t> line_[C];
  Hilbert hil_[C];
  s64 t_q16_ = 0, target_q16_ = 0;
  u32 phase_[2] = {}, inc_ = 0;
  s32 fb15_ = 0, wet15_ = 0;
  bool down_ = false, flip_ = false;
  uint32_t blocks_ = 0;
};

}  // namespace

Effect* make_fx_cwo(bool stereo, void* mem, size_t size) noexcept { return make_fx<CwoFx>(stereo, mem, size); }

}  // namespace op1::dsp
