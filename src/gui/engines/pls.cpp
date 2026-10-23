// Synth engine screen "pls" (pulse; pls.svg). Decoded from the firmware's PlsScreen (t3): constructor
// 0x01A0E4E4 ("pls.svg"), element setup 0x01A0EE38 (+0x01A0E610, 0x01A0E870, 0x01A0E99C), per-frame draw
// 0x01A10C3C (vtable 0x019111FC slot 3) and its parts. Pseudo-code with addresses: SPEC-pls.md.
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// =====================================================================================================
// Per frame (p0..p3 = knob values 0..1, dt = ms since the last frame):
//  0x01A0E6C0 (p0)   fltleft / fltright move toward each other by (1 - p0) * half their distance.
//  0x01A0FF84 (p1)   green height marker: greendottop and the dashed level line move with pulse 1's top
//                    ((1 - p1) * dot distance d); "greenh" (the H) slides (200 px/s) to the middle of the
//                    marker, or 13 px above the top dot while p1 < (d - 25) / 2d; the dashed verticals
//                    are split around the H (10 px gap) or, when short, one line to the top dot.
//                    The green items fade offgreen -> green: bright for 0.5 s after the green encoder
//                    turns, then fade over 1 s.
//  0x01A0EBBC        whitehbars scale vertically about the baseline by p1; whitetbars and righttdot move
//                    right by p2 * width(hbarspanx).
//  0x01A10B3C        tline stretches to tbar1; t_letter slides (200 px/s) to x = 39 p2 - 5 (>= 10 and 16 px
//                    up when p1 > 0.4 / p2 < 0.2757); the white items fade grey -> white after the white
//                    encoder turns (same timing).
//  template drawn, then by code:
//  0x01A10720 (p3)   red line from the centre of redlinebg, length (p3 - 0.5) * its width.
//  0x01A0F1F8 (p3)   pinkbars (node removed, path drawn in its style): 4 steps at height
//                    (1 + m sin(i + 1 + ph)) * h / 2, m = 2 p3 - 1; ph += 2 m (m >= 0) or 0.3 m per frame.
//  0x01A0ED2C        four white "glue" base lines between the pulses (hbarspanx itself is removed).
//  0x01A0F830        bluecurve (node removed, path drawn in its style): baseline with two soft pulses
//                    whose edges are S-curves (sharpness from p0), widths from p2 (and p0), heights from
//                    p1 / full, scaled by a note-activity envelope (up 10/s, down 0.5/s), gently wobbling.
// All decoded; see SPEC-pls.md for the evidence labels of the details.
// =====================================================================================================

constexpr double kFwFrame = 1.0 / 30.0;  // ASSUMED OP-1 screen frame period (s), for per-frame steps
constexpr uint32_t kPalRed = 0xFF3A5D, kPalWhite = 0xFFFFFF, kPalGrey = 0x7C7990, kPalGreen = 0x00ED95,
                   kPalOffGreen = 0x002826;  // colors.svg palette entries 2, 14, 12, 16, 6

// The firmware's linear slew (0x01919806 set target, 0x0191986C advance, 0x01919838 value): the first
// target is taken at once; then the value moves toward it by rate / 1024 per ms.
struct Slew {
  bool init = false;
  float cur = 0, target = 0, up = 0, down = 0;
  void set(float t) {
    target = t;
    if (!init) init = true, cur = t;
  }
  void advance(float ms) {
    if (!init || cur == target) return;
    if (target < cur) cur = std::max(target, cur - down * ms / 1024.f);
    else cur = std::min(target, cur + up * ms / 1024.f);
  }
};

