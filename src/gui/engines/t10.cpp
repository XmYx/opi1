// Synth engine screen "t10" (digital engine; t10.svg). Decoded from the firmware's T10 screen class by t3
// (SPEC-t10.md): ctor 0x01A524A0 (singleton at 0x01DF6FE4, vtable 0x01911A64), on-load 0x01A522E4,
// per-frame update 0x01A525DC. Evidence labels: decoded = read from the Blackfin code, inferred, ASSUMED.
//
// What the firmware does (decoded unless marked):
//  * on load it looks up blue0 blue1 blue2 green white red (names 0x018FB51C.., 8 bytes apart) and stores
//    the bbox centres of blue0, green, white and red; it REMOVES "line" from the document and keeps only its
//    stroke style (0x01A3B854 / 0x01A3B7D0).
//  * every frame six points (blue0, blue1, blue2, green, white, red) get a target T from the four
//    normalized parameters p0..p3 (blue, green, white, orange; 0..1) and are low-passed towards it with a
//    random "repel" offset; each dot node is translated to its point; the removed line's style draws seven
//    segments between the points (b0-b1, b0-b2, b0-g, b1-w, b2-r, g-r, r-w) BEFORE the document is
//    rendered, so the dots sit on top of the lines; the bbox of the six points re-centres the group.
//  * while a key is held the targets spread 25 % away from the screen centre (160, 80), ramped 10/s.
// The port draws the lines and then the dots by code (the dot nodes are hidden), which keeps that z-order.
// Geometry (dot size, colours, line colour/width, rest centres) is read from the loaded template.
// =====================================================================================================
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// The firmware's sine: a 1024-entry table indexed by floor(x * 1024 / 2pi) & 1023 (decoded: constant
// 0x28BE6 = 1024/2pi in Q10 at 0x01DF6FD0, table at 0x01D05BA4; table contents = sin, inferred).
float fw_sin(float x) {
  const int i = int(std::floor(x * (1024.f / (2.f * kPi)))) & 1023;
  return std::sin(2.f * kPi * float(i) / 1024.f);
}

struct T10State {
  bool init = false;
  double last_t = 0;
  float P[6][2] = {};   // drawn positions (screen object +0xA8), start at (0, 0) like the firmware's ctor
  float V[6][2] = {};   // repel offsets (+0x78), decay x 1022/1024 per frame
  float D[2] = {};      // re-centring term (+0xD8, +0xDC)
  float env = 0;        // key-held animation (0x01DF7100), 0..1 at 10/s
  bool env_set = false;
  uint32_t rng = 0;     // LCG state (L1 0xFF802BC8 in the firmware; its seed is not known)
};
T10State g_t10;

const char* const kT10Dots[6] = {"blue0", "blue1", "blue2", "green", "white", "red"};

// Normalized parameter 0..1 as the engine's parameter list gives it (0x01A28210 maps each stored value
// through a per-engine min/max table filled at run time; inferred). The digital engine's white knob is
// stored bipolar (-32768..32767, factory data), so it maps from that range (ASSUMED range).
float t10_param(const InstrumentView& iv, int i) {
  if (i == 2) return clamp01((float(iv.knobs[2]) + 32768.f) / 65535.f);
  return knob(iv, i);
}

