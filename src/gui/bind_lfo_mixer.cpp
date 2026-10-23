// LFO screens (duallfo, singlelfo, rndlfo, reroutelfo, midilfo, bendlfo, cranklfo) and the mixer screen
// (mixer.svg). Element map and evidence: scratchpad notes bindings/lfo_mixer.md (to be folded into
// docs/ui-bindings.md).
//
// Firmware evidence: each "<name>.svg" string in the binary sits next to the element names its screen class
// asks for, and the Single/Rnd/ReRoute/Midi/Bend/Crank screens assert "lfo single dest error"
// (..\src\gui\screens\SingleLfoScreen.cpp:230, RndLfoScreen.cpp:152, ReRouteLfoScreen.cpp:197,
// MidiLfoScreen.cpp:163, BendLfoScreen.cpp:139, CrankLfoScreen.cpp:139): the destination is drawn as one
// icon (synth / drum / adsr / dynaenv / fx / other) plus one parameter dot. Engine meaning of every slot:
// the header comment of src/dsp/lfo_*.cpp (params and lfo_display).
//
// Common to the LFO screens (ASSUMED where the firmware is not decoded):
//   - the "*anim" frames light up in their encoder's colour while that encoder turns;
//   - the destination icon follows the group (engine -> synth/drum by side, envelope -> adsr/dynaenv,
//     fx -> fx, main -> other: note = pitch, speaker = volume); the parameter is the ring over the coloured
//     dot of the encoder that owns it (bright dots = blue/green/white/orange page, dim dots = shift page);
//   - two-digit amounts are |value| * 99 / 32767 in the "%02i" slots; the "neg*" variant shows a negative
//     value;
//   - with the LFO off (shift+T4) the screen is drawn dimmed and its animation stands still (the engine
//     does not run the LFO then).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bind.h"

