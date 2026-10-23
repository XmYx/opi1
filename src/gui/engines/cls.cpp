// Synth engine screen "cls" (cluster; cls.svg; firmware Cls.cpp screen class, vtable 0x190f6ac).
// Decoded from the firmware (t3, see SPEC-cls.md): init 0x01960a70, per-frame update 0x01960bf8.
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// =====================================================================================================
// What the firmware does (decoded unless marked):
//  * init: caches the stroke colours of on0..3 / off0..3 and REMOVES those paths from the document
//    (0x1a3b854 colour, 0x1a3b7d0 = find + remove). The authored curves are never shown; the layers are
//    drawn by code each frame as 1.5 px polylines in those colours. It keeps bar0..3 and the centre of
//    base0..3's bounds.
//  * dials: bar<i> rotates clockwise by param[i] * 1.5 pi (270 degrees) about base<i>'s centre (fixed-
//    point Q10 maths, sine table of 1024 entries). param[] = the four engine parameters 0..1.
//  * curves: four ring buffers of 80 columns (one per layer, Q10 pixels, at 0x1baaa00). Every other
//    frame the write cursor advances one column; on the frames between, each layer j writes
//    level[osc(j)] * gain(j) * 0.5 into the column `lag` behind the cursor (and clears the cursor column),
//    where osc = {0, 1, 3, 6}, gain = {-300, -250, -200, -150} px, lag = 4 * (4 - n) + 8, and
//    level[] is the cluster DSP's per-oscillator level array (int16 Q15 at 0x1baa93c, decayed by
//    0x7f00/0x8000 four times per update). Layer j is drawn back to front (j = 3 .. 0) as a polyline of
//    ceil(160/s)+1 points, s = 6 - j: point k sits at x = 2 s k (-1 px on the write frames) and shows
//    buffer column pos + 40 - 80/s + k, y = base(j) + 15 + value, base = {96, 76, 60, 48}. So the back layer
//    is a compressed view (6 px per column), the front layer a wide one (12 px per column): perspective.
//  * lit layers: n = {0,1,1,2,2,2,3,3}[(int)(param[0] * 7 + 0.5)]; layers 0..n use the on<j> colour,
//    the others the off<j> colour.
// The polylines are drawn here into the on<j> / off<j> nodes themselves (re-pathed; the unused one hidden),
// which keeps the firmware's paint order (curves under the dial group) without copying any artwork.
// =====================================================================================================

constexpr int kCols = 80;                                  // columns per layer ring buffer
constexpr int kOscOfLayer[4] = {0, 1, 3, 6};               // 0x18d2424
constexpr float kGain[4] = {-300.f, -250.f, -200.f, -150.f};  // -150 - 50 * (3 - j), 0x18d2414 / 0x18d240c
constexpr int kLit[8] = {0, 1, 1, 2, 2, 2, 3, 3};          // 0x18d2434
constexpr float kBaseY[4] = {96.f, 76.f, 60.f, 48.f};      // 40 + sum(8 + 4 m, m = 1 .. 4 - j)
constexpr float kYOff = 15.f;                              // added to every buffer value (0x3c00 Q10)
// The write mix: min(2 * frame argument, 0.5) in the firmware. The argument (R1 of the update method) was not
// traced; 0.5 is INFERRED: only the clamp value gives bumps of the authored size.
constexpr float kMix = 0.5f;
// ASSUMED: the firmware screen update rate (not decoded). The cursor advances every second update.
constexpr double kFrameHz = 30.0;

struct ClsState {
  float buf[4][kCols] = {};
  int pos = 0;          // write cursor (0x1baa984)
  bool phase = false;   // 0x1baa988: false = advance frame, true = write frame
  float xoff = 0;       // 0x1baa98c: 0 or -1 px
  long frame = -1;      // last firmware frame stepped
  uint32_t step = 0;    // write frames done (drives the ASSUMED level noise)
};
ClsState g_cls;