void t10_step(BindCtx& c, const float ctr[6][2]) {
  T10State& S = g_t10;
  const InstrumentView& iv = c.inst();
  double dtd = S.init ? c.t - S.last_t : 1.0 / 60.0;
  S.last_t = c.t;
  S.init = true;
  if (dtd <= 0) return;  // same frame drawn again: no step
  const float dt = float(std::min(dtd, 0.05));  // frame time; clamped (the firmware is only stepped while shown)

  const float p0 = t10_param(iv, 0), p1 = t10_param(iv, 1), p2 = t10_param(iv, 2), p3 = t10_param(iv, 3);
  // key-held ramp: target 1 while any voice gate is set (0x01A27E48 checks 6 voice bytes; the port uses the
  // active-voice bits, inferred), Animation rate 10/s up and down (0x01A53714: 0x2800 = 10.0 Q10)
  const float tgt = sounding(iv) ? 1.f : 0.f;
  if (!S.env_set) { S.env = tgt; S.env_set = true; }
  if (S.env < tgt) S.env = std::min(tgt, S.env + 10.f * dt);
  else S.env = std::max(tgt, S.env - 10.f * dt);

  const float A = 25.f * p3 * fw_sin(20.f * p3);             // orange: amplitude
  const float n = float(int(6.f * p1 + 0.5f) - 3);           // green: 7 steps -3..3
  const float W = 100.f * p2 - 50.f;                         // white: -50..50 px
  float T[6][2];
  for (int i = 0; i < 3; ++i) {
    const float a = p0 + 0.1f + 0.5f * float(i) + 0.2f * p3 - 0.1f * n;
    const float b = p0 + 0.1f - 3.f * float(i) + 0.5f * n;
    T[i][0] = ctr[0][0] - 25.f * a * fw_sin(a);              // mirrored about blue0's centre (decoded)
    T[i][1] = ctr[0][1] + 25.f * a * fw_sin(b);
  }
  T[3][0] = ctr[3][0] + 25.f * n + W * 0.25f;
  T[3][1] = ctr[3][1] - 16.f * n - W * (163.f / 1024.f);
  T[4][0] = ctr[4][0] + W;
  T[4][1] = ctr[4][1] + (p2 > 0.5f ? 50.f - A * (102.f / 1024.f) : -50.f + A * (102.f / 1024.f));
  T[5][0] = ctr[5][0] + 100.f * p3;
  T[5][1] = ctr[5][1] - A;
  const float k = S.env * 0.25f;
  for (auto& t : T) {
    t[0] += (t[0] - 160.f) * k;
    t[1] += (t[1] - 80.f) * k;
  }
  for (auto& v : S.V) { v[0] *= 1022.f / 1024.f; v[1] *= 1022.f / 1024.f; }
  // repel: two points closer than 20 px on both axes kick point i by a random offset (LCG 1664525,
  // 1013904223; top byte signed; x 20/1024 px)
  for (int i = 0; i < 6; ++i)
    for (int j = 0; j < 6; ++j) {
      if (i == j) continue;
      const float dx = (T[i][0] + S.V[i][0]) - (T[j][0] + S.V[j][0]);
      const float dy = (T[i][1] + S.V[i][1]) - (T[j][1] + S.V[j][1]);
      if (std::fabs(dx) >= 20.f || std::fabs(dy) >= 20.f) continue;
      S.rng = S.rng * 1664525u + 1013904223u;
      const int r1 = int32_t(S.rng) >> 24;
      S.rng = S.rng * 1664525u + 1013904223u;
      const int r2 = int32_t(S.rng) >> 24;
      S.V[i][0] += float(r1) * (20.f / 1024.f);
      S.V[i][1] += float(r2) * (20.f / 1024.f);
    }
  const float s = std::min(8.f * dt, 0.5f);
  for (int i = 0; i < 6; ++i)
    for (int a = 0; a < 2; ++a) S.P[i][a] = (1.f - s) * S.P[i][a] + s * (T[i][a] + S.V[i][a] - S.D[a]);
  // re-centre: D = (min + max) - screen size, from the drawn points (min starts at 1000, max at 0)
  float mn[2] = {1000.f, 1000.f}, mx[2] = {0.f, 0.f};
  for (auto& p : S.P)
    for (int a = 0; a < 2; ++a) { mn[a] = std::min(mn[a], p[a]); mx[a] = std::max(mx[a], p[a]); }
  S.D[0] = mn[0] + mx[0] - 320.f;
  S.D[1] = mn[1] + mx[1] - 160.f;
}

void bind_t10(BindCtx& c) {
  float ctr[6][2];
  for (int i = 0; i < 6; ++i) {
    // blue1 and blue2 are translated relative to blue0's centre in the firmware (it stores only blue0's)
    const svg::Box b = c.box(kT10Dots[i == 1 || i == 2 ? 0 : i]);
    ctr[i][0] = b.empty() ? 160.f : b.cx();
    ctr[i][1] = b.empty() ? 80.f : b.cy();
  }
  t10_step(c, ctr);
  show(c.n("line"), false);  // removed from the document; its style draws the seven segments
  for (const char* id : kT10Dots) show(c.n(id), false);  // drawn by code above the lines (draw_t10)
}

void draw_t10(BindCtx& c, svg::Image& img) {
  const T10State& S = g_t10;
  const svg::Node* line = c.n("line");
  const uint32_t lc = line ? colour_of(line, "stroke") : 0x9256D7;
  const float lw = attr_f(line, "stroke-width", 1.5f);
  static const int kSeg[7][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 4}, {2, 5}, {3, 5}, {5, 4}};
  for (const auto& sg : kSeg)
    svg::stroke(img, {{S.P[sg[0]][0], S.P[sg[0]][1]}, {S.P[sg[1]][0], S.P[sg[1]][1]}}, lw, lc);
  // dots in document order (white, red, green, blue2, blue1, blue0: blue0 on top), each at its own
  // authored place moved by (point - reference centre), size and colour from the template
  const svg::Box ref0 = c.box("blue0");
  for (const int k : {4, 5, 3, 2, 1, 0}) {
    const svg::Node* n = c.n(kT10Dots[k]);
    const svg::Box b = c.box(kT10Dots[k]);
    if (!n || b.empty()) continue;
    const svg::Box r = (k < 3) ? ref0 : b;
    const float cx = b.cx() + S.P[k][0] - r.cx(), cy = b.cy() + S.P[k][1] - r.cy();
    svg::Points pts;
    for (int j = 0; j < 32; ++j) {
      const float a = 2.f * kPi * float(j) / 32.f;
      pts.emplace_back(cx + 0.5f * b.w() * std::cos(a), cy + 0.5f * b.h() * std::sin(a));
    }
    svg::fill(img, pts, colour_of(n, "fill"));
  }
}

OP1_SCREEN("t10", bind_t10, draw_t10);

}  // namespace
}  // namespace op1