struct PlsState {
  double last_t = -1, acc = 0;
  double enc_seen[4] = {-1e9, -1e9, -1e9, -1e9};
  Slew tx{false, 0, 0, 200, 200}, ty{false, 0, 0, 200, 200}, gh{false, 0, 0, 200, 200};
  Slew fade_g{false, 0, 0, 1, 1}, fade_w{false, 0, 0, 1, 1}, act{false, 0, 0, 10, 0.5f};
  float ph = 0;        // pinkbars phase (rad)
  float counter = 0;   // bluecurve wobble (rad; += dt_ms / 1024)
  float dt_ms = 0;
  uint32_t col_g = kPalOffGreen, col_w = kPalGrey;
};
PlsState g_pls;

// The firmware's 1024-entry sine table (indexed by angle * 1024 / 2 pi, truncated).
float tsin(float a) {
  const int i = int(std::floor(a * 1024.f / (2.f * kPi))) & 1023;
  return std::sin(float(i) * 2.f * kPi / 1024.f);
}

// One quadratic Bezier from the last point of pts.
void quad(svg::Points& pts, float cx, float cy, float x, float y, int n = 8) {
  const float x0 = pts.back().first, y0 = pts.back().second;
  for (int k = 1; k <= n; ++k) {
    const float t = float(k) / float(n), u = 1.f - t;
    pts.emplace_back(u * u * x0 + 2.f * u * t * cx + t * t * x, u * u * y0 + 2.f * u * t * cy + t * t * y);
  }
}

// 0x01A0F5A4: an S-shaped edge from the current point to (x, y); k = 1 - p0 sets how soft it is.
void edge(svg::Points& pts, float x, float y, float k) {
  const float px = pts.back().first, py = pts.back().second;
  const float mx = (x + px) / 2, my = (y + py) / 2;
  const float r = std::min(k * 1.5f, 1.f);
  const float a = r > 0.8f ? 2.f * r * (1.f - r) / 0.2f : 2.f * r;
  quad(pts, (px + mx) / 2, py, mx - a, py * (1 - r) + my * r);
  if (r < 993.f / 1024.f) quad(pts, mx, my, mx + a, y * (1 - r) + my * r);
  quad(pts, (mx + a + x) / 2, y, x, y);
}

void set_line_of(svg::Node* group, float y1, float y2) {  // the <line> inside a group (or the line itself)
  svg::Node* l = group;
  if (l && l->tag != "line" && !l->kids.empty()) l = &l->kids[0];
  if (!l || l->tag != "line") return;
  const float x = attr_f(l, "x1");
  set_line(l, x, y1, x, y2);
}

