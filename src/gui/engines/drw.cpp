// Synth engine screen "drw" (DR Wave). drw.svg is empty: the firmware draws the whole screen by code.
// Decoded from dr_wave_screen.cpp (draw = vtable slot at 0x0190FA08 -> 0x01976F8C; spec: SPEC-drw.md):
//
//   phase += tick_delta / 1024 * param[7] * -80          (static float 0x01CC1074, wrapped into [0, 255))
//   wave   = the engine's 256-sample single-cycle buffer (pointer at 0xFF906A94, int32; ":55 wave")
//   for i in 0..255:
//     p  = phase + i;  if (p > 255) p -= 255             (22.10 fixed)
//     x1 = p * 319 / 258,  x2 = (p + 2) * 319 / 258       (asserts :74..77: 0 <= x < kScreenWidth)
//     y  = 70 + 64 * clamp(wave[i] >> 6, -1024, 1024) / 1024
//     line (x1, y) - (x2, y), width 2.0, palette colour "red"
//
// i.e. 256 short overlapping horizontal dashes, nothing else (no labels, axis, bars or voice dots).
// The firmware's wave buffer is filled by the DrWave DSP (0x0197C668 and the six table fills 0x0197AE28..);
// the port's engine is designed by ear and does not export its cycle, so until it does (REQUEST in the
// report) the cycle is recomputed here from the knobs exactly as eng_drwave.cpp shapes it (ASSUMED source,
// full scale = +-1.0). The tick unit of the scroll rate is ASSUMED to be 1 ms.
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// One cycle of the port's drwave oscillator (mirror of drwave_shape in src/dsp/eng_drwave.cpp), -1..1.
float drw_shape(float ph, float b, float len, float pos) {
  float q = ph < b ? ph / b * 0.5f : 0.5f + (ph - b) / (1.f - b) * 0.5f;
  if (q >= len) return 0.f;
  const float x = q / len;
  auto shape = [x](int i) {
    switch (i) {
      case 0: return std::sin(2.f * kPi * x);
      case 1: { const float y = std::fmod(x + 0.25f, 1.f); return y < 0.5f ? 4.f * y - 1.f : 3.f - 4.f * y; }
      case 2: return x < 0.5f ? 2.f * x : 2.f * x - 2.f;
      default: return x < 0.5f ? 1.f : -1.f;
    }
  };
  const int idx = pos >= 2.f ? 2 : int(pos);
  const float f = pos - float(idx);
  return lerp(shape(idx), shape(idx + 1), f);
}

// Scroll state (the firmware keeps it in a static float of the screen's translation unit).
double g_drw_phase = 0.0, g_drw_last_t = -1.0;

void bind_drw(BindCtx&) {}

void draw_drw(BindCtx& c, svg::Image& img) {
  const InstrumentView& iv = c.inst();
  // ---- scroll phase: -80 units per 1024 ticks at param[7] = 1 (decoded), 1 tick = 1 ms (ASSUMED)
  const double dt = g_drw_last_t < 0 ? 0.0 : std::clamp(c.t - g_drw_last_t, 0.0, 0.25);
  g_drw_last_t = c.t;
  g_drw_phase += dt * 1000.0 / 1024.0 * double(q15(iv.knobs[7])) * -80.0;
  if (g_drw_phase < 0.0) g_drw_phase += 255.0;
  if (g_drw_phase > 255.0) g_drw_phase -= 255.0;
  if (!(g_drw_phase >= 0.0 && g_drw_phase <= 255.0)) g_drw_phase = 0.0;

  // ---- the cycle (stand-in for the engine's wave buffer, see header)
  const float wave = q15(iv.knobs[0]), phase = q15(iv.knobs[2]);
  const float pos = wave * 3.f, len = 1.f - 0.5f * wave, b = 0.5f - 0.42f * phase;

  // ---- the firmware's drawing: 256 horizontal 2-px dashes in palette red
  constexpr int N = 256;
  const float ph = float(g_drw_phase);
  for (int i = 0; i < N; ++i) {
    float p = ph + float(i);
    if (p > 255.f) p -= 255.f;
    const float x1 = p * 319.f / 258.f, x2 = (p + 2.f) * 319.f / 258.f;
    // the engine's cycle when it publishes one (InstrumentView::wave, Q16), else the stand-in shape
    const float v = std::clamp(iv.wave_valid ? float(iv.wave[i]) / 65536.f
                                             : drw_shape(float(i) / float(N), b, len, pos), -1.f, 1.f);
    const float y = 70.f + 64.f * v;
    svg::stroke(img, {{x1, y}, {x2, y}}, 2.f, kColRed, false, 1.f, false);
  }
}

OP1_SCREEN("drw", bind_drw, draw_drw);

}  // namespace
}  // namespace op1
