// drwave (id 2, DrWave.cpp; preset folder "dr wave"): a morphing single oscillator through a resonant
// low-pass and a chorus. Designed by ear (the firmware routine is not decoded) on the OG facts: defaults
// 24000/16000/8200/24000/32000, slot 4 is used by every factory preset (32000..32767).
//
// Knobs: blue (0) = wave: morphs sine > triangle > saw > square while the wave is squeezed into the first
//        part of the cycle (the rest is silent), green (1) = filter (resonant low-pass 120 Hz .. 15 kHz),
//        white (2) = phase (phase distortion: skews the cycle), orange (3) = chorus (a modulated short delay
//        mixed in), slot 4 = wave level into the filter (0.25 .. 1, 32767 = unity; factory
//        presets use 32000..32767).
// The oscillator runs 2x oversampled through the filter.
//
// display[16..]: [16] wave Q15, [17] cutoff Hz, [18] phase Q15, [19] chorus Q15, [20] level Q15.
// view_full: InstrumentView::wave[256] = one cycle of the oscillator as it is played (shape x level), Q16,
// wave_valid = true. The shape depends only on the knobs, so it is the cycle of any (the last) rendered voice.
//
// Firmware model (t3's decode of the dr_wave screen, 2026-10-23; NOT implemented here, an open DSP gap): the
// firmware plays a 256-point cycle buffer per voice filled from run-time generated wavetables (A/B wave types
// and morph from blue, variant and table position from green, a post-process mode from white) and orange runs a
// sign-flip sweep through the cycle. This engine's knob meanings (morph, filter, phase, chorus) differ.
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

// One cycle of the oscillator at phase ph; b = PD breakpoint, len = played part of the cycle, pos = morph Q16.
s32 drwave_shape(u32 ph, u32 b, u64 len, s32 pos_q16) noexcept {
  const u32 q = ph < b ? u32((u64(ph) << 31) / b)
                       : u32(kHalfCycle + (u64(ph - b) << 31) / ((u64(1) << 32) - b));
  if (q >= len) return 0;
  const u32 x = u32((u64(q) << 32) / len);
  const int idx = pos_q16 >= (2 << 16) ? 2 : int(pos_q16 >> 16);
  const s32 f = pos_q16 - (idx << 16);  // Q16
  auto shape = [x](int i) noexcept -> s32 {
    switch (i) {
      case 0: return sin_q31(x);
      case 1: return tri_q31(x + (1u << 30));      // triangle in sine phase
      case 2: return s32(x);                         // saw through zero at x = 0
      default: return x < kHalfCycle ? kOne : -kOne; // square
    }
  };
  const s32 a = shape(idx);
  if (f == 0) return a;
  const s32 c = shape(idx + 1);
  return sat(s64(a) + (((s64(c) - a) * f) >> 16));
}

struct DrWaveEngine {
  static constexpr InstrumentType kType = InstrumentType::DrWave;
  struct Voice {
    u32 ph = 0, lfo = 0;
    Svf svf;
    DelayLine<1024> dl;  // 23 ms chorus line
  };

  u32 b = kHalfCycle;
  u64 len = u64(1) << 32;
  s32 pos_q16 = 0, chorus15 = 0, level15 = 32767, wave15 = 0, phase15 = 0;
  u32 cutoff_hz = 1000;
  SvfCoef coef;

  void control(const s32 k[kParams]) noexcept {
    static constexpr ExpCurve kCut(120.0, 15000.0);
    wave15 = u15(k[0]);
    phase15 = u15(k[2]);
    chorus15 = u15(k[3]);
    level15 = 8192 + s32(s64(u15(k[4])) * 24575 / 32767);  // 0.25 .. 1: never fully silent
    pos_q16 = s32((s64(wave15) * 3 * 65536) >> 15);
    len = (u64(1) << 32) - ((u64(wave15) << 32) >> 16);              // 1 - 0.5 wave
    b = u32(kHalfCycle - ((u64(phase15) * u64(0.42 * 4294967296.0)) >> 15));  // 0.5 - 0.42 phase
    const u64 hz = kCut.at(k[1]);
    cutoff_hz = u32(hz >> 16);
    coef = svf_coef(hz_inc(hz) / 2, svf_k_from_res(9830));  // at the 2x rate
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (!legato) v.ph = 0;
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    const u32 dt = inc / 2;
    constexpr s64 kBase = ms_q16(7.0);
    const s64 depth = ms_q16(0.5) + ((ms_q16(3.5) * chorus15) >> 15);
    constexpr u32 kLfoInc = u32(0.45 / kSampleRate * 4294967296.0);
    const s32 dry15 = 32767 - ((chorus15 * 14745) >> 15);  // 1 - 0.45 c
    const s32 wet15 = (chorus15 * 24576) >> 15;             // 0.75 c
    for (int i = 0; i < n; ++i) {
      s32 y = 0;
      for (int k = 0; k < 2; ++k) {
        const s32 s = mul15(drwave_shape(v.ph, b, len, pos_q16), level15);
        v.ph += dt;
        y = v.svf.tick(coef, s >> 1);
      }
      v.dl.write(y);
      const s64 d = kBase + ((depth * (sin_q31(v.lfo) >> 16)) >> 15);
      const s32 c = v.dl.readf(d);
      v.lfo += kLfoInc;
      out[i] = soft_clip(((s64(y) * dry15 + s64(c) * wet15) >> 15) * 3 / 2);
    }
  }

  void view_full(InstrumentView& v) const noexcept {
    for (int i = 0; i < 256; ++i)
      v.wave[i] = s32((s64(drwave_shape(u32(i) << 24, b, len, pos_q16)) * level15) >> 30);  // Q31 x Q15 -> Q16
    v.wave_valid = true;
  }

  void view(int32_t* d) const noexcept {
    d[0] = wave15;
    d[1] = int32_t(cutoff_hz);
    d[2] = phase15;
    d[3] = chorus15;
    d[4] = level15;
  }
};

}  // namespace

Instrument* make_drwave(void* mem, size_t size) noexcept { return make_synth<DrWaveEngine>(mem, size); }

}  // namespace op1::dsp