namespace op1 {
namespace {

// ------------------------------------------------------------------------------------------ helpers
// Encoder colours as the dots of the "P.DEST" pickers use them: bright = params 0..3, dim = params 4..7.
constexpr uint32_t kDotCol[8] = {0x698EFF, 0x00ED95, 0xFFFFFF, 0xFF3A5D, 0x383572, 0x00475B, 0x656579, 0x512132};
constexpr uint32_t kEncCol[4] = {kColBlue, kColGreen, kColWhite, kColRed};
constexpr int kGroupSize[4] = {8, 4, 4, 2};  // engine, envelope, fx, main (lfo_common.cpp target_of)

int choose(int v, int count) {  // dsp_util.h choose(): equal bins over 0..32767
  const int u = std::clamp(v, 0, 32767);
  return std::min(count - 1, (u * count) >> 15);
}
struct Dest { int group = 0, index = 0; bool off = false; };
Dest target_of(int group_v, int param_v) {
  Dest d;
  d.group = choose(group_v, 4);
  d.index = choose(param_v, kGroupSize[d.group]);
  return d;
}
Dest flat(int i) {  // flat target 0..17 (lfo_common.cpp flat_target)
  Dest d;
  if (i < 0) { d.off = true; return d; }
  if (i < 8) { d.group = 0; d.index = i; }
  else if (i < 12) { d.group = 1; d.index = i - 8; }
  else if (i < 16) { d.group = 2; d.index = i - 12; }
  else { d.group = 3; d.index = std::min(1, i - 16); }
  return d;
}
Dest flat_of(int v) { return flat(choose(v, 18)); }

uint32_t colour_of(const svg::Node* n, const char* attr) {
  const std::string* s = n ? n->get(attr) : nullptr;
  if (!s || s->size() < 7 || (*s)[0] != '#') return 0xFFFFFFFFu;
  return uint32_t(std::strtoul(s->c_str() + 1, nullptr, 16));
}
// The dot (filled circle) of a colour among the descendants of n.
svg::Node* dot_of(svg::Node* n, uint32_t rgb) {
  if (!n) return nullptr;
  if (n->tag == "circle" && colour_of(n, "fill") == rgb) return n;
  for (svg::Node& k : n->kids)
    if (svg::Node* r = dot_of(&k, rgb)) return r;
  return nullptr;
}
// The selection ring (an unnamed circle of radius 6.289 drawn around the chosen dot).
svg::Node* ring_in(svg::Node* n) {
  if (!n) return nullptr;
  if (n->tag == "circle" && n->get("r") && std::fabs(std::atof(n->get("r")->c_str()) - 6.289f) < 0.01f) return n;
  for (svg::Node& k : n->kids)
    if (svg::Node* r = ring_in(&k)) return r;
  return nullptr;
}
// tint() on a group does not reach children that carry their own colour: tint the whole subtree.
void tint_deep(svg::Node* n, uint32_t rgb) {
  if (!n) return;
  tint(n, rgb);
  for (svg::Node& k : n->kids) tint_deep(&k, rgb);
}
svg::Node* root_of(BindCtx& c) {
  for (const char* id : {"Layer_1", "screen"})
    if (svg::Node* n = c.n(id)) return n;
  return nullptr;
}

// Moves the selection ring over the dot of parameter `index` of a group of `size` parameters, and dims the
// dots the group does not have. `scopes` hold the dots.
void pick_dot(BindCtx& c, std::initializer_list<svg::Node*> scopes, svg::Node* ring, const Dest& d) {
  auto find_dot = [&](uint32_t rgb) -> svg::Node* {
    for (svg::Node* s : scopes)
      if (svg::Node* r = dot_of(s, rgb)) return r;
    return nullptr;
  };
  const int size = d.off ? 0 : kGroupSize[d.group];
  for (int i = 0; i < 8; ++i) {
    svg::Node* dot = find_dot(kDotCol[i]);
    if (!dot) continue;
    // the main group (pitch, volume) uses the blue and green dots
    show(dot, i < size);
  }
  if (!ring) return;
  show(ring, !d.off);
  if (d.off) return;
  svg::Node* dot = find_dot(kDotCol[d.index & 7]);
  if (!dot) return;
  const svg::Box a = c.doc.bounds(ring), b = c.doc.bounds(dot);
  move(ring, b.cx() - a.cx(), b.cy() - a.cy());
  tint(ring, 0xADADFF);
}

// Shows the destination icon of a group. `sfx` is the per-column suffix of the midi screen ("" elsewhere).
void dest_icon(BindCtx& c, const Dest& d, const std::string& sfx = "") {
  const bool drum = c.side() == 1;
  const char* names[] = {"synth", "drum", "adsr", "dynaenv", "fx", "other"};
  int pick = -1;
  if (!d.off) pick = d.group == 0 ? (drum ? 1 : 0) : d.group == 1 ? (drum ? 3 : 2) : d.group == 2 ? 4 : 5;
  for (int i = 0; i < 6; ++i) {
    std::string id = names[i] + sfx;
    for (svg::Node* n : c.all(id)) show(n, i == pick);
    if (svg::Node* n = c.n(id)) show(n, i == pick);
  }
  // other = two icons: a note (pitch) and a speaker (volume)
  if (pick == 5)
    if (svg::Node* o = c.n("other" + sfx); o && o->kids.size() >= 2) {
      show(&o->kids[0], d.index == 0);
      show(&o->kids[1], d.index != 0);
    }
  // the "f" variants of the single LFO (synthf, drumf, adsrf, dynaenvf, fxf) are not used (ASSUMED)
  for (const char* f : {"synthf", "drumf", "adsrf", "dynaenvf", "fxf"}) show(c.n(f), false);
}

// A signed Q15 amount as "%02i" in the positive group, or in the "neg" group when below zero.
void amount(BindCtx& c, const char* pos_group, const char* pos_slots, const char* neg_group, const char* neg_slots, int v) {
  const bool neg = v < 0;
  char t[4];
  std::snprintf(t, sizeof t, "%02d", std::min(99, (std::abs(v) * 99 + 16383) / 32767));
  show(c.n(pos_group), !neg);
  show(c.n(neg_group), neg);
  svg::Node* g = c.n(neg ? neg_slots : pos_slots);
  if (g) text_into(c, slots_lr(c, g), t);
}

// Encoder feedback: the frame of a section lights up in its encoder's colour while it turns.
void flash(BindCtx& c, const char* id, int enc, bool shift_page = false) {
  svg::Node* n = c.n(id);
  if (!n) return;
  if (since_enc(c, enc) < 0.6 && c.ui.shift == shift_page) tint_deep(n, kEncCol[enc & 3]);
}

// LFO off: everything dimmed (ASSUMED).
void power(BindCtx& c) {
  if (!c.inst().lfo_on)
    if (svg::Node* r = root_of(c)) r->alpha = 0.45f;
}

float phase01(int32_t q16) { return float(uint32_t(q16) & 0xFFFF) / 65536.f; }

// ------------------------------------------------------------------------------------------ duallfo
// duallfo.svg (LfoDual.cpp = tremolo, lfo_tremolo.cpp). Strings next to "duallfo.svg": bluebase green white
// neggreen negwhite negg negw envstart envend trem%i blueanim redanim redanimshift speeda speedb blip blue
// green white center. Params: blue speed, green pitch depth (signed), white volume depth (signed), orange
// fade-in. lfo_display: [0] phase Q16, [1] value Q15, [2] pitch (st*256), [3] volume gain dip Q15, [4] fade.
void bind_duallfo(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const int32_t* d = iv.lfo_display;
  power(c);
  // SPEED: the clock hand turns with the LFO's phase (speeda, an alternative hand, is hidden)
  const svg::Box bb = c.box("bluebase");
  rotate(c, c.n("blue"), phase01(d[0]) * 360.f, bb.cx(), bb.cy());
  show(c.n("speeda"), false);
  flash(c, "blueanim", 0);
  // pitch depth (upper triangle) and volume depth (lower triangle)
  amount(c, "green", "g", "neggreen", "negg", iv.lfo[1]);
  amount(c, "white", "w", "negwhite", "negw", iv.lfo[2]);
  // the note icon bobs with the pitch modulation (d[2]: semitones * 256, +-512)
  move(c.n("vib"), 0, -float(d[2]) / 512.f * 4.f);
  // the speaker waves: three at full volume, fewer while the tremolo dips (d[3] <= 0)
  const float gain = 1.f + float(std::min<int32_t>(0, d[3])) / 32767.f;
  const int waves = iv.lfo[2] == 0 ? 3 : std::clamp(int(std::lround(gain * 3.f)), 0, 3);
  one_of(c, {"trem0"}, waves > 0 ? 0 : -1);
  one_of(c, {"trem1"}, waves > 1 ? 0 : -1);
  one_of(c, {"trem2"}, waves > 2 ? 0 : -1);
  // ENV: the fade-in ramp, vertical at 0 and stretched to the right end at full (orange, p3)
  const float fade = q15(iv.lfo[3]);
  const float x = lerp(249.607f, 284.588f, fade);
  set_line(c.n("env_1_"), 249.607f, 54.494f, x, 22.157f);
  move(c.n("envend"), x - 284.588f, 0);
  flash(c, "redanim", 3);
  // the shape box: the tremolo is a sine (lfo_tremolo.cpp); the other shapes are hidden
  one_of(c, {"sin", "saw", "sqr", "exp", "blip"}, 0);
  flash(c, "redanimshift", 3, true);
}

// ------------------------------------------------------------------------------------------ singlelfo
// singlelfo.svg (LfoSingle.cpp = value, lfo_value.cpp). Strings next to "singlelfo.svg": hold green neggreen
// negg blue reda redb synth drum adsr dynaenv synthf drumf adsrf dynaenvf speeda speedb bluebase redbase
// blueanim greenanim whiteanim redanim graphstart graphend "%02i" "%01i". Params: blue speed, green amount
// (signed), white destination group, orange parameter; shift: fine, shape, sync, phase.
// lfo_display: [0] phase Q16, [1] value Q15, [4] shape 0..3, [5] sync 0..2.
void bind_singlelfo(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const int32_t* d = iv.lfo_display;
  power(c);
  const svg::Box bb = c.box("bluebase");
  rotate(c, c.n("blue"), phase01(d[0]) * 360.f, bb.cx(), bb.cy());
  show(c.n("speeda"), false);
  flash(c, "blueanim", 0);
  amount(c, "green", "g", "neggreen", "negg", iv.lfo[1]);
  const Dest dst = target_of(iv.lfo[2], iv.lfo[3]);
  dest_icon(c, dst);
  flash(c, "whiteanim", 2);
  pick_dot(c, {c.n("reda"), c.n("redb")}, ring_in(c.n("static")), dst);
  flash(c, "redanim", 3);
  // sync (shift + white): the loop above the clock lights its left end for NOTE, right end for TEMPO (ASSUMED)
  const int sync = choose(iv.lfo[6], 3);
  tint(c.n("graphstart"), sync == 1 ? kColBlue : 0x383572);
  tint(c.n("graphend"), sync == 2 ? kColBlue : 0x383572);
}

// The shape of the value LFO, drawn by code inside the loop above the clock, with the phase as a dot.
float shape_at(int shape, float ph) {
  switch (shape) {
    case 0: return ph < 0.5f ? 1.f : -1.f;
    case 1: return ph * 2.f - 1.f;
    case 2: return 1.f - ph * 2.f;
    default: return std::sin(ph * 6.2831853f);
  }
}
void draw_singlelfo(BindCtx& c, svg::Image& img) {
  const InstrumentView& iv = c.inst();
  const int shape = choose(iv.lfo[5], 4);
  const float x0 = 31.f, w = 18.f, cy = 24.3f, h = 6.f;
  svg::Points pts;
  for (int i = 0; i <= 36; ++i) {
    const float ph = float(i) / 36.f;
    pts.emplace_back(x0 + w * ph, cy - h * shape_at(shape, ph));
  }
  const uint32_t col = iv.lfo_on ? kColBlue : kColDim;
  svg::stroke(img, pts, 1.2f, col);
  const float ph = phase01(iv.lfo_display[0]);
  svg::circle(img, x0 + w * ph, cy - h * shape_at(shape, ph), 1.6f, iv.lfo_on ? kColWhite : kColDim, true);
}

// ------------------------------------------------------------------------------------------ rndlfo
// rndlfo.svg (LfoRnd.cpp = random, lfo_random.cpp). Strings next to "rndlfo.svg": blue greenline greendot
// envstart envend synth drum adsr dynaenv blueanim whiteanim redanim graphstart graphend "%02i".
// The template puts AMOUNT on blue and SPEED on green; the engine has blue = speed, green = amount. Until
// the engine follows the template (Request in the notes) the two sections are re-tinted so each shows the
// colour of the encoder that really moves it.
// lfo_display: [0] phase Q16, [1] smoothed value Q15, [3] flat target, [4] applied amount.
constexpr bool kRndSwapTint = true;
constexpr int kRndSpeed = 0, kRndAmount = 1;
float g_rnd_hist[72] = {};
int g_rnd_head = 0;
double g_rnd_t = -1;

void bind_rndlfo(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  power(c);
  amount(c, "blue", "blue", "", "", iv.lfo[kRndAmount]);
  const uint32_t amount_col = kRndSwapTint ? kColGreen : kColBlue;
  tint_deep(c.n("blue"), amount_col);
  if (since_enc(c, kRndAmount) < 0.6) tint_deep(c.n("blueanim"), amount_col);
  // SPEED: the graph between graphstart and graphend is drawn by code; the dot rides its right end
  show(c.n("greenline"), false);
  const float v = float(iv.lfo_display[1]) / 32767.f;
  const svg::Box gd = c.box("greendot"), ge = c.box("graphend");
  move(c.n("greendot"), ge.cx() - 3.f - gd.cx(), 75.21f - v * 28.f - gd.cy());
  if (kRndSwapTint) for (const char* id : {"greendot", "graphstart", "graphend"}) tint(c.n(id), kColBlue);
  // DEST (white): flat target; the four dots by "ALL" show the parameter's encoder colour
  const Dest dst = flat_of(iv.lfo[2]);
  dest_icon(c, dst);
  flash(c, "whiteanim", 2);
  if (svg::Node* st = c.n("static")) {
    const uint32_t cols[4] = {0x698EFF, 0x00ED95, 0xDFD9FF, 0xFF3A5D};
    const int want = dst.group == 3 ? dst.index : (dst.index & 3);
    for (int i = 0; i < 4; ++i)
      for (svg::Node& k : st->kids)
        if (k.tag == "circle" && colour_of(&k, "fill") == cols[i]) tint(&k, i == want ? cols[i] : kColDim);
  }
  // ENV (orange): flat at 0 (constant depth), rising from the bottom at full (depth follows the envelope)
  const float env = q15(iv.lfo[3]);
  const float y0 = lerp(97.607f, 123.639f, env);
  set_line(c.n("env_1_"), 191.115f, y0, 219.177f, 97.607f);
  move(c.n("envstart"), 0, y0 - 97.845f);
  flash(c, "redanim", 3);
}

void draw_rndlfo(BindCtx& c, svg::Image& img) {
  const InstrumentView& iv = c.inst();
  // history of the smoothed value, one sample per 1/30 s (UI clock), scrolling left
  if (iv.lfo_on && (g_rnd_t < 0 || c.t - g_rnd_t >= 1.0 / 30.0 || c.t < g_rnd_t)) {
    g_rnd_t = c.t;
    g_rnd_hist[g_rnd_head] = float(iv.lfo_display[1]) / 32767.f;
    g_rnd_head = (g_rnd_head + 1) % 72;
  }
  const float x0 = 89.4f, x1 = 154.f;
  svg::Points pts;
  for (int i = 0; i < 72; ++i) {
    const float v = g_rnd_hist[(g_rnd_head + i) % 72];
    pts.emplace_back(x0 + (x1 - x0) * float(i) / 71.f, 75.21f - v * 28.f);
  }
  svg::stroke(img, pts, 1.3f, iv.lfo_on ? (kRndSwapTint ? kColBlue : kColGreen) : kColDim);
}

// ------------------------------------------------------------------------------------------ reroutelfo
// reroutelfo.svg (LfoReRoute.cpp = element, lfo_element.cpp). Strings next to "reroutelfo.svg": gforce input
// adsrin otherin green neggreen negg speed source reda redb synth drum adsr dynaenv other redbase blueanim
// greenanim whiteanim redanim "%02i". Params: blue source, green amount (signed), white group, orange
// parameter. lfo_display: [0] source, [2] smoothed source value Q15.
void bind_reroutelfo(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const int32_t* d = iv.lfo_display;
  power(c);
  const int src = choose(iv.lfo[0], 4);
  one_of(c, {"gforce", "input", "adsrin", "otherin"}, src);
  // the source icon glows with the signal it delivers (ASSUMED)
  const char* sid[] = {"gforce", "input", "adsrin", "otherin"};
  if (svg::Node* s = c.n(sid[src])) s->alpha = iv.lfo_on ? 0.45f + 0.55f * clamp01(std::fabs(float(d[2])) / 32767.f * 2.f) : 1.f;
  flash(c, "blueanim", 0);
  amount(c, "green", "g", "neggreen", "negg", iv.lfo[1]);
  const Dest dst = target_of(iv.lfo[2], iv.lfo[3]);
  dest_icon(c, dst);
  flash(c, "whiteanim", 2);
  pick_dot(c, {c.n("reda"), c.n("redb")}, ring_in(c.n("static")), dst);
  flash(c, "redanim", 3);
}

// ------------------------------------------------------------------------------------------ midilfo
// midilfo.svg (LfoMidi.cpp, lfo_midi.cpp). Strings next to "midilfo.svg": pin%i synth%i drum%i adsr%i
// dynaenv%i fx%i other%i base%i squareanim%i squareshift%i. One column per CC 1..4 (blue, green, white,
// orange): the icon is the destination group, the needle (pin) points at the parameter's dot on the gauge.
// lfo_display: [0..3] destination (-1 off, flat 0..17), [4..7] CC value 0..127.
void bind_midilfo(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const int32_t* d = iv.lfo_display;
  power(c);
  svg::Node* st = c.n("static");
  for (int k = 0; k < 4; ++k) {
    const std::string s = std::to_string(k);
    const Dest dst = flat(choose(iv.lfo[k], 19) - 1);
    dest_icon(c, dst, s);
    // needle: gauge dots left to right are params 0,4,1,5,2,6,3,7 (bright, then dim, per encoder colour)
    svg::Node* pin = c.n("pin" + s);
    svg::Node* base = c.n("base" + s);
    if (pin && base) {
      const svg::Box b = c.doc.bounds(base);
      float deg = 0;
      if (!dst.off) {
        const int i = dst.index & 7;
        const int slot = i < 4 ? 2 * i : 2 * (i - 4) + 1;
        svg::Node* gauge = st && int(st->kids.size()) > k ? &st->kids[size_t(k)] : nullptr;
        if (svg::Node* dot = dot_of(gauge, kDotCol[i])) {
          const svg::Box db = c.doc.bounds(dot);
          deg = std::atan2(db.cy() - b.cy(), db.cx() - b.cx()) * 57.29578f - 180.f;
        } else {
          deg = float(slot) * 180.f / 7.f;
        }
      }
      rotate(c, pin, deg, b.cx(), b.cy());
      if (dst.off) { tint(pin, kColDim); tint(base, kColDim); }
    }
    // the CC box: dim when the CC goes nowhere; the dot under it lights with the incoming CC value
    if (dst.off) tint(c.n("squareanim" + s), kColDim);
    // squareshift%i: a black mask (stroke-width 18) over the icon area; it covers the icon only while the
    // column is OFF (ASSUMED)
    show(c.n("squareshift" + s), dst.off);
    if (svg::Node* v = c.n("v" + s)) v->alpha = 0.3f + 0.7f * clamp01(float(d[4 + k]) / 127.f);
  }
}

// ------------------------------------------------------------------------------------------ bendlfo
// bendlfo.svg (LfoBend.cpp, lfo_bend.cpp). Strings next to "bendlfo.svg": dots bend synth drum adsr dynaenv
// other upper_stop lower_stop white negwhite rcenter "%02i". Params: blue group, green parameter, white
// amount, orange the bender, shift+orange direction. lfo_display: [0] bend Q15, [1] direction +1/-1.
void bind_bendlfo(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const int32_t* d = iv.lfo_display;
  power(c);
  const Dest dst = target_of(iv.lfo[0], iv.lfo[1]);
  dest_icon(c, dst);
  pick_dot(c, {c.n("dots")}, ring_in(c.n("Layer_1")), dst);
  // amount: one digit 0..9 (white), the "neg" glyph when the direction is DOWN (ASSUMED)
  const int dir = iv.lfo[7] >= 16384 ? -1 : 1;
  char t[2] = {char('0' + std::min(9, (std::clamp<int>(iv.lfo[2], 0, 32767) * 9 + 16383) / 32767)), 0};
  show(c.n("white"), dir > 0);
  show(c.n("negwhite"), dir < 0);
  if (dir > 0) digits(c, {"white"}, t);
  else if (svg::Node* n = c.n("negwhite")) text_into(c, slots_lr(c, n), t);
  // the lever turns about its hub between the two stops (about 12 degrees each way)
  const svg::Box rc = c.box("rcenter");
  const float bend = clamp01(float(d[0]) / 32767.f) * float(d[1] ? d[1] : dir);
  rotate(c, c.n("bend"), bend * 12.f, rc.cx(), rc.cy());
}

// ------------------------------------------------------------------------------------------ cranklfo
// cranklfo.svg (LfoCrank.cpp, lfo_crank.cpp). Strings next to "cranklfo.svg": synth drum adsr dynaenv other
// crank wheel rubber ccenter green "%02i". Params: blue the crank, green amount, white destination (flat).
// lfo_display: [0] phase Q16, [1] speed Hz Q16, [3] depth Q15.
void bind_cranklfo(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const int32_t* d = iv.lfo_display;
  power(c);
  // the handle turns with the blue encoder (the crank), about the small pulley's centre
  rotate(c, c.n("crank"), q15(iv.lfo[0]) * 360.f, 80.7f, 80.852f);
  // the wheel turns with the LFO's phase
  rotate(c, c.n("wheel"), phase01(d[0]) * 360.f, 185.273f, 80.729f);
  // the belt glows with the depth the cranking produces (ASSUMED)
  if (svg::Node* r = c.n("rubber"); r && iv.lfo_on) r->alpha = 0.4f + 0.6f * clamp01(float(d[3]) / 32767.f);
  amount(c, "green", "g", "", "", iv.lfo[1]);
  dest_icon(c, flat_of(iv.lfo[2]));
}

// ------------------------------------------------------------------------------------------ mixer
// mixer.svg (mixer screen, key 4). Strings next to "mixer.svg": lev%i pan%i pangroup panleftright vu%i led%i
// "%02i". Per track t (columns 1..4 = blue, green, white, orange):
//   lev%i   the level as two digits (volume * 99 / 32767)
//   vu%i    the needle over the bow: post-fader peak of the track's playback (TapeView::level x volume);
//           the selected track also meters the live instrument while the tape is stopped (ASSUMED: the
//           armed track monitors the input, as on the tape screen)
//   led%i   the clip LED (peak at full scale, held 0.4 s) (ASSUMED)
//   pan%i   the pan needle in its ring, +-90 degrees = full L / R (the L and R letters sit at 9 and 3 o'clock)
//   pangroup / panleftright / panmeters: lit while shift (pan) is held or a pan just changed (ASSUMED)
float g_vu[4] = {};
double g_vu_t = -1, g_clip_t[4] = {-1e9, -1e9, -1e9, -1e9};

float meter_norm(float peak) {  // 0..1 over -48 .. 0 dBFS (ASSUMED scale)
  if (peak <= 1e-5f) return 0.f;
  return clamp01((20.f * std::log10(peak) + 48.f) / 48.f);
}

void bind_mixer(BindCtx& c) {
  const MixerView& m = c.s.mixer;
  const TapeView& tv = c.s.tape;
  const double dt = g_vu_t < 0 || c.t < g_vu_t ? 0.0 : std::min(0.25, c.t - g_vu_t);
  g_vu_t = c.t;
  const float live = float(std::max(m.meter_l, m.meter_r)) / 32767.f;
  const bool pan_lit = c.ui.shift;
  for (int t = 0; t < 4; ++t) {
    const std::string s = std::to_string(t + 1);
    const float vol = q15(m.volume[t]);
    // level digits
    char txt[4];
    std::snprintf(txt, sizeof txt, "%02d", std::min(99, (std::clamp<int>(m.volume[t], 0, 32767) * 99 + 16383) / 32767));
    if (svg::Node* g = c.n("lev" + s)) text_into(c, slots_lr(c, g), txt);
    // VU needle with a fast rise and a 24 dB/s fall
    float peak = float(std::abs(int(tv.level[t]))) / 32767.f * vol;
    if (t == (tv.track & 3) && !tv.playing) peak = std::max(peak, live);
    const float target = meter_norm(peak);
    g_vu[t] = target > g_vu[t] ? target : std::max(target, g_vu[t] - float(dt) * 0.5f);
    const svg::Box dot = c.box("viftdot" + s);
    rotate(c, c.n("vu" + s), -90.f + 180.f * g_vu[t], dot.cx(), dot.cy());
    if (peak >= 0.98f) g_clip_t[t] = c.t;
    show(c.n("led" + s), c.t - g_clip_t[t] < 0.4);
    // pan needle
    const svg::Box ring = c.box("panring" + s);
    rotate(c, c.n("pan" + s), float(m.pan[t]) / 32768.f * 90.f, ring.cx(), ring.cy());
  }
  if (pan_lit) {
    for (const char* id : {"pangroup", "panleftright", "panmeters"}) tint_deep(c.n(id), kColWhite);
  }
}

}  // namespace

OP1_SCREEN("duallfo", bind_duallfo, nullptr);
OP1_SCREEN("singlelfo", bind_singlelfo, draw_singlelfo);
OP1_SCREEN("rndlfo", bind_rndlfo, draw_rndlfo);
OP1_SCREEN("reroutelfo", bind_reroutelfo, nullptr);
OP1_SCREEN("midilfo", bind_midilfo, nullptr);
OP1_SCREEN("bendlfo", bind_bendlfo, nullptr);
OP1_SCREEN("cranklfo", bind_cranklfo, nullptr);
OP1_SCREEN("mixer", bind_mixer, nullptr);

}  // namespace op1
