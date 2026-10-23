// Synth engine screen "dsynth". Decoded from the firmware by t3 (SPEC-dsynth.md in the t3 work folder):
// screen object 0x01CEBC34 (ctor 0x0197F8C4, vtable 0x0190FBEC), init 0x0197F3D8, per-frame update 0x0197FA40.
// p0..p7 = the eight parameters 0..1 (blue, green, white, orange; shift page 4..7).
//
//   page colours (shift flag 0x01DD7F78; decoded swap, the palette entries are inferred):
//       page 1: g1ls/line582 green, g2ls/line528 dim, w1 white, w2 dim, r1 red, r2 dark red,
//               modering white, redring dim, p2ring dim;
//       page 2: the same swapped, p2ring takes p2line's blue.
//   p2line  rotates about p2ring's centre by (p4 - 0.5) * 287.93 deg (sweep 2 pi * 819/1024),
//   w1line / w2line  about w1ring / w2ring by (p2 - 0.5) / (p6 - 0.5) * 287.93 deg,
//   funkline about redring by (p3 - 0.5791) * 277.03 deg (2 pi * 788/1024, offset 593/1024): continuous,
//            the 1..7 marks are only a scale,
//   filtline about redring by (p7 - 0.5) * 244.69 deg (2 pi * 696/1024),
//   g1 / g2  move up by 51.0 px * p1 / p5,
//   o1w0..2 / o2w0..2  colour = mix(green, dim, w) with w = min(2p, 1), min(|1 - 2p|, 1), min(2 - 2p, 1),
//   o1%i / o2%i (i = 0 bottom .. 3)  red (path242's colour) when i < 4 * level of the newest voice, else dim,
//   osc1env / osc2env  removed at init; the firmware draws the two crossfade shapes by code (draw below),
//   bottom-right box: a 62-point scope of the newest voice (x = 193 + 2i, y = 130 - 15 v), osc2env's colour.
// ASSUMED: the shapes' level inputs come from display[22]/[23] and the scope from the mixed output (the
// firmware reads per-voice buffers the snapshot does not carry: REQUESTS in the report); the group linestyles
// of w1/w2 are applied to the white/dim children only.
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

constexpr float kSweep = 360.f * 819.f / 1024.f;       // 0x333 * 2 pi
constexpr float kFunkSweep = 360.f * 788.f / 1024.f;   // 0x314 * 2 pi
constexpr float kFunkZero = 593.f / 1024.f;            // 0x251
constexpr float kFiltSweep = 360.f * 696.f / 1024.f;   // 0x2B8 * 2 pi
// Palette entries 0x01BAAF58 [0], [1], [12] (run-time table, not decoded): inferred from the template.
constexpr uint32_t kPalRed = 0xFF395A, kPalDarkRed = 0x522132, kPalWhite = 0xDED7FF;

struct DsColours {
  uint32_t p2line, p2ring, green, env1, env2, red;
};
DsColours ds_colours(BindCtx& c) {
  DsColours k{};
  k.p2line = colour_of(c.n("p2line"));
  k.p2ring = colour_of(c.n("p2ring"));
  k.green = colour_of(c.n("line582"));
  k.env1 = colour_of(c.n("osc1env"));
  k.env2 = colour_of(c.n("osc2env"));
  k.red = colour_of(c.n("path242"));
  return k;
}

// Recolours the descendants authored in the page colours (white/dim), not the blue accents.
void tint_page(svg::Node* n, uint32_t rgb) {
  if (!n) return;
  const uint32_t s = colour_of(n, "stroke"), f = colour_of(n, "fill");
  if (s == kRingWhite || s == kDimGrey || f == kRingWhite || f == kDimGrey) tint(n, rgb);
  for (svg::Node& k : n->kids) tint_page(&k, rgb);
}

void needle(BindCtx& c, const char* line, const char* ring, float deg) {
  const svg::Box b = c.box(ring);
  if (b.empty()) return;
  rotate(c, c.n(line), deg, b.cx(), b.cy());
}

