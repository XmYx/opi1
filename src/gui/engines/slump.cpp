// Synth engine screen "slump" (voltage engine). Decoded from the firmware's slump screen (vtable 0x0191188C:
// load 0x01A4A0C4, draw 0x01A4A33C, static init of its tables 0x01A4BA78; spec: SPEC-slump.md).
// p0..p3 = the four knob parameters normalised 0..1 (blue, green, white, orange).
//
//   amp      the needle turns about the centre of "ampbase" by 1.2 * p0 - 0.6 rad (clockwise positive):
//            a 3x3 matrix T(c) R T(-c) built from the 1024-entry sine table.                     (decoded)
//   volts    text "%02i" of int(370 * p3 + 10) (orange): 10 .. 380.                                   (decoded)
//   left / right  the terminal groups move apart: left by -50 * p1, right by +50 * p1 px.            (decoded)
//   the wire 33 segments (34 points) from the inner edge of "l" (+50 p1) to the inner edge of "r" (-50 p1)
//            at the height of the centre of "l", width 2:
//              y offset of point 1..32 = - multilinear interpolation over (p0, p1, p2, p3) of 16 shapes of
//                32 offsets (the p1 = 1 shapes scaled by 1.5); the end points stay at 0           (decoded)
//              x offset of point 1..32 = engine sample(i) * level * 10 px + LCG noise 0..1.245 px    (decoded)
//            colour palette "white"; while (sample 16 * level) > (1.1 - p3)^2 "offred", < -(1.1 - p3)^2
//            "red"; a random flicker (on 0..3 frames, off 0..7 frames) turns it #E6E6E6.            (decoded)
//   level = (max(10 log10(output peak), -24) + 24) / 24                                  (decoded; input scale ASSUMED)
// The firmware draws nothing else by code (the old "filter bead" and the scope trace are gone).
// The engine's 32 display samples (firmware: 32 int16 from the last rendered block, stride 4, buffer
// 0x01DF6154) are taken from the output scope here (ASSUMED stand-in; REQUEST in the report).
#include <cstdio>

#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// The 16 wire shapes from the static init at 0x01A4BA78 (22.10 fixed-point pixel offsets, 32 interior
// points each), indexed by corner = b0 + 2 g + 4 w + 8 o (1 = that knob at 1).
constexpr int32_t kWire[16][32] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, -6277, 0, 0, 4997, -5406, 33198, -35246, 4259, -5765, 0, 0, 4741, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // b0 g0 w0 o0: table @0x01DF6334
    {0, 0, -5734, 0, 0, 0, 0, 6144, 0, 0, 0, -25907, 0, -26214, 0, 0, 31436, 0, 31539, 0, 0, -26132, 0, -26214, 6963, 0, 0, 0, -6553, 0, 0, 0},  // b1 g0 w0 o0: table @0x01DF63B4
    {0, 0, 0, 3072, -7403, 0, 0, 0, 0, -17643, 0, 0, 2672, -2048, 27883, -27883, 2406, -3246, 0, 0, 17643, 0, 0, 0, 0, 0, 7403, -3072, 0, 0, 0, 0},  // b0 g1 w0 o0: table @0x01DF6734
    {0, 0, -9830, 0, 0, 0, 0, 19384, 0, 0, 0, -25907, 0, -26214, 0, 0, 31436, 0, 31539, 0, 0, -26132, 0, -26214, 19384, 0, 0, 0, -9625, 0, 0, 0},  // b1 g1 w0 o0: table @0x01DF67B4
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11980, 0, 0, 0, -9216, 0, 9011, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // b0 g0 w1 o0: table @0x01DF6434
    {0, 0, 0, -7526, 0, 0, 0, 0, 0, 11366, 0, 0, -14581, 0, 0, 11366, 0, 0, -14581, 0, 0, 11366, 0, 0, 0, 0, 0, 0, -6758, 0, 0, 0},  // b1 g0 w1 o0: table @0x01DF6534
    {0, 0, 0, 0, -9216, 0, 0, 11980, 0, 0, 0, 0, 11980, 0, 0, 0, -9216, 0, 9011, 0, 0, 0, 0, 0, 0, 0, 12185, 0, 0, -9216, 0, 0},  // b0 g1 w1 o0: table @0x01DF6834
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 26009, 0, 0, -22323, 0, 0, 26009, 0, 0, -22323, 0, 0, 26009, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // b1 g1 w1 o0: table @0x01DF6934
    {0, 0, -4096, 0, 0, 0, 0, 20787, 0, 0, 0, 5017, 0, 0, 0, -20992, 0, 0, 0, 5017, 0, 0, 0, 20787, 0, 0, 0, 0, -4096, 0, 0, 0},  // b0 g0 w0 o1: table @0x01DF64B4
    {0, 0, 0, 0, 0, 0, 0, 0, 7577, 0, 0, 0, 0, 0, 0, 0, 0, -24473, 23654, -9441, 0, 0, 0, 0, 0, -6246, 0, 0, 0, 0, 0, 0},  // b1 g0 w0 o1: table @0x01DF65B4
    {0, 0, -15329, 0, 0, 0, 0, 13076, 0, 0, 0, 25364, 0, 0, 0, -12032, 0, 0, 0, 25364, 0, 0, 0, 13076, 0, 0, 0, 0, -15329, 0, 0, 0},  // b0 g1 w0 o1: table @0x01DF68B4
    {0, 0, 0, 0, -5734, 0, 0, 0, 24350, 0, 0, 0, 0, 0, 0, 0, 0, -24473, 23654, -9441, 0, 0, 0, 0, 0, -25169, 0, 0, 0, 3471, 0, 0},  // b1 g1 w0 o1: table @0x01DF69B4
    {0, 0, 0, 0, 0, 0, 0, 0, 7577, 0, 0, 0, 0, 0, -10209, 10270, -9441, 0, 0, 0, 0, 0, -6246, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // b0 g0 w1 o1: table @0x01DF6634
    {0, -5734, 0, 0, 0, 0, 6144, 0, 0, 0, -25907, 0, 0, 0, 31436, 0, 0, 0, 0, -26112, 0, 0, 0, 0, 0, 6963, 0, 0, 0, -6553, 0, 0},  // b1 g0 w1 o1: table @0x01DF66B4
    {0, 0, 0, 0, 0, 0, 0, 12400, 18124, 0, 0, 0, 0, 0, -10209, 10270, -9441, 0, 0, 0, 0, 0, -16691, 8079, 0, 0, 0, 0, 0, 0, 0, 0},  // b0 g1 w1 o1: table @0x01DF6A34
    {0, -14561, 0, 0, 0, 0, 21841, 0, 0, 0, -13711, 0, 0, 0, 13772, 0, 0, 0, 0, -13916, 0, 0, 0, 0, 0, 21841, 0, 0, 0, -16926, 0, 0},  // b1 g1 w1 o1: table @0x01DF6AB4
};

