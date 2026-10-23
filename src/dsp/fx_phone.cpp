// phone (fx id 3, Ptch.cpp; screen parts mic, chip, speaker, noise dots): a pitch-synchronous grain repeater.
// t5 (2026-10-23) rebuilt it on the firmware's structure (Ptch process, decode-fx.md Q5; F = seen in the code,
// I = this port's reading). Before t5 the port had an invented "telephone line" with constant hiss: the firmware
// has no noise source, so silence in gives silence out.
//   F: blue (0) = the replay rate, (s0^2 >> 16) << 5. I: read as Q15, so the default 8000 replays at ~0.95 x
//      and the top of the knob at 16 x.
//   F: green (1) = the grain length, s1 >> 11 pitch cycles, capped at n x 735 samples. I: n = cycles + 1, a cycle
//      = the input's period found from its rising zero crossings (20 .. 735 samples).
//   F: white (2) = repeats, s2 >> 10: each grain plays that many times (here at least once).
//   F: orange (3) = dry/wet.
//   F: three grain buffers rotate (the chip on the screen follows them); stereo = two independent mono instances,
//      so the master phone keeps its width.
//   I: 32-sample crossfades at grain edges, so the repeats do not click.
//
// display[8..]: [8] input period in samples, [9] grain length in samples, [10] repeats, [11] the playing grain
//               buffer 0..2 (left side), [12] repeat number of the playing grain, [13] wet level of the last block
//               (16-bit units).
#include <algorithm>

#include "fx_base.h"

namespace op1::dsp {
namespace {

constexpr int kHist = 16384;            // input history per side (power of two)
constexpr int kGrainMax = 16 * 735;     // 16 cycles of the longest period
constexpr int kFade = 32;

template <int C>
class PhoneFx final : public FxBase<C> {
  using B = FxBase<C>;

 public:
  PhoneFx() noexcept { reset(); }
  void reset() noexcept override {
    for (auto& s : side_) {
      for (auto& h : s.hist) h = 0;
      s.w = s.since = s.cur = s.rep = 0;
      s.period = 200;
      s.prev = 0;
      for (auto& g : s.g) { g.len = 0; g.pos = 0; }
    }
  }

  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept override {
    const int ch = B::channels(r);
    const s32 s0 = u15(B::p_[0]), s1 = u15(B::p_[1]), s2 = u15(B::p_[2]), s3 = u15(B::p_[3]);
    const s64 rate_q15 = std::max<s64>(1, ((s64(s0) * s0) >> 16) << 5);  // replay rate, Q15
    const int cycles = (s1 >> 11) + 1;
    const int repeats = std::max(1, s2 >> 10);
    s32 wet_peak = 0;
    for (int k = 0; k < ch; ++k) {
      Side& sd = side_[k];
      Sample* buf = k ? r : l;
      for (int i = 0; i < ctx.frames; ++i) {
        const s32 x = buf[i];
        // history and the pitch period from rising zero crossings
        sd.hist[sd.w] = int16_t(x >> 16);
        sd.w = (sd.w + 1) & (kHist - 1);
        ++sd.since;
        if (sd.prev < 0 && x >= 0) {
          if (sd.since >= 20 && sd.since <= 735) sd.period = sd.since;
          sd.since = 0;
        }
        sd.prev = x;
        // the playing grain
        s32 y = 0;
        Grain& g = sd.g[sd.cur];
        if (g.len > 0) {
          const int pi = int(g.pos >> 15);
          const s32 f = s32(g.pos & 0x7FFF);
          const s32 a = s32(g.d[pi]) << 16, b = s32(g.d[pi + 1 < g.len ? pi + 1 : pi]) << 16;
          y = sat(s64(a) + ((s64(b) - a) * f >> 15));
          const int edge = std::min(pi, g.len - 1 - pi);  // short fades at both ends
          if (edge < kFade) y = s32((s64(y) * edge) / kFade);
          g.pos += rate_q15;
          if ((g.pos >> 15) >= g.len) {
            g.pos = 0;
            if (++sd.rep >= repeats) next_grain(sd, cycles);
          }
        } else {
          next_grain(sd, cycles);
        }
        const s32 a = y < 0 ? (y == INT32_MIN ? kOne : -y) : y;
        if (a > wet_peak) wet_peak = a;
        buf[i] = sat((s64(x) * (32768 - s3) + s64(y) * s3) >> 15);
      }
    }
    B::disp_[0] = side_[0].period;
    B::disp_[1] = side_[0].g[side_[0].cur].len;
    B::disp_[2] = repeats;
    B::disp_[3] = side_[0].cur;
    B::disp_[4] = side_[0].rep;
    B::disp_[5] = wet_peak >> 16;
  }

 private:
  struct Grain {
    int16_t d[kGrainMax];
    int len;
    s64 pos;  // Q15 samples
  };
  struct Side {
    int16_t hist[kHist];
    int w, since, period, cur, rep;
    s32 prev;
    Grain g[3];
  };

  // The next of the three buffers takes the latest `cycles` periods of the input.
  static void next_grain(Side& sd, int cycles) noexcept {
    sd.cur = (sd.cur + 1) % 3;
    sd.rep = 0;
    Grain& g = sd.g[sd.cur];
    const int len = std::min(cycles * std::clamp(sd.period, 20, 735), kGrainMax);
    for (int i = 0; i < len; ++i) g.d[i] = sd.hist[(sd.w - len + i) & (kHist - 1)];
    g.len = len;
    g.pos = 0;
  }

  Side side_[C];
};

}  // namespace

Effect* make_fx_phone(bool stereo, void* mem, size_t size) noexcept { return make_fx<PhoneFx>(stereo, mem, size); }

}  // namespace op1::dsp
