// Synth engine screen "fm" (moved from bind_engines1.cpp by t3; one file per firmware screen template).
#include "../../dsp/fm_algos.h"
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// =====================================================================================================
// fm (fm.svg). Decoded from the firmware's fm screen class (t3, 2026-10-23; full notes in SPEC-fm.md):
//   vtable 0x0190FFDC: +0x24 init 0x019A4A3C, +0x1C per-frame draw 0x019A4D34, +0x0C dtor 0x019A4CF0.
// init (0x019A4A3C):
//   - opa..oph (0x018DC600..61C) are replaced by centred text objects (0x01A3B5D0 align 1) that take the
//     glyph template's place, height and colour: opa..opd small (one per operator colour blue, green, white,
//     red), ope..oph large. opa = " ", ope = "b" (constant, 0x018DC620 / 624).
//   - box%i / out%i (i = 0..3) are the cube templates drawn around the origin; "out" gives the colour entry
//     of the cube outline.
//   - the 36 cubes op%i%i of groups A..I are measured (centre of their bounds) and removed: they are only
//     the target positions of the four cubes in the nine layouts.
//   - the four current cube positions are zeroed.
// draw (0x019A4D34), reads only the four knob floats p[0..3] and the frame time:
//   layout L = int(p[2] * 8 + 0.5)                                   (0x019A4D72, rounds)
//   k = min(10 * dt, 0.5); for each cube i: pos += k * (centre(op<L><i>) + (p[3] * off[i], 0) - pos),
//     off = {5, -7, 13, -15} px (L1 0xFF907020 -> 0x01D05B24)
//   outline colour = palette blue * (0.25 + 0.75 * p[0])             (0x019A4F34..0x019A5026)
//   per cube i: draw box<i>, out<i>, small text op[a+i], large text op[e+i], each translated to pos[i]
//   code = int(p[1] * 4095 + 0.5); A = code >> 9 & 7, B = code >> 6 & 7, C = code >> 3 & 7
//     opb / opf = fraction / integer string of A (green), opc / opg of B (white), opd / oph of C (red):
//     the fraction tables have "" where the integer tables have the number, so each slot shows one of them.
// Not drawn by the firmware: no voice / envelope input, no value pop-ups, the three bright "out" paths
// (authored empty, never set).
// =====================================================================================================

constexpr const char* kFmGroups[9] = {"A", "B", "C", "D", "E", "F", "G", "H", "I"};
constexpr float kFmOrangeOff[4] = {5.f, -7.f, 13.f, -15.f};  // decoded (0xFF907020)

// The firmware rounds (+0.5f before its float->int at 0x01AA4DBC); fm_algos.h's fm_layout/fm_code truncate.
int fm_layout_fw(const InstrumentView& iv) { return std::clamp(int(knob(iv, 2) * 8.f + 0.5f), 0, 8); }
int fm_code_fw(const InstrumentView& iv) { return std::clamp(int(knob(iv, 1) * 4095.f + 0.5f), 0, 4095); }

// Splits a label into the firmware's two tables: fraction / decimal strings go to the small slot, whole
// numbers to the large one (0x018DC64C/674, 69C/6C4, 6EC/714).
bool is_whole(const char* s) {
  for (; *s; ++s)
    if (*s < '0' || *s > '9') return false;
  return true;
}

struct FmState {
  float x[4] = {}, y[4] = {};  // current cube centres (document units)
  double last_t = -1e9;
};
FmState g_fm;

void bind_fm(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  FmState& st = g_fm;
  // init: the screen (re)opens -> cubes start at the origin and glide in (ASSUMED: init runs on open).
  double dt = c.t - st.last_t;
  if (dt > 0.5 || dt < 0) {
    st = FmState{};
    dt = 1.0 / 60.0;
  }
  st.last_t = c.t;

  for (const char* g : kFmGroups) show(c.n(g), false);  // position markers only
  for (const char* g : {"opa", "opb", "opc", "opd", "ope", "opf", "opg", "oph"}) show(c.n(g), false);  // text: draw()

  const int L = fm_layout_fw(iv);
  const float k = std::min(10.f * float(dt), 0.5f);
  const float orange = knob(iv, 3);
  const uint32_t outline = mix_rgb(0, kColBlue, 0.25f + 0.75f * knob(iv, 0));
  for (int i = 0; i < 4; ++i) {
    char id[8];
    std::snprintf(id, sizeof id, "op%d%d", L, i);
    const svg::Node* op = c.n(id);
    if (op) {
      const svg::Box b = c.doc.bounds(op);
      st.x[i] += k * (b.cx() + orange * kFmOrangeOff[i] - st.x[i]);
      st.y[i] += k * (b.cy() - st.y[i]);
    }
    svg::Node* box = c.n("box" + std::to_string(i));
    svg::Node* out = c.n("out" + std::to_string(i));
    show(box, true);
    show(out, true);
    move(box, st.x[i], st.y[i]);
    move(out, st.x[i], st.y[i]);
    if (out)
      for (svg::Node& p : out->kids)
        if (p.tag == "polyline") tint(&p, outline);  // the "out" colour entry (inferred: the outline)
  }
}

// Draws a label with the opfont, centred on cx ('.' advances by its glyph's metric box, svg.cpp Font::draw).
void draw_label(const svg::Font& f, svg::Image& img, float cx, float top, float h, const std::string& str,
                uint32_t col) {
  static svg::Image scratch(1, 1);
  constexpr float lw = 1.5f;
  const float w = f.draw(scratch, -1e4f, -1e4f, h, str, col, lw);
  f.draw(img, cx - 0.5f * w, top, h, str, col, lw);
}

void draw_fm(BindCtx& c, svg::Image& img) {
  if (!c.font.ok()) return;
  const InstrumentView& iv = c.inst();
  const FmState& st = g_fm;
  const int code = fm_code_fw(iv);
  const char* lab[4] = {"b", dsp::kFmLabelA[(code >> 9) & 7], dsp::kFmLabelB[(code >> 6) & 7],
                        dsp::kFmLabelC[(code >> 3) & 7]};
  const float s = c.doc.px_scale(img.w, img.h);
  static const char* const kSmall[4] = {"opa", "opb", "opc", "opd"};
  static const char* const kLarge[4] = {"ope", "opf", "opg", "oph"};
  for (int i = 0; i < 4; ++i) {
    // cube 0 (blue): small " ", large "b"; cubes 1..3: one of the two slots holds the label
    const bool whole = i == 0 || is_whole(lab[i]);
    const svg::Node* slot = c.n(whole ? kLarge[i] : kSmall[i]);
    if (!slot) continue;
    const svg::Box b = c.doc.bounds(slot);
    uint32_t col = 0;
    for (const svg::Node& p : slot->kids)
      if ((col = colour_of(&p))) break;
    const float h = b.h() * s;
    draw_label(c.font, img, (st.x[i] + b.cx()) * s, (st.y[i] + b.y0) * s, h, lab[i], col);
  }
}

OP1_SCREEN("fm", bind_fm, draw_fm);

}  // namespace
}  // namespace op1