// Smooth per-oscillator random hills 0 .. 1 (ASSUMED stand-in for the DSP's per-oscillator level, which the
// firmware recomputes per audio block with an LCG random term; its exact shape is not decoded).
float osc_wobble(int osc, uint32_t step) {
  const uint32_t k = step / 3, f = step % 3;
  auto knot = [&](uint32_t i) {
    const float r = float(hash32(i * 2654435761u ^ uint32_t(osc) * 97u) & 0xffff) / 65535.f;
    return r * r;
  };
  const float t = float(f) / 3.f, s = t * t * (3.f - 2.f * t);
  return lerp(knot(k), knot(k + 1), s);
}

// level[osc] 0..1: the DSP's per-oscillator level. REQUESTED from the engine as display[20..23] (Q15, oscillators
// 0, 1, 3, 6). Until the engine fills them (all zero while a voice sounds) it is derived from the port's own
// engine state (ASSUMED): active waves (display[16]) x loudest voice envelope x a slow random wobble.
float osc_level(const InstrumentView& iv, int layer, uint32_t step) {
  bool have = false;
  for (int i = 0; i < 4; ++i) have |= iv.display[20 + i] != 0;
  if (have) return q15(iv.display[20 + layer]);
  const int osc = kOscOfLayer[layer];
  const int waves = std::clamp(int(iv.display[16]), 1, 12);
  if (osc >= waves) return 0.f;
  return 0.45f * voice_env(iv) * osc_wobble(osc, step);
}

// One firmware screen update of the curve state (0x01961072 .. 0x01961210, 0x01961418).
void cls_frame(const InstrumentView& iv, int n) {
  ClsState& s = g_cls;
  if (!s.phase) {
    s.pos = (s.pos + 1) % kCols;
    s.xoff = 0;
    s.phase = true;
    return;
  }
  const int lag = 4 * (4 - n) + 8;
  for (int j = 3; j >= 0; --j) {
    const float v = osc_level(iv, j, s.step) * kGain[j];
    s.buf[j][s.pos] = 0;
    float& d = s.buf[j][(s.pos - lag + kCols) % kCols];
    d = d * (1.f - kMix) + v * kMix;
  }
  ++s.step;
  s.phase = false;
  s.xoff = -1.f;
}

void bind_cls(BindCtx& c) {
  const InstrumentView& iv = c.inst();

  // dials: clockwise param * 270 degrees about the dot centre (decoded)
  for (int i = 0; i < 4; ++i) {
    svg::Node* bar = c.n("bar" + std::to_string(i));
    const svg::Box b = c.box("base" + std::to_string(i));
    if (bar && !b.empty()) rotate(c, bar, knob(iv, i) * 270.f, b.cx(), b.cy());
  }

  const int n = kLit[std::clamp(int(knob(iv, 0) * 7.f + 0.5f), 0, 7)];

  // advance the firmware frames that elapsed (bounded: a long absence just refills the buffers)
  const long now = long(c.t * kFrameHz);
  if (g_cls.frame < 0 || now < g_cls.frame) g_cls.frame = now - 1;
  for (long f = std::max(g_cls.frame + 1, now - 2 * kCols); f <= now; ++f) cls_frame(iv, n);
  g_cls.frame = now;

  for (int j = 0; j < 4; ++j) {
    svg::Node* on = c.n("on" + std::to_string(j));
    svg::Node* off = c.n("off" + std::to_string(j));
    svg::Node* use = j <= n ? on : off;
    show(on, false);
    show(off, false);
    if (!use) continue;
    const int st = 6 - j;                       // 3 (back) .. 6 (front)
    const float dx = 2.f * float(st);           // px per column
    const int pts_n = (st + 159) / st + 1;
    const int start = g_cls.pos + 40 - 80 / st;
    svg::Points pts;
    pts.reserve(size_t(pts_n) + 1);
    pts.emplace_back(-dx + g_cls.xoff, kBaseY[j]);  // the firmware's first segment starts from a zero value
    for (int k = 0; k < pts_n; ++k)
      pts.emplace_back(dx * float(k) + g_cls.xoff, kBaseY[j] + kYOff + g_cls.buf[j][(start + k) % kCols]);
    set_path(use, pts);
    show(use, true);
  }
}

OP1_SCREEN("cls", bind_cls, nullptr);

}  // namespace
}  // namespace op1