constexpr uint32_t kColOffRed = 0x4E2832, kColFlicker = 0xE6E6E6;

// Wire state the firmware keeps in the screen object / globals: LCG seed, flicker flag, frame counter.
uint32_t g_seed = 0x12345678u;
bool g_flicker = false;
uint32_t g_count = 0, g_dur = 32;

uint32_t lcg() { return g_seed = g_seed * 1664525u + 1013904223u; }

float level_of(const InstrumentView& iv) {
  const float pk = peak(iv);
  if (pk <= 0.f) return 0.f;
  const float db = std::max(10.f * std::log10(pk), -24.f);
  return (db + 24.f) / 24.f;
}

// Sample i (0..31) of the engine's last block, -1..1.
float wire_sample(const BindCtx& c, int i) {
  const int k = std::clamp(int(kDisplayW) - 128 + 4 * i, 0, int(kDisplayW) - 1);
  return float(c.s.scope[k]) / 2147483648.f;
}

void bind_slump(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const float p0 = knob(iv, 0), p1 = knob(iv, 1);
  // needle about the centre of ampbase
  const svg::Box ab = c.box("ampbase");
  if (!ab.empty()) rotate(c, c.n("amp"), (1.2f * p0 - 0.6f) * 180.f / kPi, ab.cx(), ab.cy());
  show(c.n("volts"), !c.font.ok());  // the number is drawn by draw_slump
  const float off = 50.f * p1;
  move(c.n("left"), -off, 0.f);
  move(c.n("right"), off, 0.f);
}

void draw_slump(BindCtx& c, svg::Image& img) {
  const InstrumentView& iv = c.inst();
  const float p[4] = {knob(iv, 0), knob(iv, 1), knob(iv, 2), knob(iv, 3)};
  // ---- volts
  const svg::Box vb = c.box("volts");
  if (c.font.ok() && !vb.empty()) {
    char buf[4];
    std::snprintf(buf, sizeof buf, "%02i", int(370.f * p[3] + 10.f));
    text_right(c.font, img, vb.x1, vb.y0, vb.h(), buf, kColRed);  // in the authored digits' box
  }
  // ---- wire geometry
  const svg::Box l = c.box("l"), r = c.box("r");  // "l" is the dot of the right group, "r" of the left one
  const float off = 50.f * p[1];
  if (l.empty() || r.empty()) return;
  const float x0 = l.x0 + off, xn = r.x1 - off, cy = l.cy();
  const float dx = (xn - x0) / 33.f;
  const float lvl = level_of(iv);
  float ax[34] = {}, ay[34] = {};
  for (int i = 0; i < 32; ++i) {
    float y = 0.f;
    for (int k = 0; k < 16; ++k) {
      float w = (k & 2) ? 1.5f : 1.f;  // the green = 1 shapes are scaled by 1.5
      for (int d = 0; d < 4; ++d) w *= ((k >> d) & 1) ? p[d] : 1.f - p[d];
      y += w * float(kWire[k][i]) / 1024.f;
    }
    ay[i + 1] = -y;
    const float noise = float(lcg() >> 24) * 5.f / 1024.f;
    ax[i + 1] = wire_sample(c, i) * lvl * 10.f + noise;
  }
  // ---- colour
  uint32_t col = kColWhite;
  const float s16 = wire_sample(c, 16) * lvl, thr = (1.1f - p[3]) * (1.1f - p[3]);
  if (s16 > thr) col = kColOffRed;
  else if (s16 < -thr) col = kColRed;
  if (g_dur < g_count) {
    g_count = 0;
    const uint32_t s = lcg();
    g_dur = g_flicker ? s >> 29 : s >> 30;
    g_flicker = !g_flicker;
  } else {
    ++g_count;
  }
  if (g_flicker) col = kColFlicker;
  for (int i = 0; i < 33; ++i) {
    svg::stroke(img, {{x0 + float(i) * dx + ax[i], cy + ay[i]}, {x0 + float(i + 1) * dx + ax[i + 1], cy + ay[i + 1]}},
                2.f, col, false, 1.f, false);
  }
}

OP1_SCREEN("slump", bind_slump, draw_slump);

}  // namespace
}  // namespace op1