void bind_dsynth(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const DsColours k = ds_colours(c);
  float p[8];
  for (int i = 0; i < 8; ++i) p[i] = knob(iv, i);
  const bool sh = c.ui.shift;
  const uint32_t dim = k.p2ring;

  // ---- page colours
  tint(c.n("p2ring"), sh ? k.p2line : dim);
  stroke_col(c.n("g1ls"), sh ? dim : k.green);
  tint(c.n("line582"), sh ? dim : k.green);
  stroke_col(c.n("g2ls"), sh ? k.green : dim);
  tint(c.n("line528"), sh ? k.green : dim);
  tint_page(c.n("w1"), sh ? dim : kPalWhite);
  tint_page(c.n("w2"), sh ? kPalWhite : dim);
  tint(c.n("r1"), sh ? kPalDarkRed : kPalRed);
  tint(c.n("r2"), sh ? kPalRed : kPalDarkRed);
  tint(c.n("modering"), sh ? dim : kPalWhite);
  tint(c.n("redring"), sh ? kPalWhite : dim);

  // ---- needles
  needle(c, "p2line", "p2ring", (p[4] - 0.5f) * kSweep);
  needle(c, "w1line", "w1ring", (p[2] - 0.5f) * kSweep);
  needle(c, "w2line", "w2ring", (p[6] - 0.5f) * kSweep);
  needle(c, "funkline", "redring", (p[3] - kFunkZero) * kFunkSweep);
  needle(c, "filtline", "redring", (p[7] - 0.5f) * kFiltSweep);

  // ---- wave sliders and their icons
  for (int o = 0; o < 2; ++o) {
    const float v = p[o ? 5 : 1];
    move(c.n(o ? "g2" : "g1"), 0, -51.f * v);
    const float wk[3] = {std::min(2.f * v, 1.f), std::min(std::fabs(1.f - 2.f * v), 1.f), std::min(2.f - 2.f * v, 1.f)};
    for (int i = 0; i < 3; ++i) {
      char id[8];
      std::snprintf(id, sizeof id, "o%dw%d", o + 1, i);
      tint(c.n(id), mix_rgb(k.green, dim, wk[i]));
    }
    // envelope level dots, o?0 at the bottom
    const float lvl = clamp01(float(iv.display[22 + o]) / 32767.f);
    for (int i = 0; i < 4; ++i) {
      char id[8];
      std::snprintf(id, sizeof id, "o%d%d", o + 1, i);
      tint(c.n(id), float(i) < 4.f * lvl ? k.red : dim);
    }
  }
  // ---- the crossfade shapes are drawn by code
  show(c.n("osc1env"), false);
  show(c.n("osc2env"), false);
}

struct DsScope {
  float v[62] = {};
};
DsScope g_ds_scope;

void draw_dsynth(BindCtx& c, svg::Image& img) {
  const InstrumentView& iv = c.inst();
  const DsColours k = ds_colours(c);
  const float sc = c.doc.px_scale(img.w, img.h);
  auto P = [sc](float x, float y) { return std::make_pair(x * sc, y * sc); };

  // ---- envelope crossfader (0x01980EFA .. 0x019812CA): two 5-point shapes from (14, 55), 18 px high
  const float q = knob(iv, 0) * 0.999f;
  const int seg = std::clamp(int(q * 4.f), 0, 3);
  const float t = float((int(1024.f * q) * 4) & 1023) / 1024.f;
  float X[2], C[2];
  switch (seg) {
    case 0: X[0] = t; X[1] = 0; C[0] = 1 - t; C[1] = 1; break;
    case 1: X[0] = 1 - t; X[1] = t; C[0] = t; C[1] = 1 - t; break;
    case 2: X[0] = t; X[1] = 1; C[0] = 1 - t; C[1] = -t; break;
    default: X[0] = 1; X[1] = 1; C[0] = -t; C[1] = -1; break;
  }
  for (int j = 0; j < 2; ++j) {
    const float d = j ? 3.f : 0.f;
    const float base = 55.f - 18.f * X[j];
    const float peak_y = 55.f - 18.f * (C[j] > 0 ? 1.f : 1.f + C[j]);
    svg::stroke(img, {P(14, base), P(22 + d, base), P(35 + d, peak_y), P(48 + d, base), P(56, base)}, 1.5f * sc,
                j ? k.env2 : k.env1);
  }

  // ---- scope of the newest voice (0x01980DAC .. 0x01980EEE): 62 points, frozen while nothing sounds
  if (iv.voices_active) {
    for (int i = 0; i < 62; ++i)  // ASSUMED: the mixed output at stride 5 (firmware: the voice, every 32 samples)
      g_ds_scope.v[i] = float(c.s.scope[std::min(kDisplayW - 1, i * 5)]) / 2147483648.f;
  }
  svg::Points pts;
  for (int i = 0; i < 62; ++i) pts.push_back(P(193.f + 2.f * float(i), 130.f - 15.f * std::clamp(g_ds_scope.v[i], -1.f, 1.f)));
  svg::stroke(img, pts, 1.5f * sc, k.env2);
}

OP1_SCREEN("dsynth", bind_dsynth, draw_dsynth);

}  // namespace
}  // namespace op1
