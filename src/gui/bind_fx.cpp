// Effect screens and the mixer's EQ / master pages (owner: t10 fx group). The firmware's effect classes
// (Dssd.cpp, Grid.cpp, Mllp.cpp, Ptch.cpp, Rymd.cpp, Mmmf.cpp, Ftwo.cpp, BodeAudioEngine.cpp) each ask for
// the element names that sit next to their "<name>.svg" string in the binary; the screens are shared by
// the instrument effect (synth/drum T3) and the master effect (mixer T3). Common to all eight:
//   - the colour groups "white", "green", "red" hold two digit slots each (the "%02i" next to every effect
//     name): the parameter of that encoder as 0..99;
//   - "blue" + "bluebase" is a dial needle turned by the blue encoder (pointing left = 0, right = max);
//   - effect off (shift+T3) dims the whole screen and stops its animation (the firmware's fx_bypass.cpp
//     "unknown fx mode" asserts show a bypass state exists; its look is ASSUMED: dimmed).
// The parameters shown are InstrumentView::fx[0..3] (or the master effect's, see master_params()), the
// animation reads fx_display[8..] (src/dsp/fx_*.cpp header comments) and the output level.
// Evidence and element tables: scratchpad bindings/fx.md.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "bind.h"

namespace op1 {
namespace {

// ------------------------------------------------------------------ shared helpers

// What the effect screen shows: the instrument's effect, or the master effect in mixer mode.
struct FxState {
  Param p[kParams];
  int32_t d[16];
  bool on;
  bool master;
};

FxState fx_state(const BindCtx& c) {
  FxState f{};
  if (c.ui.mode == Mode::Mixer) {
    // t5 (2026-10-23): the master effect's parameters and fx_display come from the snapshot (MixerView), so a
    // type switch (which now loads that type's defaults) and the effect's own animation values show.
    for (int i = 0; i < kParams; ++i) f.p[i] = c.s.mixer.master_fx_params[i];
    for (int i = 0; i < 16; ++i) f.d[i] = c.s.mixer.master_fx_display[i];
    f.on = c.s.mixer.master_fx_on;
    f.master = true;
  } else {
    const InstrumentView& iv = c.inst();
    for (int i = 0; i < kParams; ++i) f.p[i] = iv.fx[i];
    for (int i = 0; i < 16; ++i) f.d[i] = iv.fx_display[i];
    f.on = iv.fx_on;
    f.master = false;
  }
  return f;
}

float p01(const FxState& f, int i) { return q15(f.p[i & 7]); }
int to99(int v) { return std::clamp(int(std::lround(std::abs(float(v)) * 99.f / 32767.f)), 0, 99); }

// Writes a two-digit value into a colour group's digit slots.
void num(BindCtx& c, const char* group, int value) {
  svg::Node* g = c.n(group);
  if (!g) return;
  char b[8];
  std::snprintf(b, sizeof b, "%02d", std::clamp(value, 0, 99));
  text_into(c, slots_lr(c, g), b);
}

uint32_t mix_rgb(uint32_t a, uint32_t b, float t);

// Some digit groups are authored with a narrow "1" (bode "10", master "1300") or with a glyph made of a
// sub-group (master drive "76"): Font::set aligns a new glyph at the authored glyph's left edge (crowding
// the digits) or cannot fill a group slot. Those are hidden and drawn here instead,
// right-aligned in the group's box at its height.
void draw_num(BindCtx& c, svg::Image& img, const char* group, const std::string& text, uint32_t rgb, float opacity) {
  if (!c.font.ok()) return;
  svg::Node* g = c.n(group);
  if (!g) return;
  svg::Box b = c.doc.bounds(g, false);
  if (b.empty()) return;
  b.x0 += g->dx; b.x1 += g->dx; b.y0 += g->dy; b.y1 += g->dy;
  const float h = b.h(), adv = 0.5f * h + 1.5f;
  const float x = b.x1 - float(text.size() - 1) * adv - 0.45f * h;  // last glyph ends at the box's right
  c.font.draw(img, x, b.y0, h, text, opacity < 1 ? mix_rgb(0, rgb, opacity) : rgb, 1.5f);
}

// The dial needle: authored pointing left from its base; turns clockwise up to pointing right.
void dial(BindCtx& c, const char* needle, const char* base, float v01) {
  svg::Node* n = c.n(needle);
  if (!n) return;
  const svg::Box b = c.box(base);
  const svg::Box nb = c.box(needle);
  const float cx = b.empty() ? nb.x1 : b.cx(), cy = b.empty() ? nb.cy() : b.cy();
  rotate(c, n, 180.f * clamp01(v01), cx, cy);
}

// tint() sets the colour of one node; children with their own stroke/fill keep theirs, so tint a subtree.
void tint_tree(svg::Node* n, uint32_t rgb) {
  if (!n) return;
  tint(n, rgb);
  for (svg::Node& k : n->kids) tint_tree(&k, rgb);
}

// The bypass look (ASSUMED): the whole screen at reduced brightness. Opacity is inherited by the renderer.
void bypass(BindCtx& c, bool on) {
  if (on) return;
  if (svg::Node* r = c.n("Layer_1")) r->alpha = 0.38f;
}

// Output level 0..1 of what the effect processes: the instrument's peak (display[14], Q15) or the master
// meters, with a short hold so the animation follows notes rather than single blocks.
struct Smooth {
  float v = 0;
  double t = -1;
  float step(float target, double now, float tau) {
    const double dt = t < 0 ? 0 : std::clamp(now - t, 0.0, 0.5);
    t = now;
    const float k = std::exp(-float(dt) / tau);
    v = std::max(target, v * k);
    return v;
  }
};

float raw_level(const BindCtx& c) {
  // x 2.5: a single note peaks around 0.15 .. 0.5 of full scale; the animations should read it clearly
  if (c.ui.mode == Mode::Mixer) return clamp01(2.5f * float(std::max(c.s.mixer.meter_l, c.s.mixer.meter_r)) / 32767.f);
  return clamp01(2.5f * float(c.inst().display[14]) / 32767.f);
}

float level(const BindCtx& c, Smooth& s, float tau = 0.18f) { return s.step(raw_level(c), c.t, tau); }

uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
  return x;
}
float rnd01(uint32_t a, uint32_t b) { return float(hash32(a * 0x9E3779B9u ^ hash32(b)) & 0xFFFF) / 65535.f; }

// Shows the first `lit` of a numbered set ("g0".."g9"), hides the rest.
void bars(BindCtx& c, const char* prefix, int count, int lit) {
  char id[16];
  for (int i = 0; i < count; ++i) {
    std::snprintf(id, sizeof id, "%s%d", prefix, i);
    show(c.n(id), i < lit);
  }
}

uint32_t mix_rgb(uint32_t a, uint32_t b, float t) {
  t = clamp01(t);
  auto ch = [&](int s) { return uint32_t(lerp(float((a >> s) & 255), float((b >> s) & 255), t) + 0.5f) << s; };
  return ch(16) | ch(8) | ch(0);
}

// ------------------------------------------------------------------ delay (Dssd.cpp, delay.svg)
// Strings next to delay.svg: green white input vert cdot rbgnd ring%i off%i %02i.
// A square "R" (range) box with five rings; a crosshair (vert, hor) with a red dot (cdot) and a red line
// (rbgnd) from the left; the red level number and the "INPUT" label (both authored above the screen,
// y < 0, on top of each other) ride on the crosshair's height, the number replacing the label while the
// orange encoder turns. ASSUMED reading: crosshair x = the delay time inside the range
// (green, speed), y = the level (orange); rings lit = feedback (white), a highlight runs outward once per
// echo while sound plays.
constexpr float kDlX0 = 89.442f, kDlX1 = 228.348f, kDlY0 = 5.027f, kDlY1 = 143.924f;

void bind_delay(BindCtx& c) {
  static Smooth sm;
  const FxState f = fx_state(c);
  const float lvl = level(c, sm, 0.4f);
  const float speed = clamp01(0.5f + float(f.p[1]) / 65534.f);  // bipolar slot: centre = 0
  const float fb = p01(f, 2), out = p01(f, 3);
  const float x = lerp(kDlX0 + 6, kDlX1 - 6, speed), y = lerp(kDlY1 - 6, kDlY0 + 6, out);
  move(c.n("vert"), 0, y);
  move(c.n("hor"), x, 0);
  move(c.n("cdot"), x, y);
  move(c.n("rbgnd"), x, y);
  move(c.n("red"), 0, y);
  move(c.n("input"), 0, y);
  // the level number and the "INPUT" label share their place: the number while orange turns (ASSUMED)
  const bool lvl_pop = since_enc(c, 3) < 1.5;
  show(c.n("red"), lvl_pop);
  if (lvl_pop) tint_tree(c.n("red"), kColRed);  // authored dark (#4E2832): lit while it is the value shown
  show(c.n("input"), !lvl_pop);
  num(c, "white", to99(f.p[2]));
  num(c, "green", to99(f.p[1]));
  num(c, "red", to99(f.p[3]));
  // rings: feedback lights 0..5 of them (the dim "off" copies stay underneath)
  const int lit = int(std::lround(fb * 5.f));
  const float ms = f.d[8] > 0 ? float(f.d[8]) : lerp(50.f, 2000.f, p01(f, 0));
  const int run = f.on && lvl > 0.02f ? int(std::fmod(c.t * 1000.0 / std::max(40.f, ms), 5.0)) : -1;
  char id[8];
  for (int i = 0; i < 5; ++i) {
    std::snprintf(id, sizeof id, "ring%d", i);
    svg::Node* r = c.n(id);
    show(r, i < lit || i == run);
    if (i == run) tint(r, mix_rgb(kColBlue, kColWhite, std::min(1.f, lvl * 3.f)));
  }
  bypass(c, f.on);
}

// ------------------------------------------------------------------ grid (Grid.cpp, grid.svg)
// Strings next to grid.svg: blue green white wtop wbase grid %02i (and the "x", "y" glyph groups).
// The isometric floor "grid" stretches along its two axes with x size (blue) and y size (green); the small
// diamond wtop rises above wbase with z feedback (white); m0..m20 is the mix meter (orange). The digits
// of blue/green replace their "x"/"y" letters while the encoder turns (ASSUMED). While sound plays an echo
// front runs over the floor (code-drawn), at the rate of the line lengths.
constexpr float kGrT[2] = {120.58f, 47.05f}, kGrR[2] = {218.66f, 95.24f}, kGrL[2] = {23.08f, 95.81f};
constexpr float kGrC[2] = {120.8f, 95.5f};

void grid_axes(const FxState& f, float& su, float& sv) {
  su = 0.45f + 0.55f * p01(f, 0);
  sv = 0.45f + 0.55f * p01(f, 1);
}

void bind_grid(BindCtx& c) {
  const FxState f = fx_state(c);
  float su, sv;
  grid_axes(f, su, sv);
  // scale along u = R - T and v = L - T, about the floor's centre
  const float ux = kGrR[0] - kGrT[0], uy = kGrR[1] - kGrT[1], vx = kGrL[0] - kGrT[0], vy = kGrL[1] - kGrT[1];
  const float det = ux * vy - vx * uy;
  const float i00 = vy / det, i01 = -vx / det, i10 = -uy / det, i11 = ux / det;  // B^-1
  // M = B diag(su, sv) B^-1
  const float a = ux * su * i00 + vx * sv * i10, cc = ux * su * i01 + vx * sv * i11;
  const float b = uy * su * i00 + vy * sv * i10, d = uy * su * i01 + vy * sv * i11;
  if (svg::Node* g = c.n("grid")) {
    svg::Affine m{a, b, cc, d, kGrC[0] - (a * kGrC[0] + cc * kGrC[1]), kGrC[1] - (b * kGrC[0] + d * kGrC[1])};
    g->xf = g->xf.then(m);
  }
  const bool xpop = since_enc(c, 0) < 1.5, ypop = since_enc(c, 1) < 1.5;
  show(c.n("x"), !xpop);
  show(c.n("blue"), xpop);
  show(c.n("y"), !ypop);
  show(c.n("green"), ypop);
  num(c, "blue", to99(f.p[0]));
  num(c, "green", to99(f.p[1]));
  num(c, "white", to99(f.p[2]));
  move(c.n("wtop"), 0, -p01(f, 2) * 28.f);
  bars(c, "m", 21, int(std::lround(p01(f, 3) * 21.f)));
  bypass(c, f.on);
}

void draw_grid(BindCtx& c, svg::Image& img) {
  static Smooth sm;
  const FxState f = fx_state(c);
  const float lvl = level(c, sm, 0.5f);
  // the z column: vertical edges between the base and the raised top diamond
  const svg::Box wb = c.box("wbase");
  const float h = p01(f, 2) * 28.f;
  const float dim = f.on ? 1.f : 0.38f;
  if (!wb.empty() && h > 0.5f) {
    for (float xx : {wb.x0, wb.cx(), wb.x1}) {
      const float yy = xx == wb.cx() ? wb.y1 : wb.cy();
      svg::stroke(img, {{xx, yy}, {xx, yy - h}}, 1.2f, 0xDFD9FF, false, dim);
    }
  }
  if (!f.on || lvl < 0.02f) return;
  // echo front: a diamond growing from the centre, period = x size line length
  float su, sv;
  grid_axes(f, su, sv);
  const float ms = f.d[8] > 0 ? float(f.d[8]) / 10.f : 30.f;
  const float period = std::clamp(ms * 0.025f, 0.12f, 1.2f);
  for (int k = 0; k < 2; ++k) {
    const float r = float(std::fmod(c.t / period + k * 0.5, 1.0));
    const float hu = 0.5f * su * r, hv = 0.5f * sv * r;
    const float ux = kGrR[0] - kGrT[0], uy = kGrR[1] - kGrT[1], vx = kGrL[0] - kGrT[0], vy = kGrL[1] - kGrT[1];
    svg::Points pts = {{kGrC[0] - hu * ux - hv * vx, kGrC[1] - hu * uy - hv * vy},
                       {kGrC[0] + hu * ux - hv * vx, kGrC[1] + hu * uy - hv * vy},
                       {kGrC[0] + hu * ux + hv * vx, kGrC[1] + hu * uy + hv * vy},
                       {kGrC[0] - hu * ux + hv * vx, kGrC[1] - hu * uy + hv * vy}};
    svg::stroke(img, pts, 1.2f, kColBlue, true, clamp01(lvl * 2.f) * (1.f - r) * (0.3f + 0.7f * p01(f, 2)));
  }
}

// ------------------------------------------------------------------ punch (Mllp.cpp, mllp.svg)
// Strings next to mllp.svg: blue green white lglove rglove head lbody ubody bluebase %02i (plus the
// p0..p3 "pow" rays and the g0..g9 bars of the template). The boxer: the right glove (red, power) jabs
// out on every transient, the head and upper body (green, punch) take the hit back as gain reduction,
// the rays flash on hard hits; g0..g9 meter the transient (ASSUMED readings of the firmware's animation).
void bind_mllp(BindCtx& c) {
  static Smooth hit_s, gr_s;
  const FxState f = fx_state(c);
  dial(c, "blue", "bluebase", p01(f, 0));
  num(c, "green", to99(f.p[1]));
  num(c, "white", to99(f.p[2]));
  num(c, "red", to99(f.p[3]));
  float hit = 0, gr = 0;
  if (f.on) {
    {  // t5: the master effect publishes its display too (MixerView), so both positions read the detector
      // the transient detector reads ~0.05 of full scale on a plain note: scale it up; the level bobs too
      hit = hit_s.step(std::max(clamp01(float(f.d[10]) / 3000.f), 0.5f * raw_level(c)), c.t, 0.12f);
      gr = gr_s.step(std::max(clamp01(3.f * (1.f - float(f.d[9]) / 32767.f)), 0.4f * raw_level(c)), c.t, 0.25f);
    }
  }
  if (svg::Node* g = c.n("rglove")) {
    const svg::Box b = c.box("rglove");
    move(g, 9.f * hit, -5.f * hit);
    if (!b.empty()) scale(g, 1.f + 0.25f * hit, 1.f + 0.25f * hit, b.cx(), b.cy());
  }
  const svg::Box hb = c.box("head");
  rotate(c, c.n("head"), -10.f * gr, hb.cx(), hb.y1);
  move(c.n("head"), -3.f * gr, 0);
  const svg::Box ub = c.box("ubody");
  rotate(c, c.n("ubody"), -3.f * gr, ub.cx(), ub.y1);
  move(c.n("lglove"), 0, -4.f * gr);
  const bool pow = hit > 0.35f;
  for (const char* id : {"p0", "p1", "p2", "p3"}) {
    show(c.n(id), pow);
    tint(c.n(id), kColWhite);
  }
  bars(c, "g", 10, int(std::lround(hit * 10.f)));
  bypass(c, f.on);
}

// ------------------------------------------------------------------ phone (Ptch.cpp, ptch.svg)
// Strings next to ptch.svg: blue green white bluebase chip speaker noise %02i (plus "pot", "mic",
// n0..n9 in the template). Rate dial (blue), grain length (green), repeats (white), dry/wet (orange, the
// speaker): fx_phone.cpp. Animation (ASSUMED except the chip's buffer rotation): the speaker pumps with the level.
void bind_ptch(BindCtx& c) {
  static Smooth sm;
  const FxState f = fx_state(c);
  const float lvl = f.on ? level(c, sm, 0.15f) : 0.f;
  dial(c, "blue", "bluebase", p01(f, 0));
  num(c, "green", to99(f.p[1]));
  num(c, "white", to99(f.p[2]));
  num(c, "red", to99(f.p[3]));
  if (svg::Node* sp = c.n("speaker")) {
    const svg::Box b = c.box("speaker");
    const float k = 1.f + 0.18f * lvl;
    if (!b.empty()) scale(sp, k, k, b.cx(), b.cy());
  }
  // t5: the firmware's phone is a grain repeater (fx_phone.cpp): the chip follows the rotation of its three
  // grain buffers (display[11]), the noise dots show the repeats of the playing grain (display[12] of [10]) while
  // sound plays; nothing moves in silence (the firmware has no noise source).
  const uint32_t tick = uint32_t(c.t * 18.0);
  const float reps = f.d[10] > 0 ? clamp01(float(f.d[12] + 1) / float(f.d[10])) : 0.f;
  const float crackle = f.on && lvl > 0.02f ? clamp01(0.2f + 0.8f * reps) * clamp01(lvl * 4.f) : 0.f;
  char id[8];
  for (int i = 0; i < 10; ++i) {
    std::snprintf(id, sizeof id, "n%d", i);
    show(c.n(id), rnd01(uint32_t(i), tick) < crackle);
  }
  if (svg::Node* ch = c.n("chip")) ch->alpha = f.on && lvl > 0.02f ? (f.d[11] % 2 ? 0.55f : 1.f) : 1.f;
  // mic shakes with gsm damage while sound plays
  if (f.on && lvl > 0.02f) move(c.n("mic"), (rnd01(5u, tick) - 0.5f) * 2.f * p01(f, 1), 0);
  bypass(c, f.on);
}

// ------------------------------------------------------------------ spring (Rymd.cpp, rymd.svg)
// Strings next to rymd.svg: down (and "up" in the template): two red dots with black masks over a dashed
// red line, the mix (orange): the dots close in on the centre as the mix goes down (inferred from the
// masks). The spring itself has no element: it is drawn by code (ASSUMED shape: a horizontal coil with
// turns (green) loops, tone (blue) brightness and wave speed, swinging with the output level while it
// rings, longer swing with more damping (white)).
void bind_rymd(BindCtx& c) {
  const FxState f = fx_state(c);
  const float gap = (1.f - p01(f, 3)) * 28.f;
  move(c.n("up"), 0, gap);
  move(c.n("down"), 0, -gap);
  bypass(c, f.on);
}

void draw_rymd(BindCtx& c, svg::Image& img) {
  static Smooth sm;
  const FxState f = fx_state(c);
  const float lvl = f.on ? level(c, sm, 0.6f) : 0.f;
  const int turns = 6 + int(std::lround(p01(f, 1) * 18.f));
  const float x0 = 52.f, x1 = 262.f, cy = 80.f, ry = 15.f, rx = 4.f;
  const float damp = p01(f, 2);
  const int n = turns * 24;
  svg::Points pts;
  pts.reserve(size_t(n) + 1);
  const float swing = f.on ? (6.f + 10.f * damp) : 0.f;
  for (int i = 0; i <= n; ++i) {
    const float u = float(i) / float(n);
    const float ph = 2.f * 3.14159265f * float(turns) * u;
    // displacement along the coil from the output samples (a standing wave between the fixed ends)
    const float env = std::sin(3.14159265f * u);
    const float wave = std::sin(2.f * 3.14159265f * (2.5f * u - float(c.t) * (1.5f + 3.f * p01(f, 0))));
    const float dy = swing * lvl * env * wave;
    pts.emplace_back(x0 + (x1 - x0) * u + rx * std::cos(ph), cy + ry * std::sin(ph) + dy);
  }
  const uint32_t col = mix_rgb(0x34477D, kColBlue, 0.4f + 0.6f * p01(f, 0));
  svg::stroke(img, pts, 1.5f, col, false, f.on ? 1.f : 0.38f);
  // the two anchors
  svg::stroke(img, {{x0 - 14, cy - ry - 4}, {x0 - 14, cy + ry + 4}}, 1.5f, kColGrey, false, f.on ? 1.f : 0.38f);
  svg::stroke(img, {{x1 + 14, cy - ry - 4}, {x1 + 14, cy + ry + 4}}, 1.5f, kColGrey, false, f.on ? 1.f : 0.38f);
}

// ------------------------------------------------------------------ filter (Mmmf.cpp, mmmf.svg; hidden)
// Strings next to mmmf.svg: blue green white bluebase %02i, then the type names "lp iv" "lp iii" "lp ii"
// "lp i" "ap iii" "notch ii" "bp ii" "hp i" "hp ii" "hp iii" "hp iv". Frequency dial (blue), resonance
// bars g0..g9 and number (green), the type (white: drawn as its name in the white slot area), drive bars
// r0..r9 and number (orange). Our filter has five types (fx_filter.cpp); the names picked from the
// firmware's list are ASSUMED. The bars bounce with the level while sound passes.
// t5: the firmware's 12 type names (0x018E4958), in record order (fx_filter.cpp)
const char* const kFilterNames[12] = {"lp iv", "lp iii", "lp ii", "lp i", "ap", "ap iii",
                                      "notch ii", "bp ii", "hp i", "hp ii", "hp iii", "hp iv"};

int filter_type(const FxState& f) {
  if (f.d[10] >= 0 && f.d[10] <= 11 && f.d[8] > 0) return f.d[10];
  return std::clamp(int((std::min(int(f.p[2]), 22527) * 11 + 11264) / 22528), 0, 11);
}

void bind_mmmf(BindCtx& c) {
  static Smooth sm;
  const FxState f = fx_state(c);
  const float lvl = f.on ? level(c, sm, 0.2f) : 0.f;
  dial(c, "blue", "bluebase", p01(f, 0));
  num(c, "green", to99(f.p[1]));
  num(c, "red", to99(f.p[3]));
  show(c.n("white"), false);
  const int bounce = int(std::lround(lvl * 3.f));
  bars(c, "g", 10, std::min(10, int(std::lround(p01(f, 1) * 10.f)) + bounce));
  bars(c, "r", 10, std::min(10, int(std::lround(p01(f, 3) * 10.f)) + bounce));
  bypass(c, f.on);
}

void draw_mmmf(BindCtx& c, svg::Image& img) {
  const FxState f = fx_state(c);
  const std::string name = kFilterNames[filter_type(f)];
  const float h = 12.f;
  if (!c.font.ok()) return;
  const float w = float(name.size()) * (0.5f * h + 1.5f);  // Font::draw's advance per character
  c.font.draw(img, 297.f - w, 47.f, h, name, f.on ? 0xDFD9FFu : 0x55525Fu, 1.5f);
}

// ------------------------------------------------------------------ nitro (Ftwo.cpp, ftwo.svg)
// Strings next to ftwo.svg: blue ffmeter town r%02ix%02i rr%02i rb%02i ff%i ffbase bluebase redbase.
// A road to the town: rr rows (dim) and rb rows (bright), ten segments each across the width. ASSUMED
// reading: the width is the spectrum (20 Hz left .. 20 kHz right), the bright segments are the pass band
// between the high-pass (blue dial) and the low-pass (orange dial) cutoffs, and the rows are history
// (nearest = now) scrolling toward the town at a speed set by the follower; ffmeter is the follower needle
// on ffbase; ff0..ff7 light up with the feedback (white).
constexpr int kNitroRows = 16;
struct NitroHist {
  uint16_t band[kNitroRows] = {};  // bit per segment
  double last = -1;
};

uint16_t nitro_band(const FxState& f) {
  float lp = float(f.d[8]), hp = float(f.d[9]);
  if (lp <= 0) {
    hp = 20.f * std::pow(300.f, p01(f, 0));
    lp = 60.f * std::pow(266.f, p01(f, 3));
  }
  auto pos = [](float hz) { return std::log2(std::max(hz, 20.f) / 20.f) / std::log2(1000.f) * 10.f; };
  const float a = pos(hp), b = pos(lp);
  uint16_t m = 0;
  for (int k = 0; k < 10; ++k)
    if (float(k) + 1.f > a && float(k) < b) m = uint16_t(m | (1u << k));
  return m;
}

void bind_ftwo(BindCtx& c) {
  static NitroHist h;
  static Smooth fol_s;
  const FxState f = fx_state(c);
  dial(c, "blue", "bluebase", p01(f, 0));
  dial(c, "red", "redbase", p01(f, 3));
  const float fol = fol_s.step(f.on ? clamp01(float(f.d[10]) / 32767.f) : 0.f, c.t, 0.1f);
  // history: a new row every 30..120 ms, faster with the follower
  const uint16_t now = f.on ? nitro_band(f) : 0;
  const double period = 0.12 - 0.09 * double(fol);
  if (h.last < 0 || c.t - h.last > 1.0) {
    for (auto& r : h.band) r = now;
    h.last = c.t;
  }
  while (c.t - h.last >= period) {
    for (int r = kNitroRows - 1; r > 0; --r) h.band[r] = h.band[r - 1];
    h.last += period;
  }
  h.band[0] = now;
  char id[16];
  for (int r = 0; r < kNitroRows; ++r) {
    for (int k = 0; k < 10; ++k) {
      std::snprintf(id, sizeof id, "r%02dx%02d", r * 2, k);
      show(c.n(id), (h.band[r] >> k) & 1);
    }
  }
  const svg::Box fb = c.box("ffbase");
  if (svg::Node* m = c.n("ffmeter")) rotate(c, m, -60.f + 120.f * fol, fb.empty() ? NAN : fb.cx(), fb.empty() ? NAN : fb.cy());
  const int gear = int(std::lround(p01(f, 2) * 8.f));
  for (int i = 0; i < 8; ++i) {
    std::snprintf(id, sizeof id, "ff%d", i);
    show(c.n(id), i < gear);
  }
  bypass(c, f.on);
}

// ------------------------------------------------------------------ cwo (BodeAudioEngine.cpp, bode.svg)
// Strings next to bode.svg: blue bluecenter green white ggroup rr%i xx%i %02i (plus b0..b6, m0..m13, r0..r9
// in the template). The cow: shift dial (blue), delay number (green), feedback number (white), sideband
// meter r0..r9 (orange). ASSUMED animation: the blue sound arcs b0..b6 travel at a rate set by the shift
// while sound plays; the green ticks xx0..xx12 are the echoes along the ground line, spaced by the delay
// time and fading with the feedback; the stomach dashes m0..m13 and the red stripes rr0..rr5 churn with
// the level.
void bind_bode(BindCtx& c) {
  static Smooth sm;
  const FxState f = fx_state(c);
  const float lvl = f.on ? level(c, sm, 0.5f) : 0.f;
  dial(c, "blue", "bluecenter", p01(f, 0));
  show(c.n("green"), false);  // drawn by draw_bode (authored "10")
  num(c, "white", to99(f.p[2]));
  bars(c, "r", 10, int(std::lround(clamp01(float(f.p[3]) / 32767.f) * 10.f)));
  // sound arcs
  const float shift01 = p01(f, 0);
  const int head = lvl > 0.02f ? int(std::fmod(c.t * (2.0 + 14.0 * shift01), 7.0)) : -1;
  char id[8];
  for (int i = 0; i < 7; ++i) {
    std::snprintf(id, sizeof id, "b%d", i);
    show(c.n(id), head < 0 || (i + 7 - head) % 7 < 3);
  }
  // echoes on the ground
  const float fb = p01(f, 2);
  const float ms = f.d[9] > 0 ? float(f.d[9]) : lerp(5.f, 1000.f, p01(f, 1) * p01(f, 1));
  const float spacing = 6.f + 32.f * std::sqrt(clamp01(ms / 1000.f));
  const float off = lvl > 0.02f ? float(std::fmod(c.t * 1000.0 / std::max(ms, 5.f), 1.0)) * spacing : 0.f;
  for (int k = 0; k < 13; ++k) {
    std::snprintf(id, sizeof id, "xx%d", k);
    svg::Node* n = c.n(id);
    const float x = float(k) * spacing + off;
    const bool vis = x < 240.f && (k == 0 || std::pow(std::max(fb, 0.01f), float(k)) > 0.04f);
    show(n, vis);
    move(n, x, 0);
  }
  const uint32_t tick = uint32_t(c.t * 12.0);
  for (int i = 0; i < 14; ++i) {
    std::snprintf(id, sizeof id, "m%d", i);
    show(c.n(id), lvl < 0.02f || rnd01(uint32_t(100 + i), tick) < 0.35f + lvl);
  }
  for (int i = 0; i < 6; ++i) {
    std::snprintf(id, sizeof id, "rr%d", i);
    show(c.n(id), lvl < 0.02f || (i + int(c.t * 10.0)) % 3 != 0);
  }
  bypass(c, f.on);
}

void draw_bode(BindCtx& c, svg::Image& img) {
  const FxState f = fx_state(c);
  char b[8];
  std::snprintf(b, sizeof b, "%02d", to99(f.p[1]));
  draw_num(c, img, "green", b, kColGreen, f.on ? 1.f : 0.38f);
}

// ------------------------------------------------------------------ eq (mixer T2, eq.svg)
// Strings next to eq.svg: lowtop midtop lowbot midbot highbot clean. Three solids, low (blue ellipse),
// mid (green diamond), high (white diamond), each with a fixed lower half and a top that rises with boost
// and flattens with cut (ASSUMED reading of the top/bot split). "clean" (the arrow to the CLEAN box) shows
// while all three are flat, "max" (MAX EQ) while any band is at its end (ASSUMED).
void bind_eq(BindCtx& c) {
  const MixerView& m = c.s.mixer;
  auto g = [&](int i) { return std::clamp((float(m.eq[i]) - 16383.f) / 16384.f, -1.f, 1.f); };
  const float gl = g(0), gm = g(1), gh = g(2);
  // low: the top arc of lowtop scales about the rim; lowbot duplicates the lower arc
  show(c.n("lowbot"), false);
  if (svg::Node* lt = c.n("lowtop"); lt && !lt->kids.empty()) scale(&lt->kids[0], 1.f, std::max(0.02f, 1.f + gl), 34.5f, 81.f);
  // mid: the full diamond, apex moved; midbot duplicates it
  show(c.n("midbot"), false);
  const float rim = 82.965f, depth = 9.283f;
  set_points(c.n("midtop"), {{87.4f, rim}, {104.112f, 91.868f}, {120.823f, rim}, {104.112f, rim - depth * (1.f + gm)}});
  // high: highbot's two upper edges (authored dim) move their apex
  if (svg::Node* hb = c.n("highbot"); hb && hb->kids.size() >= 3) {
    const float ay = rim - depth * (1.f + gh);
    set_line(&hb->kids[0], 171.609f, ay, 155.065f, rim);
    set_line(&hb->kids[1], 187.857f, rim, 171.609f, ay);
  }
  const bool flat = std::abs(gl) < 0.02f && std::abs(gm) < 0.02f && std::abs(gh) < 0.02f;
  show(c.n("clean"), flat);
  show(c.n("max"), std::max({std::abs(gl), std::abs(gm), std::abs(gh)}) > 0.97f);
}

// ------------------------------------------------------------------ master (mixer T4, master.svg)
// Strings next to master.svg: leftpeak rightpeak leftpeakhold rightpeakhold leftgr rightgr leftknob
// rightknob textleftknob textrightknob textdriveknob textreleaseknob (MasterController.cpp: subscreen up
// to kCompressor). A stereo compressor: L/R peak meters rising from the bottom with a peak-hold line,
// L/R gain-reduction lines falling from the top, L/R threshold sliders (blue, green) with their numbers,
// drive (white) and release (orange, 4 digits). MixerView::master[0..3] = 32767/32767/8192/10240 by
// default; the mapping master[0] = left, [1] = right, [2] = drive, [3] = release is ASSUMED from the
// colours, the release read-out as ms = value * 4160 / 32767 (the template's sample reads 1300 for the
// default 10240) is ASSUMED. The mixer stores but does not apply these (tape_engine.h), so the gain
// reduction shown is computed here from the meters and the thresholds (ASSUMED).
struct MasterMeters {
  Smooth pk[2];
  float hold[2] = {0, 0};
  double hold_t[2] = {-1e9, -1e9};
  Smooth gr[2];
};

constexpr float kMsTrackTop = 16.157f, kMsTrackBottom = 104.195f;
constexpr float kMsMeterBottom = 109.5f, kMsMeterTop = 10.8f;

void bind_master(BindCtx& c) {
  static MasterMeters mm;
  const MixerView& m = c.s.mixer;
  const float thr[2] = {q15(m.master[0]), q15(m.master[1])};
  const float drive = q15(m.master[2]);
  const char* knob[2] = {"leftknob", "rightknob"};
  const char* text[2] = {"textleftknob", "textrightknob"};
  const char* pk[2] = {"leftpeak", "rightpeak"};
  const char* ph[2] = {"leftpeakhold", "rightpeakhold"};
  const char* gr[2] = {"leftgr", "rightgr"};
  const int16_t met[2] = {m.meter_l, m.meter_r};
  const float span = kMsMeterBottom - kMsMeterTop;
  for (int s = 0; s < 2; ++s) {
    const float dy = -thr[s] * (kMsTrackBottom - kMsTrackTop);
    move(c.n(knob[s]), 0, dy);
    move(c.n(text[s]), 0, dy);
    num(c, text[s], to99(m.master[s]));
    const float raw = clamp01(float(met[s]) / 32767.f);
    const float v = mm.pk[s].step(raw, c.t, 0.12f);
    move(c.n(pk[s]), 0, -v * span);
    if (v >= mm.hold[s] || c.t - mm.hold_t[s] > 1.5) {
      mm.hold[s] = v;
      mm.hold_t[s] = c.t;
    }
    move(c.n(ph[s]), 0, -mm.hold[s] * span);
    show(c.n(ph[s]), mm.hold[s] > 0.01f);
    const float red = clamp01((raw - thr[s]) / std::max(0.05f, 1.f - thr[s])) * (0.3f + 0.7f * drive);
    move(c.n(gr[s]), 0, mm.gr[s].step(red, c.t, 0.3f) * span * 0.5f);
  }
  show(c.n("textdriveknob"), false);    // drawn by draw_master (a slot is a sub-group)
  show(c.n("textreleaseknob"), false);  // drawn by draw_master (authored "1300")
}

void draw_master(BindCtx& c, svg::Image& img) {
  char b[8];
  std::snprintf(b, sizeof b, "%d", std::clamp(int(std::lround(float(c.s.mixer.master[3]) * 4160.f / 32767.f)), 0, 9999));
  draw_num(c, img, "textreleaseknob", b, kColRed, 1.f);
  std::snprintf(b, sizeof b, "%d", to99(c.s.mixer.master[2]));
  draw_num(c, img, "textdriveknob", b, kColWhite, 1.f);
}

}  // namespace

OP1_SCREEN("delay", bind_delay, nullptr);
OP1_SCREEN("grid", bind_grid, draw_grid);
OP1_SCREEN("mllp", bind_mllp, nullptr);
OP1_SCREEN("ptch", bind_ptch, nullptr);
OP1_SCREEN("rymd", bind_rymd, draw_rymd);
OP1_SCREEN("mmmf", bind_mmmf, draw_mmmf);
OP1_SCREEN("ftwo", bind_ftwo, nullptr);
OP1_SCREEN("bode", bind_bode, draw_bode);
OP1_SCREEN("eq", bind_eq, nullptr);
OP1_SCREEN("master", bind_master, draw_master);

}  // namespace op1
