// sampler (id 6, SynthSampler.cpp): one sample played chromatically. OG facts used: 6 s sample slot, loop in/out,
// `base_freq` (root frequency in Hz, float) stored per preset, all 8 knobs used by the factory presets, no db
// default row (firmware-behaviour.md §4, og-behaviour-details.md §4). The sample comes from the firmware image's
// factory presets (state/worker.cpp) or the user (InstrumentOp::SetSample). The firmware's playback routine is
// not decoded: interpolation and loop behaviour are designed here.
//
// Knobs (u15 fractions of the sample length unless noted):
//   0 start, 1 loop in, 2 loop out, 3 end;
//   4 direction: forward below 16384, reverse from 16384. The screen has exactly two direction elements,
//     play_fwd and play_rev (sampler.svg); all 9 factory presets store 9728..12000 and play forward.
//     (Until t2, 2026-10-23: three bands incl. ping-pong, so 8 of 9 factory presets played reversed.)
//   5, 6 fine adjustment of loop in / loop out: as the firmware's sampler screen computes them (0x01A34FB0,
//     decoded by t3): loop in = max(start, k1 + k5 / 32768), loop out = max(loop in, k2 + k6 / 32768), with every
//     knob as a 0..1 fraction, i.e. k5/k6 add up to 1/32768 of the sample length (~8 frames of 6 s).
//   7 gain = 16 (k7 / 32768)^2: unity at 8192, x16 (+24 dB) at the top, silent at 0 (same screen code: display
//     [23] = 200 log10(16 p^2)). Factory values 7864..17377 = -0.7 .. +6.5 dB.
// There is no loop crossfade (the firmware has no knob for one; t2 had assumed k5 was one).
// The loop plays whenever loop out is after loop in (it runs on through the release); without a loop the
// voice stops at `end`. Stereo samples are summed to mono (OG instruments are mono).
//
// display[16..]: [16] sample length (frames, 0 = none), [17] start, [18] loop in, [19] loop out, [20] end
// (frames), [21] direction 0 forward / 1 reverse, [22] root frequency in Hz x 100, [23] gain in dB x 10.
#include <algorithm>
#include <cmath>

#include "synth_instrument.h"

namespace op1::dsp {
namespace {

struct SamplerEngine {
  static constexpr InstrumentType kType = InstrumentType::Sampler;
  struct Voice {
    u64 pos = 0;   // Q32.32 frames
    int dir = 1;
    bool started = false, done = true;
  };

  const SampleData* smp = nullptr;
  u32 root_inc = 0;          // phase increment of the root frequency
  u32 len = 0;               // frames in the sample
  u32 start = 0, lo = 0, hi = 0, end = 0;
  int direction = 0;
  s32 gain15 = 32768;        // Q15, up to 4x (+12 dB)
  s32 gain_db10 = 0;

  SamplerEngine() noexcept { set_root(261.63f); }

  void set_root(float hz) noexcept {
    if (!(hz > 1.0f) || hz > 20000.0f) hz = 261.63f;  // a missing or broken base_freq plays at C4
    root_inc = hz_inc(u64(double(hz) * 65536.0));
    root_hz100 = s32(hz * 100.0f);
  }
  s32 root_hz100 = 26163;

  bool command(const Command& c) noexcept {
    if (InstrumentOp(c.op) != InstrumentOp::SetSample) return false;
    smp = static_cast<const SampleData*>(c.ptr);
    len = (smp && smp->frames && smp->length > 1) ? u32(smp->length) : 0;
    if (smp && smp->root_hz > 0) set_root(smp->root_hz);
    if (!len) smp = nullptr;
    update_points();
    return true;
  }
  void load_patch(const SynthPatch& p) noexcept { set_root(p.base_freq); }

