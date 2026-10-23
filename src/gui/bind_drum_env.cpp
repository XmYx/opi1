// Drum and envelope screens (task t10, group drum_env): drum2 (drum sampler), dbox (drum synth), adsr (synth
// envelope), dynaenv (drum envelope), playmode (synth play mode / portamento) and octave (the octave pop-up).
// Element names come from the firmware's strings next to each "<name>.svg" string; what is observed, inferred
// or ASSUMED is listed per element in the notes (bindings/drum_env.md). Notes:
//   - bind() sets overrides only; draw() paints the code-drawn parts (waves, voice markers) in display pixels.
//   - A group's tint does not reach children that carry their own colour, so colour changes here go through
//     stroke/fill attribute overrides on the leaves (paint_tree).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bind.h"

namespace op1 {
namespace {

// ------------------------------------------------------------------ shared helpers

std::string hex(uint32_t rgb) {
  char b[8];
  std::snprintf(b, sizeof b, "#%06X", unsigned(rgb & 0xFFFFFFu));
  return b;
}

// Recolours every leaf below n that draws a stroke (and, with fill, a fill that is not "none").
void paint_tree(svg::Node* n, uint32_t rgb, bool fill = false) {
  if (!n) return;
  const std::string* st = n->get("stroke");
  if (st && *st != "none") n->set("stroke", hex(rgb));
  if (fill) {
    const std::string* fl = n->get("fill");
    if (fl && *fl != "none") n->set("fill", hex(rgb));
  }
  for (svg::Node& k : n->kids) paint_tree(&k, rgb, fill);
}

uint32_t mix(uint32_t a, uint32_t b, float t) {
  t = clamp01(t);
  auto ch = [&](int s) {
    const float x = float((a >> s) & 0xFF), y = float((b >> s) & 0xFF);
    return uint32_t(std::lround(x + (y - x) * t)) << s;
  };
  return ch(16) | ch(8) | ch(0);
}

// Moves a dot group that is authored around (ox, oy) so that it sits at (x, y).
void place(svg::Node* n, float x, float y, float ox, float oy) { move(n, x - ox, y - oy); }

// The firmware's lower-case glyphs draw as capitals; the upper-case set is incomplete (no C, B, G).
// Returns letter + octave ("c4"); sharp tells whether a '#' goes with it (MIDI numbering, 60 = c4).
std::string note_name(int n, bool& sharp) {
  static const char nm[12] = {'c', 'c', 'd', 'd', 'e', 'f', 'f', 'g', 'g', 'a', 'a', 'b'};
  static const bool sh[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
  n = std::clamp(n, 0, 127);
  sharp = sh[n % 12];
  return std::string(1, nm[n % 12]) + std::to_string(n / 12 - 1);
}

// The "pitch" read-out of drum2 and dbox: kids 0/1 = the sign (vertical, horizontal), 2 = ones digit,
// 3 = tens digit (the format next to drum2.svg is "%+.0f"). The authored digit slots ("10": a narrow '1' and
// a '0') are too close for two wide digits, so bind() keeps the sign and draw() writes the number after it.
void pitch_sign(svg::Node* g, int semis) {
  if (!g || g->kids.size() < 4) return;
  show(&g->kids[0], semis >= 0);  // '+' needs the upright, '-' only the bar
  show(&g->kids[1], true);
  show(&g->kids[2], false);
  show(&g->kids[3], false);
}

void pitch_digits(BindCtx& c, svg::Image& img, svg::Node* g, int semis, uint32_t rgb) {
  if (!g || g->kids.size() < 4 || !c.font.ok()) return;
  const svg::Box sign = c.doc.bounds(&g->kids[1], false), dig = c.doc.bounds(&g->kids[3], false);
  if (sign.empty() || dig.empty()) return;
  const float gap = dig.h() * 0.12f;
  c.font.draw(img, sign.x1 + gap, dig.y0, dig.h(), std::to_string(std::min(99, std::abs(semis))), rgb, 1.5f);
}

// Remembers when each instrument parameter last changed (the snapshot only has values), so a screen can
// highlight what was touched last even when shift is already released.
struct ChangeClock {
  int prev[2][8] = {};
  double when[2][8] = {{-1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9}, {-1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9}};
  bool primed[2] = {};
  void update(int side, const Param* v, double t) {
    for (int i = 0; i < 8; ++i) {
      if (primed[side] && v[i] != prev[side][i]) when[side][i] = t;
      prev[side][i] = v[i];
    }
    primed[side] = true;
  }
  double since(int side, int i, double t) const { return t - when[side][i & 7]; }
};
ChangeClock g_knob_clock;

// ------------------------------------------------------------------ envelope curves

// Shapes (t 0..1 along a segment -> level 0..1). Attack rises fast and flattens, decay and release fall fast
// and flatten, like the authored a/d/r curves.
float attack_curve(float t) { return 1.f - (1.f - t) * (1.f - t); }
float fall_curve(float t) { return (1.f - t) * (1.f - t); }
float attack_inv(float l) { return 1.f - std::sqrt(clamp01(1.f - l)); }
float fall_inv(float l) { return 1.f - std::sqrt(clamp01(l)); }

// A curve segment from x0 to x1: level(t) mapped between yb (level 0) and yt (level 1).
template <typename F>
svg::Points curve(float x0, float x1, float yb, float yt, F level, int n = 24) {
  svg::Points p;
  p.reserve(size_t(n + 1));
  for (int i = 0; i <= n; ++i) {
    const float t = float(i) / float(n);
    p.emplace_back(lerp(x0, x1, t), lerp(yb, yt, level(t)));
  }
  return p;
}

// ------------------------------------------------------------------ adsr (synth envelope, T2)
// Decoded from the firmware's adsr screen (t3, 2026-10-23; SPEC-adsr.md): element init 0x01912848, layout
// 0x019129AC, per-frame draw 0x01913A90 (vtable 0x0190EA14 +0x1c), guide lines 0x01913038, curves 0x01913664,
// dot colours 0x01913534. Names: rdot adot addot dsdot srdot a d r s.
//   - a, d, r, s are removed from the document; their authored bounds frame the code-drawn curves:
//       x_a  = a.x0 + a.w * A        (end of attack, top a.y0)
//       x_d  = x_a  + d.w * D        (end of decay)
//       y_s  = a.y1 - a.h * S        (sustain level)
//       x_sr = r.x0 + r.w * R'       (start of release; release ends at r.x1, a.y1)
//     A, D, R, S are the adsr params as floats 0..1. The firmware normalises each stored value through a
//     per-parameter range table filled at run time (0x01A2843C); A/D/R use /16384 here (their stored range,
//     like play mode, whose 4 factory bands round to the 4 modes only with /16384) and S /32768 (inferred).
//     R' = 1 - R/16384: with R' = R the release would shorten as the release time grows (inferred inversion).
//   - curves (cubic, 1.5 px): attack (a.x0,a.y1) c1=start c2=(a.x0, a.y1 - 675/1024 a.h) -> (x_a, a.y0);
//     decay (x_a, d.y0) c1=start c2=(x_a, y_s) -> (x_d, y_s); sustain line (x_d,y_s)-(x_sr,y_s);
//     release (x_sr, y_s) c1=start c2=(x_sr, r.y1) -> (r.x1, r.y1).
//   - guides (offblue, 1.5 px, under the curves): verticals at x_a (a.y1..a.y0), x_d and x_sr (y_s..a.y1),
//     the baseline a.x0..r.x1 at a.y1.
//   - dots: addot moved by (x_a, 0), dsdot by (x_d, y_s), srdot by (x_sr, y_s); adot, rdot fixed.
//   - colours: segment k = lerp(blue, hi_k, anim_k), hi = lightblue / green / white / orange for A D S R;
//     anim_k jumps to 1 when encoder k turns and falls back at 1/1024 per tick (fade time ASSUMED 1 s).
//     adot = attack colour, rdot = release colour, addot/dsdot/srdot = the colour of the more lit neighbour.
//   - no per-voice markers: the draw routine reads no voice state.

constexpr uint32_t kAdsrBlue = 0x698EFF, kAdsrLightBlue = 0xBFD8FF, kAdsrGreen = 0x00ED95, kAdsrWhite = 0xFFFFFF,
                   kAdsrOrange = 0xFF423F, kAdsrOffBlue = 0x1D2040;  // colors.svg palette entries
constexpr double kAdsrFade = 1.0;  // seconds for an encoder highlight to fade (1024 ticks; ASSUMED ~1 kHz)

struct AdsrLayout {
  svg::Box a, d, r;
  float xa = 0, xd = 0, ys = 0, xsr = 0;
  bool ok = false;
};

AdsrLayout adsr_layout(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  AdsrLayout L;
  // authored bounds (bind runs after the overrides are reset; draw sees the same values, cached per frame)
  static AdsrLayout cache;
  static double cache_t = -1;
  if (cache_t == c.t && cache.ok) L = cache;
  else {
    L.a = c.box("a"), L.d = c.box("d"), L.r = c.box("r");
    L.ok = !L.a.empty() && !L.d.empty() && !L.r.empty();
  }
  if (!L.ok) return L;
  auto f16 = [&](int i) { return clamp01(float(iv.adsr[i]) / 16384.f); };
  const float A = f16(0), D = f16(1), R = 1.f - f16(3), S = clamp01(float(iv.adsr[2]) / 32768.f);
  L.xa = L.a.x0 + L.a.w() * A;
  L.xd = L.xa + L.d.w() * D;
  L.ys = L.a.y1 - L.a.h() * S;
  L.xsr = L.r.x0 + L.r.w() * R;
  cache = L, cache_t = c.t;
  return L;
}

svg::Points cubic(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, int n = 32) {
  svg::Points p;
  p.reserve(size_t(n + 1));
  for (int i = 0; i <= n; ++i) {
    const float t = float(i) / float(n), u = 1.f - t;
    const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    p.emplace_back(b0 * x0 + b1 * x1 + b2 * x2 + b3 * x3, b0 * y0 + b1 * y1 + b2 * y2 + b3 * y3);
  }
  return p;
}

float adsr_anim(const BindCtx& c, int k) {
  const double s = since_enc(c, k);
  return s < 0 ? 0.f : clamp01(float(1.0 - s / kAdsrFade));
}

void bind_adsr(BindCtx& c) {
  const AdsrLayout L = adsr_layout(c);
  if (!L.ok) return;
  const float ax0 = L.a.x0, base = L.a.y1;
  set_path(c.n("a"), cubic(ax0, base, ax0, base, ax0, base - L.a.h() * (675.f / 1024.f), L.xa, L.a.y0));
  set_path(c.n("d"), cubic(L.xa, L.d.y0, L.xa, L.d.y0, L.xa, L.ys, L.xd, L.ys));
  set_line(c.n("s"), L.xd, L.ys, L.xsr, L.ys);
  set_path(c.n("r"), cubic(L.xsr, L.ys, L.xsr, L.ys, L.xsr, L.r.y1, L.r.x1, L.r.y1));
  move(c.n("addot"), L.xa, 0);
  move(c.n("dsdot"), L.xd, L.ys);
  move(c.n("srdot"), L.xsr, L.ys);

  static const uint32_t hi[4] = {kAdsrLightBlue, kAdsrGreen, kAdsrWhite, kAdsrOrange};
  static const char* seg[4] = {"a", "d", "s", "r"};
  float an[4];
  uint32_t col[4];
  for (int k = 0; k < 4; ++k) {
    an[k] = adsr_anim(c, k);
    col[k] = mix(kAdsrBlue, hi[k], an[k]);
    if (svg::Node* n = c.n(seg[k])) {
      n->set("stroke", hex(col[k]));
      n->set("stroke-width", "1.5");
    }
  }
  paint_tree(c.n("adot"), col[0], true);
  paint_tree(c.n("rdot"), col[3], true);
  paint_tree(c.n("addot"), an[1] < an[0] ? col[0] : col[1], true);
  paint_tree(c.n("dsdot"), an[2] < an[1] ? col[1] : col[2], true);
  paint_tree(c.n("srdot"), an[3] < an[2] ? col[2] : col[3], true);
}

// The guide lines, drawn by the firmware before the document (so under the curves and dots): painted on a
// scratch image and copied only where the rendered screen is still background.
void draw_adsr(BindCtx& c, svg::Image& img) {
  const AdsrLayout L = adsr_layout(c);
  if (!L.ok) return;
  static svg::Image g;
  if (g.w != img.w || g.h != img.h) g = svg::Image(img.w, img.h);
  std::fill(g.px.begin(), g.px.end(), 0xFF000000u);
  const float s = float(img.w) / 320.f, base = L.a.y1;
  auto line = [&](float x0, float y0, float x1, float y1) {
    svg::stroke(g, {{x0 * s, y0 * s}, {x1 * s, y1 * s}}, 1.5f * s, kAdsrOffBlue, false, 1.f, false);
  };
  line(L.xa, base, L.xa, L.a.y0);
  line(L.xd, L.ys, L.xd, base);
  line(L.xsr, L.ys, L.xsr, base);
  line(L.a.x0, base, L.r.x1, base);
  for (size_t i = 0; i < img.px.size(); ++i)
    if ((img.px[i] & 0xFFFFFFu) == 0 && (g.px[i] & 0xFFFFFFu) != 0) img.px[i] = g.px[i];
}
// ------------------------------------------------------------------ dynaenv (drum envelope, drum T2)
// Firmware: "dynaenv.svg" with blue green white center bdot wdot bgdot gwdot g1dot g2dot next to it.
// Elements: center (the dashed time axis, kept), blue (rise, re-pathed), white (fall, re-pathed), green (the
// hold length, a dimension line under the curve as authored), bdot/bgdot/gwdot/wdot (the curve's corners),
// g1dot/g2dot (the ends of the green line). t5: the drum envelope page's encoders edit dyna_env 0..4
// (InstrumentOp::SetAdsr on a drum, with the firmware's ranges) and InstrumentView::adsr shows dyna_env, so
// adsr[0] (blue, -32767..32767) = rise, adsr[1] (green, 0..8192) = hold, adsr[2] (white, -32767..32767) =
// fall. ASSUMED: the sound of the slots is not decoded (drum.cpp: |s0| fades in, |s2| fades out, a negative
// value curves the other way; s1 has no effect there).

constexpr float kDynX0 = 28.046f, kDynBase = 84.35f, kDynTop = 26.162f, kDynGreenY = 138.161f;

struct DynLayout { float xa, xh, xr; };

DynLayout dyn_layout(const InstrumentView& iv) {
  DynLayout L;
  L.xa = kDynX0 + 3.f + 82.f * std::fabs(q15(iv.adsr[0]));
  L.xh = L.xa + 2.f + 92.f * clamp01(float(iv.adsr[1]) / 8192.f);
  L.xr = L.xh + 3.f + 82.f * std::fabs(q15(iv.adsr[2]));
  return L;
}

void bind_dynaenv(BindCtx& c) {
  const DynLayout L = dyn_layout(c.inst());
  set_path(c.n("blue"), curve(kDynX0, L.xa, kDynBase, kDynTop, attack_curve));
  set_path(c.n("white"), curve(L.xh, L.xr, kDynBase, kDynTop, fall_curve));
  set_line(c.n("green"), L.xa, kDynGreenY, L.xh, kDynGreenY);
  // all six dots are authored around (-0.168, 0.168)
  place(c.n("bdot"), kDynX0, kDynBase, -0.168f, 0.168f);
  place(c.n("bgdot"), L.xa, kDynTop, -0.168f, 0.168f);
  place(c.n("gwdot"), L.xh, kDynTop, -0.168f, 0.168f);
  place(c.n("wdot"), L.xr, kDynBase, -0.168f, 0.168f);
  place(c.n("g1dot"), L.xa, kDynGreenY, -0.168f, 0.168f);
  place(c.n("g2dot"), L.xh, kDynGreenY, -0.168f, 0.168f);
  paint_tree(c.n("g1dot"), kColGreen, true);
  paint_tree(c.n("g2dot"), kColGreen, true);
}

// The newest drum hit: rising = on the blue curve, holding near full = along the hold, falling = on the
// white curve (drum.cpp / dbox.cpp: display[16 + key] = level of key, last key in [41] (drum) / [40] (dbox)).
struct DrumTrack { int key = -1; int phase = 0; float prev = 0, rel0 = 1; double t_hold = 0; };
DrumTrack g_dyn;

int drum_last_key(const InstrumentView& iv) {
  const int k = iv.type == InstrumentType::Dbox ? iv.display[40] : iv.display[41];
  return k >= 0 && k < kDrumKeys ? k : -1;
}

void draw_dynaenv(BindCtx& c, svg::Image& img) {
  const InstrumentView& iv = c.inst();
  const DynLayout L = dyn_layout(iv);
  const int key = drum_last_key(iv);
  const float lvl = key >= 0 ? clamp01(float(iv.display[16 + key]) / 32767.f) : 0.f;
  DrumTrack& d = g_dyn;
  if (key < 0 || lvl <= 0.f) { d.phase = 0; d.prev = 0; d.key = key; return; }
  if (key != d.key || d.phase == 0 || lvl > d.prev + 0.05f) { d.key = key; d.phase = 1; }
  if (d.phase == 1 && lvl >= 0.98f) { d.phase = 2; d.t_hold = c.t; }
  if (d.phase == 1 && lvl < d.prev - 0.002f) { d.phase = 3; d.rel0 = std::max(d.prev, 1e-3f); }
  if (d.phase == 2 && lvl < 0.97f) { d.phase = 3; d.rel0 = std::max(lvl, 1e-3f); }
  d.prev = lvl;
  float x, y;
  if (d.phase == 1) {
    const float t = attack_inv(lvl);
    x = lerp(kDynX0, L.xa, t), y = lerp(kDynBase, kDynTop, attack_curve(t));
  } else if (d.phase == 2) {  // the hold is not reported: walk it in 0.5 s (ASSUMED)
    x = lerp(L.xa, L.xh, clamp01(float(c.t - d.t_hold) / 0.5f)), y = kDynTop;
  } else {
    const float t = fall_inv(lvl / d.rel0);
    x = lerp(L.xh, L.xr, t), y = lerp(kDynBase, kDynTop, fall_curve(t));
  }
  svg::circle(img, x, y, 3.2f, 0x000000, true);
  svg::circle(img, x, y, 2.6f, kColWhite, true);
}

// ------------------------------------------------------------------ playmode (synth shift+T2)
// Decoded from the firmware's playmode screen (t3, 2026-10-23; SPEC-playmode.md): init 0x01A0D968, value
// update 0x01A0DA44, per-frame draw 0x01A0DAD8. Names next to "playmode.svg": playmode portamento dt dtval
// g gval, words poly mono legato unison ".", "off" "max" "%d".
//   - init: playmode and portamento are text nodes; dt, dtval, g and gval are removed from the document
//     (0x01A3B7D0, the same call that removes the code-drawn parts of sampler/adsr): the G-BEND and DETUNE
//     rows are not shown on the device.
//   - mode = (unsigned)(3 * p4 + 0.5), p4 = adsr[4] normalised (/16384, inferred: the factory bands
//     2048/6144/10240/14336 then give poly/mono/legato/unison); 0..3 -> the word, >= 4 -> ".".
//   - portamento = (unsigned)(128 * p5 + 0.5), p5 = adsr[5] / 16384: 0 -> "off", 128 -> "max", else "%d".
//   - text is set every frame; no colour or position changes.

struct PlaymodeText { std::string pm, porta; };

PlaymodeText playmode_text(const InstrumentView& iv) {
  static const char* words[4] = {"poly", "mono", "legato", "unison"};
  const float p4 = float(iv.adsr[4]) / 16384.f, p5 = float(iv.adsr[5]) / 16384.f;
  const auto mode = uint32_t(std::max(0.f, 3.f * p4 + 0.5f));
  const auto porta = uint32_t(std::max(0.f, 128.f * p5 + 0.5f));
  PlaymodeText t;
  t.pm = mode < 4 ? words[mode] : ".";
  if (porta == 0) t.porta = "off";
  else if (porta == 128) t.porta = "max";
  else t.porta = std::to_string(porta);
  return t;
}

void bind_playmode(BindCtx& c) {
  for (const char* id : {"playmode", "portamento", "g", "gval", "dt", "dtval"}) show(c.n(id), false);
}

void draw_playmode(BindCtx& c, svg::Image& img) {
  if (!c.font.ok()) return;
  const PlaymodeText t = playmode_text(c.inst());
  // ASSUMED placement: left-aligned at the authored placeholder glyphs (cap top, height 19.84), in their
  // authored colours: playmode x 214.74 y 17.50 (blue), portamento x 214.37 y 49.94 (green)
  constexpr float kH = 19.84f;
  const float s = float(img.w) / 320.f;
  c.font.draw(img, 214.74f * s, 17.50f * s, kH * s, t.pm, kColBlue, 1.5f * s);
  c.font.draw(img, 214.37f * s, 49.94f * s, kH * s, t.porta, kColGreen, 1.5f * s);
}

// ------------------------------------------------------------------ octave pop-up (z / x)
// Firmware: "octave.svg" with "text" next to it. Elements: rect (the box), the keyboard icon (kept), text:
// kid 0 = the digit, kid 1 = the '-' bar, kid 2 = the '+' upright. UiState::octave -3..3.

void bind_octave(BindCtx& c) {
  svg::Node* t = c.n("text");
  if (!t || t->kids.size() < 3) return;
  const int o = c.ui.octave;
  show(&t->kids[1], o != 0);
  show(&t->kids[2], o > 0);
  if (c.font.ok()) c.font.set(c.doc, &t->kids[0], char('0' + std::min(9, std::abs(o))));
}

// ------------------------------------------------------------------ drum2 (drum sampler, drum T1)
// Firmware: "drum2.svg" (DrumScreen.cpp; asserts m_sampler, m_sample_buffer, note_name at :228) with
// magnified overview reverse play_fwd play_rev pm_rev loopin loopinbar loopout loopoutbar loop oneshot gate
// loop_rev oneshot_rev gate_rev rootnote pitch gain loopinring loopoutring and "%+.0f" next to it.
// Engine (drum.cpp): knobs 0 tune (+-12 st), 1 start offset (+10 % of the slice), 2 end offset, 3 level;
// display [0..7] voice keys, [16..39] key levels, [40] sample length, [41] last key, [42] keys from a patch.
// The kit audio itself is not in the snapshot, so the waves are drawn by code from what is: the slice layout
// and the key levels (overview), and the output while a key plays (magnified).

struct DrumGeom {
  svg::Box mag, ovw;
};

DrumGeom drum_geom(BindCtx& c) {
  DrumGeom g;
  g.mag = c.box("magnified");
  g.ovw = c.box("overview");
  if (g.mag.empty()) g.mag = {16.853f, 50.f, 306.f, 134.18f};
  if (g.ovw.empty()) g.ovw = {100.5f, 17.9f, 268.8f, 27.54f};
  return g;
}

// x of a slice position f (0 = slice start, 1 = slice end) in the magnified view, which shows the slice
// with a 10 % margin on both sides (the start/end offsets reach 10 %).
float mag_x(const svg::Box& m, float f) { return m.x0 + m.w() * (f + 0.1f) / 1.2f; }

// The edited key's pitch in semitones: 6208 raw per octave (drum.cpp, I).
int drum_semis(const InstrumentView& iv) { return int(std::lround(float(iv.display[46]) * 12.f / 6208.f)); }

// Min/max of a sample range [a, b) into n columns (mono or the left channel), each -1..1.
void sample_columns(const SampleData& sd, int64_t a, int64_t b, int n, float* lo, float* hi) {
  a = std::clamp<int64_t>(a, 0, sd.length);
  b = std::clamp<int64_t>(b, a, sd.length);
  for (int x = 0; x < n; ++x) {
    const int64_t i0 = a + (b - a) * x / n, i1 = std::max<int64_t>(i0 + 1, a + (b - a) * (x + 1) / n);
    float mn = 0, mx = 0;
    const int64_t step = std::max<int64_t>(1, (i1 - i0) / 64);  // at most 64 reads per column
    for (int64_t i = i0; i < i1 && i < sd.length; i += step) {
      const float v = float(sd.frames[sd.channels == 2 ? 2 * i : i]) / 32768.f;
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    lo[x] = mn;
    hi[x] = mx;
  }
}

void bind_drum2(BindCtx& c) {
  const InstrumentView& iv = c.s.drum;
  const DrumGeom g = drum_geom(c);
  g_knob_clock.update(1, iv.knobs, c.t);
  show(c.n("magnified"), false);  // drawn by code (draw_drum2)
  show(c.n("overview"), false);
  // ---- root note of the last key played (keys map from notes as (note - 53) mod 24)
  const int key = std::max(0, drum_last_key(iv));
  int note = 53 + key;
  if (c.ui.last_note >= 0 && ((c.ui.last_note - 53) % kDrumKeys + kDrumKeys) % kDrumKeys == key) note = c.ui.last_note;
  bool sharp = false;
  const std::string nn = note_name(note, sharp);
  svg::Node* rn = c.n("rootnote");
  if (nn.size() == 2) text_into(c, slots_lr(c, rn), nn);  // a sharp is drawn small between them (draw_drum2)
  else show(rn, false);
  // t5: everything below shows the key the encoders edit (the last played; drum.cpp display[43..53]), as the
  // firmware's DrumScreen does: blue pitch, green / white start / end, orange play mode, shift+blue reverse,
  // shift+orange volume.
  // ---- pitch (blue) and direction: the read-out while pitched or turning, else the play arrow
  const int semis = drum_semis(iv);
  const bool pitch_on = semis != 0 || (!c.ui.shift && since_enc(c, 0) < 1.5);  // shift+blue = reverse
  show(c.n("pitch"), pitch_on);
  if (pitch_on) pitch_sign(c.n("pitch"), semis);
  const bool rev = iv.display[51] != 0;
  show(c.n("reverse"), !pitch_on || rev);
  show(c.n("play_fwd"), !pitch_on && !rev);
  show(c.n("play_rev"), !pitch_on && rev);
  // ---- play mode (orange): gate / one-shot / loop, the reversed set when the key plays backwards; volume
  // (shift+orange) is drawn where the icon sits while it turns
  const bool gain_on = c.ui.shift && since_enc(c, 3) < 1.5;
  const int mode = std::clamp(iv.display[50], 0, 2);  // 0 gate, 1 one-shot, 2 loop
  const int icon = mode == 0 ? 1 : (mode == 1 ? 2 : 0);
  show(c.n("pm"), !gain_on && !rev);
  one_of(c, {"loop", "gate", "oneshot"}, icon);
  show(c.n("pm_rev"), !gain_on && rev);
  one_of(c, {"loop_rev", "gate_rev", "oneshot_rev"}, icon);
  show(c.n("gain"), false);  // the authored gain group repeats the loop icon: the level is drawn instead
  // ---- start (green) / end (white) markers at the slice edges; the magnified view shows the slice between them
  const float fs = 0.f, fe = 1.f;
  const float xs = mag_x(g.mag, fs), xe = mag_x(g.mag, fe);
  constexpr float kMarkX = 16.83f;  // loopin / loopout and their bars are authored at the left edge
  move(c.n("loopin"), xs - kMarkX, 0);
  move(c.n("loopout"), xe - kMarkX, 0);
  move(c.n("loopinbar"), xs - kMarkX, 0);
  move(c.n("loopoutbar"), xe - kMarkX, 0);
  // the start / end bar while green / white turns (t5: the encoders edit the key, not the kit-wide knobs)
  const double ts = since_enc(c, 1), te = since_enc(c, 2);
  show(c.n("loopinbar"), ts < 1.5 && ts <= te);
  show(c.n("loopoutbar"), te < 1.5 && te < ts);
}

void draw_drum2(BindCtx& c, svg::Image& img) {
  const InstrumentView& iv = c.s.drum;
  const DrumGeom g = drum_geom(c);
  constexpr uint32_t kDimWave = 0x00475B, kSelWave = 0x00AB6B;
  const bool has_sample = iv.display[40] > 0;
  const int key = std::max(0, drum_last_key(iv));
  // ---- root note with a sharp (three characters do not fit the two slots)
  int note = 53 + key;
  if (c.ui.last_note >= 0 && ((c.ui.last_note - 53) % kDrumKeys + kDrumKeys) % kDrumKeys == key) note = c.ui.last_note;
  bool sharp = false;
  const std::string nn = note_name(note, sharp);
  if (c.font.ok()) {
    if (nn.size() != 2) c.font.draw(img, 9.36f, 8.6f, 20.f, nn, kColWhite, 1.5f);
  }
  if (sharp) {  // a small raised '#' between the letter (9.4 .. 19.4) and the octave slot (cap 8.6 .. 28.6)
    constexpr float x0 = 20.3f, y0 = 8.6f, w = 3.8f, h = 6.6f;
    svg::stroke(img, {{x0 + 1.1f, y0}, {x0 + 1.1f, y0 + h}}, 1.1f, kColWhite, false, 1.f, false);
    svg::stroke(img, {{x0 + w - 1.1f, y0}, {x0 + w - 1.1f, y0 + h}}, 1.1f, kColWhite, false, 1.f, false);
    svg::stroke(img, {{x0, y0 + 2.f}, {x0 + w, y0 + 2.f}}, 1.1f, kColWhite, false, 1.f, false);
    svg::stroke(img, {{x0, y0 + h - 2.f}, {x0 + w, y0 + h - 2.f}}, 1.1f, kColWhite, false, 1.f, false);
  }
  // ---- pitch digits after the sign (blue)
  const int semis = drum_semis(iv);
  if (semis != 0 || (!c.ui.shift && since_enc(c, 0) < 1.5)) pitch_digits(c, img, c.n("pitch"), semis, kColBlue);
  // ---- volume (shift+orange) while it turns: the gain (volume / 8192)^2 in percent, where the icon sits
  if (c.ui.shift && since_enc(c, 3) < 1.5 && c.font.ok()) {
    char b[16];
    const float v = float(iv.display[49]) / 8192.f;
    std::snprintf(b, sizeof b, "%d", int(std::lround(v * v * 100.f)));
    c.font.draw(img, 280.f, 10.f, 14.f, b, kColRed, 1.5f);
  }
  // ---- overview: the 24 slices of the kit along the strip; the last key in green, playing keys rise
  const svg::Box& o = g.ovw;
  const float base = o.y1, hgt = std::max(4.f, o.h());
  if (has_sample && iv.sample && iv.sample->frames && iv.sample->length > 1) {
    // t5: the kit's own waveform (as DrumScreen reads m_sample_buffer), the edited key's slice in green
    const SampleData& sd = *iv.sample;
    const int n = std::max(8, int(o.w()));
    float lo[512], hi[512];
    const int cols = std::min(n, 512);
    sample_columns(sd, 0, sd.length, cols, lo, hi);
    const float mid = (o.y0 + o.y1) / 2, half = std::max(2.f, o.h() / 2);
    const int64_t a = iv.display[52], b = iv.display[53];
    for (int x = 0; x < cols; ++x) {
      const int64_t i = int64_t(sd.length) * x / cols;
      const float px = o.x0 + o.w() * (float(x) + 0.5f) / float(cols);
      const uint32_t col = i >= a && i <= b ? kColGreen : kDimWave;
      svg::stroke(img, {{px, mid - hi[x] * half}, {px, mid - lo[x] * half + 0.5f}}, 1.f, col, false, 1.f, false);
    }
  } else if (!has_sample) {
    svg::stroke(img, {{o.x0, base}, {o.x1, base}}, 1.5f, kDimWave, false, 1.f, false);
    const float w = o.w() / kDrumKeys;
    svg::stroke(img, {{o.x0 + w * key, base}, {o.x0 + w * (key + 1), base}}, 1.5f, kSelWave, false, 1.f, false);
  } else {
    const float w = o.w() / kDrumKeys;
    for (int k = 0; k < kDrumKeys; ++k) {
      const float x0 = o.x0 + w * k + 0.6f, x1 = o.x0 + w * (k + 1) - 0.6f;
      const float l = clamp01(float(iv.display[16 + k]) / 32767.f);
      const float top = base - 1.5f - l * (hgt - 1.5f);
      const uint32_t col = k == key ? kColGreen : kDimWave;
      svg::stroke(img, {{x0, base}, {x0 + (x1 - x0) * 0.3f, top}, {x1, base}}, 1.5f, col, false, 1.f, true);
    }
  }
  // ---- magnified: the selected slice between the markers; the output while a key sounds
  const svg::Box& m = g.mag;
  const float fs = 0.f, fe = 1.f;  // t5: the markers sit on the edited key's slice edges
  const float xs = mag_x(m, fs), xe = mag_x(m, fe), mb = m.y1;
  svg::stroke(img, {{m.x0, mb}, {xs, mb}}, 1.5f, kDimWave, false, 1.f, false);
  svg::stroke(img, {{xe, mb}, {m.x1, mb}}, 1.5f, kDimWave, false, 1.f, false);
  const bool sounding = has_sample && iv.voices_active != 0;
  if (has_sample && iv.sample && iv.sample->frames && iv.sample->length > 1 && xe > xs + 2) {
    // t5: the slice's own waveform between the markers (the firmware draws the sample buffer, DrumScreen.cpp)
    const int cols = std::min(320, int(xe - xs));
    float lo[320], hi[320];
    sample_columns(*iv.sample, iv.display[52], int64_t(iv.display[53]) + 1, cols, lo, hi);
    const float mid = (m.y0 + mb) / 2, half = std::max(2.f, (mb - m.y0) / 2);
    for (int x = 0; x < cols; ++x) {
      const float px = xs + (xe - xs) * (float(x) + 0.5f) / float(cols);
      svg::stroke(img, {{px, mid - hi[x] * half}, {px, mid - lo[x] * half + 0.5f}}, 1.f, kSelWave, false, 1.f, false);
    }
  } else if (!sounding) {
    svg::stroke(img, {{xs, mb}, {xe, mb}}, 1.5f, kSelWave, false, 1.f, false);
  } else {
    const int n = std::max(2, int(xe - xs));
    svg::Points pts;
    pts.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
      const float v = std::fabs(float(c.s.scope[i * (kDisplayW - 1) / (n - 1)]) / 2147483648.f);
      pts.emplace_back(xs + float(i), mb - clamp01(v * 1.5f) * (mb - m.y0));
    }
    svg::stroke(img, pts, 1.5f, kSelWave, false, 1.f, true);
  }
}

// ------------------------------------------------------------------ dbox (drum synth, drum T1 after R)
// Firmware: "dbox.svg" with pitch p1line p2line p1ring p2ring g1ls g2ls w1ring w2ring w1line w2line funkline
// filtline redring o1w0 o1w1 o1w2 o2w0 o2w1 o2w2 modering o1%i o2%i next to it (dbox.cpp asserts nearby).
// Layout per oscillator row (1 top, 2 bottom): pitch knob (pN ring + line), wave slider (gN group, icons
// oNw0..2), envelope knob (wN ring + line, labels DECAY/LONG/SHORT/ATTACK in group wN), four dots oN0..oN3;
// right panel: mode knob (modering + funkline), filter knob (redring + filtline), r1/r2 (MODE / FILTER
// lamps), key grid k0..k23. Engine (dbox.cpp): key values 0 pitch1 1 wave1 2 env1 3 cross mod 4 pitch2
// 5 wave2 6 env2 7 filter, plus knobs[0..7] as kit-wide offsets; display [40] last key, [41] from a patch,
// [16..39] key levels. Shift = the second page = oscillator 2 and the filter.

constexpr uint32_t kDbLabel = 0xDED7FF, kDbDim = 0x514959, kDbKnob = 0x638BFF, kDbGreen = 0x00EE92,
                   kDbGreenDim = 0x00676C, kDbRed = 0xFF395A, kDbLamp = 0x522132, kDbKey = 0x8D5141,
                   kDbKeySel = 0xFF7D70;

// t5: the edited key's eight words come with the view (dbox.cpp display[42..49], the last played key, also for
// factory kits); the second page (oscillator 2 + filter) is active while shift is held or right after one of
// its words changed.
ChangeClock g_dbox_clock;
Param g_dbox_words[8] = {};

bool dbox_second(const BindCtx& c) {
  double recent2 = 1e9, recent1 = 1e9;
  for (int i = 0; i < 4; ++i) {
    recent1 = std::min(recent1, g_dbox_clock.since(1, i, c.t));
    recent2 = std::min(recent2, g_dbox_clock.since(1, 4 + i, c.t));
  }
  return c.ui.shift || (recent2 < 1.5 && recent2 < recent1);
}

// The active oscillator's pitch in semitones from 261.63 Hz: word 0 signed, 6208 raw per octave (dbox.cpp, I);
// word 4 unsigned on dbox's 30 Hz .. 8 kHz curve.
int dbox_semis(const BindCtx& c) {
  const InstrumentView& iv = c.s.drum;
  if (!dbox_second(c)) return int(std::lround(float(iv.display[42]) * 12.f / 6208.f));
  const double hz = 30.0 * std::pow(8000.0 / 30.0, double(std::clamp(iv.display[46], 0, 32767)) / 32767.0);
  return int(std::lround(12.0 * std::log2(hz / 261.63)));
}

void draw_dbox(BindCtx& c, svg::Image& img) { pitch_digits(c, img, c.n("pitch"), dbox_semis(c), kColBlue); }

void bind_dbox(BindCtx& c) {
  const InstrumentView& iv = c.s.drum;
  for (int i = 0; i < 8; ++i) g_dbox_words[i] = Param(std::clamp(iv.display[42 + i], -32768, 32767));
  g_dbox_clock.update(1, g_dbox_words, c.t);
  const int key = std::clamp(iv.display[50], 0, kDrumKeys - 1);
  auto val = [&](int p) { return float(std::clamp(iv.display[42 + p], 0, 32767)) / 32767.f; };  // 0..1
  // the second page (oscillator 2 + filter) is active while shift is held or right after one of its knobs moved
  const bool second = dbox_second(c);
  for (int o = 0; o < 2; ++o) {
    const bool act = (o == 1) == second;
    const int p0 = o * 4;
    const std::string sfx = std::to_string(o + 1);
    // pitch knob: -140 .. +140 degrees (the outer ticks)
    // pitch 1 is signed with 0 at the top (dbox.svg marks 0 there); pitch 2 runs over the dial
    const svg::Box pr = c.box("p" + sfx + "ring");
    const int pw = iv.display[42 + p0];
    const float pa = o == 0 ? (pw >= 0 ? 140.f * float(pw) / 24832.f : 140.f * float(pw) / 24320.f)
                            : lerp(-140.f, 140.f, val(p0 + 0));
    rotate(c, c.n("p" + sfx + "line"), pa, pr.cx(), pr.cy());
    paint_tree(c.n("p" + sfx + "ring"), act ? kDbKnob : kDbDim);
    // wave slider (sine at the bottom .. the top) and the icon of the nearest wave
    const float w = val(p0 + 1);
    move(c.n("g" + sfx), 0, -w * (65.244f - 13.78f));
    const int wi = w < 1.f / 3.f ? 0 : (w < 2.f / 3.f ? 1 : 2);
    for (int i = 0; i < 3; ++i)
      paint_tree(c.n("o" + sfx + "w" + std::to_string(i)), i == wi ? (act ? kDbGreen : kDbGreenDim) : kDbDim);
    paint_tree(c.n("g" + sfx + "ls"), act ? kDbGreen : kDbGreenDim);
    // envelope knob: short (-140) .. long (+140)
    const svg::Box wr = c.box("w" + sfx + "ring");
    rotate(c, c.n("w" + sfx + "line"), lerp(-140.f, 140.f, val(p0 + 2)), wr.cx(), wr.cy());
    paint_tree(c.n("w" + sfx), act ? kDbLabel : kDbDim);
    paint_tree(c.n("w" + sfx + "line"), act ? kDbLabel : kDbDim);
  }
  // MODE (orange): 8 discrete positions, word 3 >> 12 (F); the filter (shift + orange, 0 = open), -120 .. +120
  const svg::Box mr = c.box("modering"), rr = c.box("redring");
  // the dial marks 0 (bottom) .. 7 sit 45 degrees apart, 4 at the top (the needle's authored direction)
  rotate(c, c.n("funkline"), -180.f + 45.f * float(std::clamp(iv.display[51], 0, 7)), mr.cx(), mr.cy());
  rotate(c, c.n("filtline"), lerp(-120.f, 120.f, val(7)), rr.cx(), rr.cy());
  paint_tree(c.n("r1"), second ? kDbLamp : kDbRed, true);
  paint_tree(c.n("r2"), second ? kDbRed : kDbLamp, true);
  // pitch read-out: the active oscillator's offset in semitones (digits in draw_dbox)
  pitch_sign(c.n("pitch"), dbox_semis(c));
  // the four dots of each row: the level of the selected key while it sounds
  const float lvl = clamp01(float(iv.display[16 + key]) / 32767.f);
  const int lit = lvl > 0.f ? 1 + int(lvl * 3.99f) : 0;
  for (int o = 1; o <= 2; ++o)
    for (int i = 0; i < 4; ++i)
      paint_tree(c.n("o" + std::to_string(o) + std::to_string(i)), i < lit ? kDbRed : kDbLamp, true);
  // key grid: the selected key outlined bright, sounding keys filled by level
  for (int k = 0; k < kDrumKeys; ++k) {
    svg::Node* kn = c.n("k" + std::to_string(k));
    if (!kn) continue;
    kn->set("stroke", hex(k == key ? kDbKeySel : kDbKey));
    const float l = clamp01(float(iv.display[16 + k]) / 32767.f);
    if (l > 0.02f) kn->set("fill", hex(mix(0x000000, kDbKeySel, 0.25f + 0.75f * l)));
  }
}

}  // namespace

OP1_SCREEN("adsr", bind_adsr, draw_adsr);
OP1_SCREEN("dynaenv", bind_dynaenv, draw_dynaenv);
OP1_SCREEN("playmode", bind_playmode, draw_playmode);
OP1_SCREEN("octave", bind_octave, nullptr);
OP1_SCREEN("drum2", bind_drum2, draw_drum2);
OP1_SCREEN("dbox", bind_dbox, draw_dbox);

}  // namespace op1