void bind_pls(BindCtx& c) {
  PlsState& S = g_pls;
  const InstrumentView& iv = c.inst();
  const float p0 = knob(iv, 0), p1 = knob(iv, 1), p2 = knob(iv, 2), p3 = knob(iv, 3);

  // time: dt in ms for the slews, whole firmware frames for the per-frame steps
  if (S.last_t < 0 || c.t < S.last_t) S.last_t = c.t;
  const double dts = std::min(c.t - S.last_t, 0.25);
  S.last_t = c.t;
  S.dt_ms = float(dts * 1000.0);
  S.acc = std::min(S.acc + dts, 2.0);
  // the firmware's per-frame encoder flags (0x01DD7F74 + n, set by an encoder event, cleared per frame)
  bool turned[4];
  for (int i = 0; i < 4; ++i) {
    turned[i] = c.ui.enc_t[i] != S.enc_seen[i] && c.ui.enc_t[i] > -1e8;
    S.enc_seen[i] = c.ui.enc_t[i];
  }

  // ---- 0x01A0E6C0: the two filter rings
  const svg::Box fl = c.box("fltleft"), fr = c.box("fltright");
  if (!fl.empty() && !fr.empty()) {
    const float hd = (fr.x0 - fl.x0) / 2.f * (1.f - p0);
    move(c.n("fltleft"), hd, 0);
    move(c.n("fltright"), -hd, 0);
  }

  // ---- 0x01A0FF84: the green height marker
  const svg::Box db = c.box("greendotbottom"), dtp = c.box("greendottop");
  if (!db.empty() && !dtp.empty()) {
    const float d = db.y1 - dtp.y1;
    const float r6 = (1.f - p1) * d;  // how far the top dot drops
    move(c.n("hlinehoris_7_"), 0, r6);  // the dashed level line (outer group of all six dashes)
    move(c.n("greendottop"), 0, r6);
    const bool split = !(((d + 25.f) / 2.f) < r6);
    S.gh.set(split ? -p1 * d / 2.f : -p1 * d - 13.f);
    S.gh.advance(S.dt_ms);
    const float cyb = db.cy(), cyt = dtp.cy();
    svg::Node* lower = c.n("hlinevertlower");
    svg::Node* upper = c.n("hlinevertupper");
    auto ends = [](svg::Node* g, float& a, float& b) {
      svg::Node* l = g && g->tag != "line" && !g->kids.empty() ? &g->kids[0] : g;
      a = attr_f(l, "y1"), b = attr_f(l, "y2");
    };
    float la = 0, lb = 0, ua = 0, ub = 0;
    ends(lower, la, lb);
    ends(upper, ua, ub);
    if (split) {
      const float s = (S.gh.cur + 10.f) / -d;
      set_line_of(lower, cyb + s * (la - cyb), cyb + s * (lb - cyb));
      const float ct = cyt + r6;
      set_line_of(upper, ct + s * (ua + r6 - ct), ct + s * (ub + r6 - ct));
    } else {
      set_line_of(lower, cyb + p1 * (la - cyb), cyb + p1 * (lb - cyb));
    }
    show(upper, split);
    move(c.n("greenh"), 0, S.gh.cur);
  }
  if (turned[1]) S.fade_g.init = false, S.fade_g.set(1.5f);
  S.fade_g.advance(S.dt_ms);
  S.fade_g.set(0);
  S.col_g = mix_rgb(kPalOffGreen, kPalGreen, std::min(S.fade_g.cur, 1.f));
  for (const char* id : {"greenh", "greendottop", "greendotbottom", "hlinevertupper", "hlinevertlower"})
    tint(c.n(id), S.col_g);

  // ---- 0x01A0EBBC: pulse 1 height, pulse 2 offset
  const svg::Box wb1 = c.box("whitehbar1"), span = c.box("hbarspanx");
  scale(c.n("whitehbars"), 1.f, p1, 0.f, wb1.y1);
  const float off = p2 * span.w();
  move(c.n("whitetbars"), off, 0);
  move(c.n("righttdot"), off, 0);
  show(c.n("hbarspanx"), false);  // removed by the firmware (only a measure)

  // ---- 0x01A10B3C: the T (distance between the pulses)
  if (svg::Node* tl = c.n("tline")) {
    const float x1 = attr_f(tl, "x1"), x2 = attr_f(tl, "x2"), y = attr_f(tl, "y1");
    set_line(tl, x1, y, x2 + off, y);  // scaled about its left end by (w + off) / w
  }
  float xt = 39.f * p2 - 5.f, yt = 0.f;
  if (p1 > 0.4f) {
    if (p2 < 0.27573f) yt = -16.f;
    xt = std::max(xt, 10.f);
  }
  S.tx.set(xt);
  S.ty.set(yt);
  S.tx.advance(S.dt_ms);
  S.ty.advance(S.dt_ms);
  move(c.n("t_letter"), S.tx.cur, S.ty.cur);
  if (turned[2]) S.fade_w.init = false, S.fade_w.set(1.5f);
  S.fade_w.advance(S.dt_ms);
  S.fade_w.set(0);
  S.col_w = mix_rgb(kPalGrey, kPalWhite, std::min(S.fade_w.cur, 1.f));
  tint(c.n("white"), S.col_w);
  tint(c.n("tline"), S.col_w);

  // ---- 0x01A0F1F8: pinkbars (redrawn by code in the node's style)
  const float m = 2.f * p3 - 1.f;
  const svg::Box pb = c.box("pinkbars");
  if (!pb.empty()) {
    svg::Points pts;
    const float hp = pb.y1 - pb.y0;
    for (int i = 0; i < 4; ++i) {
      const float xa = pb.x0 + pb.w() * float(i) / 4.f, xb = pb.x0 + pb.w() * float(i + 1) / 4.f;
      const float y = pb.y1 - (1.f + m * tsin(float(i + 1) + S.ph)) * hp * 0.5f;
      pts.emplace_back(xa, y);
      pts.emplace_back(xb, y);
    }
    set_points(c.n("pinkbars"), pts);
  }
  while (S.acc >= kFwFrame) {  // phase step once per firmware frame
    S.ph += m < 0 ? m * 0.3f : m * 2.f;
    S.acc -= kFwFrame;
  }

  // ---- 0x01A0F830: bluecurve (redrawn by code in the node's style)
  const svg::Box bc = c.box("bluecurve");
  if (!bc.empty()) {
    const float k = 1.f - p0, W = bc.w(), H = bc.y1 - bc.y0;
    S.act.set(sounding(iv) ? 0.5f + p0 / 2.f : 0.f);
    S.act.advance(S.dt_ms);
    const float e = S.act.cur;
    float xa = std::max(p2 * W / 2.f, 12.f) * (1.f + 2.f * k);
    if (p3 > 0.5f) xa += 20.f * (p3 - 0.5f) * tsin(0.2f * S.ph);
    xa = std::min(xa, W / 2.f);
    const float wob1 = 0.8f + 0.2f * tsin(S.counter);
    const float x1 = bc.x0 + xa + 10.f * wob1;
    svg::Points pts{{bc.x0, bc.y1}};
    const float h1 = p1 * H * e * wob1;
    float px = pts.back().first;
    edge(pts, (x1 + px) / 2.f, bc.y1 - h1, k);
    edge(pts, x1, bc.y1, k);
    const float wob2 = 0.8f + 0.2f * tsin(S.counter + kPi);
    S.counter += S.dt_ms / 1024.f;
    if (x1 < bc.cx()) pts.emplace_back(bc.cx(), bc.y1);
    const float x2 = bc.cx() + xa, h2 = H * wob2 * e;
    px = pts.back().first;
    edge(pts, (x2 + px) / 2.f, bc.y1 - h2, k);
    edge(pts, x2, bc.y1, k);
    pts.emplace_back(bc.x1, bc.y1);
    set_path(c.n("bluecurve"), pts);
  }
}