  s32 k_[kParams] = {};
  void control(const s32 k[kParams]) noexcept {
    for (int i = 0; i < kParams; ++i) k_[i] = k[i];
    direction = u15(k[4]) >= 16384 ? 1 : 0;
    // gain = 16 p^2, p = k7 / 32768, in Q15: k7^2 >> 11 (8192 -> 32768 = unity)
    const s64 g7 = u15(k[7]);
    gain15 = s32((g7 * g7) >> 11);
    gain_db10 = gain15 > 0 ? s32(std::lround(200.0 * std::log10(double(gain15) / 32768.0))) : -999;
    update_points();
  }
  void update_points() noexcept {
    if (!len) { start = lo = hi = end = 0; return; }
    auto at = [&](s32 v) { return u32((u64(u15(v)) * (len - 1)) / 32767); };
    // coarse + fine / 32768, as a fraction of the length (both knobs 0..1), clamped to the sample
    auto fine = [&](s32 c, s32 f) {
      const u64 q = u64(u15(c)) * 32768 + u64(u15(f));  // (c + f / 32768) * 32767 * 32768
      return u32(std::min<u64>(q * (len - 1) / (u64(32767) * 32768), len - 1));
    };
    start = at(k_[0]);
    end = at(k_[3]);
    lo = std::max(start, fine(k_[1], k_[5]));
    hi = std::max(lo, fine(k_[2], k_[6]));
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (legato && !v.done) return;
    v.started = false;
    v.done = false;
  }

  s32 frame_at(u32 i) const noexcept {
    if (i >= len) return 0;
    if (smp->channels == 2) return (s32(smp->frames[2 * i]) + smp->frames[2 * i + 1]) * 32768;
    return s32(smp->frames[i]) * 65536;
  }
  s32 read(u64 pos) const noexcept {
    const u32 i = u32(pos >> 32);
    const s32 f = s32((pos >> 16) & 0xffff);
    const s32 a = frame_at(i), b = frame_at(i + 1);
    return interp16(a, b, f);
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    if (!smp || !len || v.done) {
      for (int i = 0; i < n; ++i) out[i] = 0;
      v.done = true;
      return;
    }
    const bool reverse = direction == 1;
    const bool looping = hi > lo + (len >> 9) + 2;
    if (!v.started) {
      v.pos = u64(reverse ? end : start) << 32;
      v.dir = reverse ? -1 : 1;
      v.started = true;
    }
    // playback rate = note / root, corrected for the sample's own rate, in Q32 frames per output frame
    const u64 rate_q32 = ((u64(inc) << 32) / (root_inc ? root_inc : 1)) *
                         u64(smp->sample_rate > 0 ? smp->sample_rate : kSampleRate) / kSampleRate;
    const u64 lo64 = u64(lo) << 32, hi64 = u64(hi) << 32, span = hi64 - lo64;
    for (int i = 0; i < n; ++i) {
      if (v.done) { out[i] = 0; continue; }
      const s32 y = read(v.pos);
      out[i] = soft_clip((s64(y) * gain15) >> 15);
      if (v.dir > 0) v.pos += rate_q32;
      else v.pos = v.pos > rate_q32 ? v.pos - rate_q32 : 0;
      if (looping) {
        if (v.dir > 0 && v.pos >= hi64 && v.pos - rate_q32 < hi64) {
          v.pos -= span;
        } else if (v.dir < 0 && v.pos <= lo64 && v.pos + rate_q32 > lo64) {
          v.pos += span;
        }
      }
      const bool past = v.dir > 0 ? (v.pos >= (u64(end) << 32) || (v.pos >> 32) >= len - 1)
                                  : (v.pos <= (u64(start) << 32) || v.pos == 0);
      if (past && !(looping && v.pos >= lo64 && v.pos <= hi64)) v.done = true;
    }
  }

  void view(int32_t* d) const noexcept {
    d[0] = int32_t(len);
    d[1] = int32_t(start);
    d[2] = int32_t(lo);
    d[3] = int32_t(hi);
    d[4] = int32_t(end);
    d[5] = direction;
    d[6] = root_hz100;
    d[7] = gain_db10;
  }
  void view_full(InstrumentView& v) const noexcept { v.sample = len ? smp : nullptr; }
};

}  // namespace

Instrument* make_sampler(void* mem, size_t size) noexcept { return make_synth<SamplerEngine>(mem, size); }

}  // namespace op1::dsp
