// Synth engine screen "st" (string; st.svg). Decoded from the firmware's screen class (t3): constructor
// 0x01A4F734 ("st.svg"), element setup 0x01A4F500 (vtable 0x019118F4 slot 4), per-frame draw 0x01A4F8A8
// (slot 3). Full pseudo-code with addresses: SPEC-st.md (t3 work folder).
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// =====================================================================================================
// What the firmware does each frame (p0..p3 = the four knob values 0..1 from 0x01A28210):
//  * g0..g21 (decay meter): g<i> visible while i < int(22 * p1 + 0.5).                         [decoded]
//  * w0 / w1 (white handles): translated toward each other by (centre(w0) - centre(w1)) * p2 / 2;
//    at p2 = 1 both meet half way along the floor's front edge.                                  [decoded]
//  * line0..15: removed from the template at setup and redrawn by code with their own style as one
//    quadratic Bezier from the authored bounds' (min_x, min_y) to (max_x, max_y), control point
//    (centre_x + v[i], centre_y + (1 - p0) * 45 px): blue = 1 straight, blue = 0 sagging.          [decoded]
//    v[i] = horizontal string displacement, per firmware frame: while any voice sounds
//    v = 0.0996 v + 50 px * s[i] (s = cell i of the last-triggered voice's delay line, int16 / 32768);
//    otherwise v *= min(dt_ms * 30 / 1024, 0.989).                                                [decoded]
//    The port has no delay-line cells in the snapshot: s[i] = the last 16 output samples (scope),
//    which are consecutive samples of the same travelling wave (REQUEST in the t3 report).  [substitute]
//  * red: the dim authored line stays; on top a red (#FF3A5D, palette "red") 1.0 px line rises from its
//    bottom end by p3 * its height.                                                              [decoded]
// The firmware steps v once per screen frame; the port emulates that at kFwFrame (frame rate ASSUMED).
// =====================================================================================================

constexpr double kFwFrame = 1.0 / 30.0;  // ASSUMED OP-1 screen frame period (s)
constexpr uint32_t kPalRed = 0xFF3A5D;   // colors.svg "red" (palette entry 2, 0x01BAAF58)
constexpr float kScopeGain = 4.f;

struct StState {
  double last_t = -1, acc = 0;
  float v[16] = {};
};
StState g_st;

void step_strings(const CoreSnapshot& s, const InstrumentView& iv) {
  if (sounding(iv)) {
    for (int i = 0; i < 16; ++i) {
      // kScopeGain: the mixed output is ~4x quieter than one voice's delay line (ASSUMED stand-in)
      const float smp = std::clamp(float(s.scope[kDisplayW - 16 + i]) / 2147483648.f * kScopeGain, -1.f, 1.f);
      g_st.v[i] = g_st.v[i] * (102.f / 1024.f) + smp * 50.f;
    }
  } else {
    const float f = std::min(float(kFwFrame * 1000.0) * 30.f / 1024.f, 1013.f / 1024.f);
    for (float& v : g_st.v) v *= f;
  }
}

void bind_st(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const float p0 = knob(iv, 0), p1 = knob(iv, 1), p2 = knob(iv, 2);

  // the string displacements, stepped at the firmware's frame rate
  if (g_st.last_t < 0 || c.t < g_st.last_t) g_st.last_t = c.t;
  g_st.acc = std::min(g_st.acc + (c.t - g_st.last_t), 2.0);
  g_st.last_t = c.t;
  while (g_st.acc >= kFwFrame) {
    step_strings(c.s, iv);
    g_st.acc -= kFwFrame;
  }

  // decay meter
  const int lit = int(22.f * p1 + 0.5f);
  for (int i = 0; i < 22; ++i) show(c.n("g" + std::to_string(i)), i < lit);

  // handles
  const svg::Box b0 = c.box("w0"), b1 = c.box("w1");
  if (!b0.empty() && !b1.empty()) {
    const float dx = (b0.cx() - b1.cx()) * p2 * 0.5f, dy = (b0.cy() - b1.cy()) * p2 * 0.5f;
    move(c.n("w0"), -dx, -dy);
    move(c.n("w1"), dx, dy);
  }

  // the 16 strings
  const float sag = (1.f - p0) * 45.f;
  for (int i = 0; i < 16; ++i) {
    svg::Node* ln = c.n("line" + std::to_string(i));
    const svg::Points p = points_of(ln);
    if (p.size() < 2) continue;
    svg::Box b;
    for (const auto& q : p) b.add(q.first, q.second);
    const float x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1;
    const float cx = b.cx() + g_st.v[i], cy = b.cy() + sag;
    svg::Points q;
    constexpr int kSeg = 24;
    for (int k = 0; k <= kSeg; ++k) {
      const float t = float(k) / kSeg, u = 1.f - t;
      q.emplace_back(u * u * x0 + 2.f * u * t * cx + t * t * x1, u * u * y0 + 2.f * u * t * cy + t * t * y1);
    }
    set_points(ln, q);
  }
}

void draw_st(BindCtx& c, svg::Image& img) {
  const svg::Box r = c.box("red");
  if (r.empty()) return;
  const float h = (r.y1 - r.y0) * knob(c.inst(), 3);
  if (h > 0.01f) svg::stroke(img, {{r.x1, r.y1}, {r.x1, r.y1 - h}}, 1.0f, kPalRed);
}

OP1_SCREEN("st", bind_st, draw_st);

}  // namespace
}  // namespace op1