void draw_pls(BindCtx& c, svg::Image& img) {
  const float p3 = knob(c.inst(), 3), p2 = knob(c.inst(), 2);
  // 0x01A10720: the modulation line from the centre of redlinebg
  const svg::Box rl = c.box("redlinebg");
  if (!rl.empty()) {
    const float len = (p3 - 0.5f) * rl.w();
    if (std::fabs(len) > 0.01f) svg::stroke(img, {{rl.cx(), rl.cy()}, {rl.cx() + len, rl.cy()}}, 1.5f, kPalRed);
  }
  // 0x01A0ED2C: the white base line between the pulses
  const svg::Box wb1 = c.box("whitehbar1"), wb2 = c.box("whitehbar2"), t1 = c.box("tbar1"), t2 = c.box("tbar2"),
                 st = c.box("static"), span = c.box("hbarspanx");
  if (wb1.empty() || wb2.empty() || t1.empty() || t2.empty() || st.empty() || span.empty()) return;
  const float off = p2 * span.w(), y = span.y1;
  const float seg[4][2] = {{wb1.x1, t1.x0 + off}, {t1.x1 + off, wb2.x0}, {wb2.x1, t2.x0 + off}, {t2.x1 + off, st.x1}};
  for (const auto& s : seg)
    if (s[1] > s[0]) svg::stroke(img, {{s[0], y}, {s[1], y}}, 1.5f, kPalWhite);
}

OP1_SCREEN("pls", bind_pls, draw_pls);

}  // namespace
}  // namespace op1
