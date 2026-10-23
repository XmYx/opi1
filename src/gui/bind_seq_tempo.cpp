// Sequencer and tempo screens (task t10, group seq_tempo): pattern, endless, tombola, ok (finger),
// sketch, etchasketch and tempo (the arpeggio's template is simple.svg, bound in bind_system.cpp). Element
// names come from the firmware's strings next to each "<name>.svg" string and its screen class asserts;
// what each element is driven from is listed per screen below and in the notes (bindings/seq_tempo.md).
// Data: SeqView::display (seq_engines.h layout, side 0 = 0..31, side 1 = 32..63), the sequencer parameters
// the UI sent (UiState::seq_param, kept per sequencer type here, see SeqParams), TempoView, TapeView and
// the instruments' voice slots.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "bind.h"

namespace op1 {
namespace {

constexpr int kPattern = 0, kEndless = 1, kTombola = 2, kFinger = 3, kSketch = 4, kArp = 5;

// ---------------------------------------------------------------- shared helpers

// UiState::seq_param[side][index] holds the last value the UI sent for an index, whatever the type. The
// screens need it per type, so this keeps a per-type copy: a value that changed since the last frame
// belongs to the type on screen now (the encoders only send on the sequencer page, which shows it).
// Defaults: the UI's starting values (ui_model.cpp kSeqDefaults).
struct SeqParams {
  int v[2][6][16];
  int last[2][16] = {};
  SeqParams() {
    static const int def[6][16] = {
        {16, 2, 50, 0},
        {24, 2, 0, 0, 50, 0},
        {20000, 870, 0},
        {2, 50, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16},
        {16, 24, 0},
        {0, 0, 48, 50, 12, 0, 0},
    };
    for (auto& side : v) std::memcpy(side, def, sizeof def);
  }
};

SeqParams& seq_params() {
  static SeqParams p;
  return p;
}

// The parameter `idx` of sequencer `type` on the screen's side.
int seqp(const BindCtx& c, int type, int idx) {
  SeqParams& p = seq_params();
  const int side = c.side();
  for (int i = 0; i < 16; ++i)
    if (c.ui.seq_param[side][i] != p.last[side][i]) {
      p.last[side][i] = c.ui.seq_param[side][i];
      p.v[side][type][i] = c.ui.seq_param[side][i];
    }
  return p.v[side][type % 6][idx & 15];
}

// The side's sequencer view when the sequencer on screen is the selected type, else an idle view of that
// type (browse mode shows any template whatever the side has selected).
const int32_t* view_of(const BindCtx& c, int type) {
  const int32_t* d = c.seq_display();
  if (d[1] == type) return d;
  static int32_t idle[6][32];
  int32_t* v = idle[type % 6];
  std::fill(v, v + 32, 0);
  v[1] = type;
  v[2] = -1;
  if (type == kFinger) v[4] = v[5] = -1;
  if (type == kSketch) std::fill(v + 6, v + 30, -1);
  return v;
}

// Timing.cpp names of the tick lengths (96 PPQN): "1/8t", "1/16", "1/16t", "1/32", "1/32t" are in the binary.
const char* timing_name(int ticks) {
  switch (ticks) {
    case 96: return "1/4";
    case 48: return "1/8";
    case 32: return "1/8t";
    case 24: return "1/16";
    case 16: return "1/16t";
    case 12: return "1/32";
    case 8: return "1/32t";
    default: return "1/16";
  }
}

// Writes one character into one slot (keeps the slot if the font lacks it).
// A '1' into a slot authored as a single line (an authored '1') keeps the authored stroke: those '1's
// stand at their cell's left edge, where the font's '1' would not.
void glyph(BindCtx& c, svg::Node* slot, char ch) {
  if (!slot || !c.font.ok()) return;
  if (ch == '1' && slot->tag == "line") return;
  if (!c.font.set(c.doc, slot, ch)) slot->visible = 0;
}

// Text drawn by code with the firmware font. The font's '.' is only a cell marker, so a decimal point is
// drawn as a dot. Returns the width; img == nullptr only measures.
float draw_str(const BindCtx& c, svg::Image* img, float x, float y, float h, const std::string& s, uint32_t rgb,
               float width = 1.5f) {
  static svg::Image scratch(1, 1);
  float cx = x;
  std::string run;
  auto flush = [&] {
    if (run.empty()) return;
    cx += c.font.draw(img ? *img : scratch, img ? cx : -1000.f, img ? y : -1000.f, h, run, rgb, width);
    run.clear();
  };
  for (char ch : s) {
    if (ch != '.') { run += ch; continue; }
    flush();
    if (img) svg::circle(*img, cx + width, y + h - width * 0.5f, width * 0.8f, rgb, true);
    cx += width * 2 + h * 0.15f;
  }
  flush();
  return cx - x;
}
float text_w(const BindCtx& c, const std::string& s, float h, float width = 1.5f) {
  return draw_str(c, nullptr, 0, 0, h, s, 0, width);
}

// Digit slots: a path/line slot takes the font glyph (Font::set); a group slot (a digit authored as
// several shapes, e.g. a '9' as circle + line) is hidden and its character drawn by code at its place
// (flush_slots, from the screen's draw function). Pending draws are rebuilt by every bind call.
struct PendingChar { svg::Box b; char ch; uint32_t rgb; };
std::vector<PendingChar>& pending() {
  static std::vector<PendingChar> v;
  return v;
}
void put_char(BindCtx& c, svg::Node* slot, char ch, uint32_t rgb) {
  if (!slot || !c.font.ok()) return;
  if (slot->tag == "g") {
    slot->visible = 0;
    if (ch != ' ') pending().push_back({c.doc.bounds(slot, false), ch, rgb});
    return;
  }
  glyph(c, slot, ch);
}
void text_slots(BindCtx& c, const std::vector<svg::Node*>& slots, const std::string& text, uint32_t rgb) {
  const size_t n = slots.size();
  const std::string t = text.size() > n ? text.substr(text.size() - n) : text;
  for (size_t i = 0; i < n; ++i) put_char(c, slots[i], i < n - t.size() ? ' ' : t[i - (n - t.size())], rgb);
}
void flush_slots(BindCtx& c, svg::Image& img) {
  for (const PendingChar& p : pending()) {
    const std::string s(1, p.ch);
    const float w = text_w(c, s, p.b.h());
    draw_str(c, &img, p.b.cx() - w / 2, p.b.y0, p.b.h(), s, p.rgb);
  }
}

// The two digit slots of an authored "55%" (the two leftmost children of the group), value 0..99.
void percent_digits(BindCtx& c, svg::Node* group, int v) {
  const auto slots = slots_lr(c, group);
  if (slots.size() < 2) return;
  v = std::clamp(v, 0, 99);
  glyph(c, slots[0], v >= 10 ? char('0' + v / 10) : ' ');
  glyph(c, slots[1], char('0' + v % 10));
}

// Beats elapsed, for the tempo pendulum and pulse: the tape position while the tape runs, else a free
// clock at the tempo (accumulated, so a tempo change does not jump the phase).
double beats(const BindCtx& c) {
  static double phase = 0, last_t = -1;
  const double bpm = std::max<int>(400, c.s.tempo.bpm_tenths) / 10.0;
  if (last_t >= 0 && c.t > last_t) phase += (c.t - last_t) * bpm / 60.0;
  last_t = c.t;
  if (c.s.tape.playing) return double(c.s.tape.position) * bpm / (60.0 * kSampleRate);
  return phase;
}

// ================================================================ pattern (pattern.svg; pattern_screen.cpp)
// Strings next to "pattern.svg": swing length start notes x%iy%i rubber editptr %i%% swing hold cycle;
// asserts pattern_screen.cpp:258/286 row >= 0 && row < 24. Template: dots x{step}y{key} (16 steps x 24 keys,
// sharps white), vbars v1..v16 (step columns), editptr (row pointer), swing "55%", pm group rev/fwd/cycle,
// hold, rubber.
void bind_pattern(BindCtx& c) {
  const int32_t* d = view_of(c, kPattern);
  // t5: the window is start .. start + length - 1 (view [22] start, [4] length); the playing step while it
  // plays, else the entry cursor ([21]) is lit (shift + keys toggle that step)
  const int start = std::clamp<int>(d[22], 0, 15);
  const int len = std::clamp<int>(d[4] > 0 ? d[4] : seqp(c, kPattern, 0), 1, 16);
  const int cur = d[3] && d[2] >= 0 ? d[2] : std::clamp<int>(d[21], 0, 15);
  char id[16];
  for (int s = 0; s < 16; ++s) {
    const uint32_t mask = uint32_t(d[5 + s]);
    for (int k = 0; k < 24; ++k) {
      std::snprintf(id, sizeof id, "x%dy%d", s, k);
      svg::Node* n = c.n(id);
      const bool on = s >= start && s < start + len && (mask >> k & 1u);
      show(n, on);
      if (on && s == cur) tint(n, kColGreen);
    }
    std::snprintf(id, sizeof id, "v%d", s + 1);
    svg::Node* v = c.n(id);
    show(v, s >= start && s < start + len);
    tint(v, s == cur ? kColWhite : (s % 4 == 0 ? 0x8080BD : 0x534468));
  }
  show(c.n("v16_1_"), len >= 16);
  // the key rows end at the last step
  const float xe = len >= 16 ? 242.704f : 50.988f + 12.f * float(len - 1);
  for (int h = 1; h <= 14; ++h) {
    std::snprintf(id, sizeof id, "h%d", h);
    if (svg::Node* hl = c.n(id)) {
      const float y = std::stof(*hl->get("y1"));
      set_line(hl, xe, y, 50.988f, y);
    }
  }
  // the row of the last played key (keyboard index 0..23 = note 53 + k, seq_engines.h)
  const int row = c.ui.last_note < 0 ? 23 : ((c.ui.last_note - 53) % 24 + 24) % 24;
  std::snprintf(id, sizeof id, "x0y%d", row);
  const svg::Box rb = c.box(id);
  if (!rb.empty()) move(c.n("editptr"), 0, rb.cy() - 13.5f);
  // swing (green) and play mode (shift+orange: 0 rev, 1 cycle, 2 fwd), hold = the latch (orange right)
  percent_digits(c, c.n("swing"), seqp(c, kPattern, 2));
  one_of(c, {"rev", "cycle", "fwd"}, std::clamp(seqp(c, kPattern, 1), 0, 2));
  show(c.n("hold"), d[25] != 0 || c.ui.shift);
}

// ================================================================ endless (endless.svg; EndlessScreen.cpp)
// Strings: euclidean grid e%i%i, stepcount (m_step_count_text, drawn as two digits), timing, swing, hold,
// shiftkey (the "shift + keys" hint), rnd4, crank, pingpong, playmodes (rev fwd pingpong rnd), "%i%%".
void bind_endless(BindCtx& c) {
  pending().clear();
  const int32_t* d = view_of(c, kEndless);
  const int count = std::max<int>(0, d[4]);
  const int cur = d[2];
  const bool rec = d[6] != 0;
  const int eucl = seqp(c, kEndless, 2), off = seqp(c, kEndless, 3);
  char id[8];
  for (int p = 0; p < 40; ++p) {
    std::snprintf(id, sizeof id, "e%d%d", p % 8 + 1, p / 8 + 1);
    svg::Node* n = c.n(id);
    if (p >= count) { show(n, false); continue; }
    bool hit = true;  // euclidean rests (seq_engines.cpp EndlessSeq::play)
    if (eucl > 0) {
      const int k = std::min(eucl, count);
      hit = int((int64_t(p + off) * k) % count) < k;
    }
    tint(n, p == cur ? kColGreen : (hit ? kColWhite : 0x40404D));
  }
  // step count: two digit slots (stepcount); the hint while nothing is recorded or while recording
  svg::Node* sc = c.n("stepcount");
  show(sc, count > 0);
  if (count > 0) {
    text_slots(c, slots_lr(c, sc), std::to_string(std::min(count, 99)), kColRed);
    if (rec && std::fmod(c.t, 1.0) >= 0.5) tint(sc, kColDark);
  }
  show(c.n("shiftkey"), count == 0 || rec);
  show(c.n("timing"), false);  // drawn as text in draw_endless
  show(c.n("crank"), false);
  percent_digits(c, c.n("swing"), seqp(c, kEndless, 4));
  const bool hold = c.ui.shift;
  show(c.n("hold"), hold);
  show(c.n("playmodes"), !hold);
  // t5: playmode 0 rev, 1 a shuffled bag ("rnd"), 2 fwd (F, EndlessPlayer 0x01988368); pingpong and rnd4 are
  // drawn in the template but have no player code
  {
    const int pm = std::clamp(seqp(c, kEndless, 1), 0, 2);
    one_of(c, {"rev", "rnd", "fwd", "pingpong", "rnd4"}, pm == 1 ? 1 : (pm == 0 ? 0 : 2));
  }
  show(c.n("rnd4"), false);
}

void draw_endless(BindCtx& c, svg::Image& img) {
  if (!c.font.ok()) return;
  flush_slots(c, img);
  const int32_t* d = view_of(c, kEndless);
  c.font.draw(img, 11.f, 6.5f, 26.6f, timing_name(d[5] ? d[5] : seqp(c, kEndless, 0)), kColBlue, 2.f);
}

// ================================================================ tombola (tombola.svg; TombolaScreen.cpp)
// Strings: tombola ball%i speed gravity rotation crank bouncebar bounceline; asserts gravity and
// restitution 0..1 (TombolaScreen.cpp:203/208). The hexagon turns with the physics angle, the balls sit
// where the physics has them, gravity scales the green circle, restitution lifts the bounce bar.
constexpr float kTomCx = 160.795f, kTomCy = 69.723f, kTomPxPerM = 62.696f / 5.0f;  // hexagon radius 5 m

void bind_tombola(BindCtx& c) {
  pending().clear();
  const int32_t* d = view_of(c, kTombola);
  const float ang = float(d[4]) / 1000.f;  // milli-rad, counter-clockwise
  rotate(c, c.n("tombola"), -ang * 57.29578f, kTomCx, kTomCy);
  const int nb = std::clamp<int>(d[5], 0, 16);
  char id[8];
  for (int k = 0; k < 16; ++k) {
    std::snprintf(id, sizeof id, "ball%d", k);
    svg::Node* b = c.n(id);
    show(b, k < nb);
    if (k >= nb) continue;
    const uint32_t xy = uint32_t(d[6 + k]);
    const float x = float(int16_t(xy & 0xFFFF)) / 1000.f, y = float(int16_t(xy >> 16)) / 1000.f;
    move(b, kTomCx + x * kTomPxPerM, kTomCy - y * kTomPxPerM);
  }
  // gravity 0..60 m/s^2 (UI range) -> the green circle's radius; restitution 0..1 -> the bounce bar
  const float g = clamp01(float(seqp(c, kTombola, 0)) / 20000.f);  // t5: gravity knob 0..20
  scale(c.n("gravity"), std::max(g, 0.05f), std::max(g, 0.05f), 297.783f, 14.643f);
  const float r = clamp01(float(seqp(c, kTombola, 1)) / 1500.f);   // t5: restitution 0..1.5
  move(c.n("bouncebar"), 0, -r * (135.02f - 116.02f));
  // spin: the rotation arrow points the way the hexagon turns, the number is |spin| in 0.1 rad/s
  const int spin = seqp(c, kTombola, 2);
  if (spin < 0) scale(c.n("rotation"), -1, 1, 17.6f, 14.f);
  text_slots(c, slots_lr(c, c.n("speed")), std::to_string(std::min(99, std::abs(spin) / 100)), kColWhite);
  show(c.n("crank"), false);
}

// ================================================================ ok = finger (ok.svg; ok_screen.cpp)
// Strings: synth drum syntha synthb cowbella cowbellb join replace fillin chain left right octlow octhigh
// octdown octup green greenc g%ibg ak%i bk%i ad%i bd%i halu haru hald hard hblu hbru hbld hbrd ahand_left
// ahand_right bhand_left bhand_right face%i%i bkguide hold rubber; asserts ok_screen.cpp:487 octave range,
// :492 step 0..31. Artist a = hand 1, artist b = hand 2 (FingerSeq view [4], [5]).
void bind_ok(BindCtx& c) {
  const int32_t* d = view_of(c, kFinger);
  const bool drum = c.side() == 1;
  show(c.n("synth"), !drum);
  show(c.n("drum"), drum);
  // t5: the finger view (seq_engines.h): [4] / [5] the slots of players A / B, [6..19] slot lengths, [20] the
  // cursor, [21] the selected (edited) slot, [24] / [25] the steps of A / B, [28] the selected slot's non-empty
  // steps (bit mask). The grid shows the selected slot: cells past its length off, filled steps without the X
  // (empty) mark, the playing step (or the cursor while stopped) green.
  const int hand[2] = {d[4], d[5]};
  const int shown = std::clamp<int>(d[21], 0, 13);
  const int len = std::clamp<int>(d[6 + shown] > 0 ? d[6 + shown] : 8, 1, 32);
  const bool playing = d[3] != 0;
  const int at = playing ? (hand[0] == shown ? d[24] : (hand[1] == shown ? d[25] : -1)) : d[20];
  const int cur = playing ? d[24] : -1;  // the hands tap on the steps of player A
  const uint32_t filled = uint32_t(d[28]);
  char id[16];
  for (int i = 0; i < 32; ++i) {
    std::snprintf(id, sizeof id, "g%d", i);
    svg::Node* g = c.n(id);
    show(g, i < len);
    if (i == at) tint(g, kColGreen);
    std::snprintf(id, sizeof id, "x%d", i);
    show(c.n(id), i < len && !(filled & (1u << i)));
  }
  // the slot keys: the slots the hands hold light up (ASSUMED: two slots per key circle p0..p6)
  for (int k = 0; k < 7; ++k) {
    std::snprintf(id, sizeof id, "p%d", k);
    bool on = false;
    for (int h : hand) on |= h >= 0 && h / 2 == k;
    tint(c.n(id), on ? 0xDFD9FF : 0x5E5F8F);
  }
  // play mode (shift+orange): 0 replace, 1 join, 2 fill in; chain is never shown (t5, F: screen table 0x018E627C)
  one_of(c, {"replace", "join", "fillin", "chain"}, std::clamp(seqp(c, kFinger, 0), 0, 2));
  // swing (green): the dot on the line, 50 = centre
  move(c.n("green"), float(seqp(c, kFinger, 1) - 50) / 40.f * 39.7f, 0);
  // keyboard octave
  show(c.n("octup"), c.ui.octave > 0);
  show(c.n("octdown"), c.ui.octave < 0);
  tint(c.n("octhigh"), c.ui.octave > 0 ? kColGreen : 0x5E5F8F);
  tint(c.n("octlow"), c.ui.octave < 0 ? kColRed : 0x5E5F8F);
  show(c.n("hold"), c.ui.shift);

  const bool up[2] = {!(hand[0] >= 0 && cur >= 0 && (cur & 1)), !(hand[1] >= 0 && cur >= 0 && !(cur & 1))};
  if (!drum) {
    // hands: up or down, alternating with the steps while the artist plays
    show(c.n("halu"), up[0]); show(c.n("hald"), !up[0]);
    show(c.n("haru"), !up[0]); show(c.n("hard"), up[0]);
    show(c.n("hblu"), up[1]); show(c.n("hbld"), !up[1]);
    show(c.n("hbru"), !up[1]); show(c.n("hbrd"), up[1]);
    if (hand[0] < 0) { show(c.n("haru"), true); show(c.n("hard"), false); }
    if (hand[1] < 0) { show(c.n("hbru"), true); show(c.n("hbrd"), false); }
    // keys the sounding synth voices play, on the keyboard of each playing artist
    bool key[12] = {};
    for (int v = 0; v < 6; ++v)
      if (c.s.synth.display[v] >= 0) key[c.s.synth.display[v] % 12] = true;
    for (int k = 0; k < 12; ++k) {
      for (int a = 0; a < 2; ++a) {
        std::snprintf(id, sizeof id, a ? "bk%d" : "ak%d", k);
        if (key[k] && hand[a] >= 0) { tint(c.n(id), kColGreen); move(c.n(id), 0, 1.5f); }
      }
    }
  } else {
    // ad%i / bd%i: the arm of artist a / b hitting drum piece i. An arm shows while its piece sounds
    // (drum voice key k and k + 12 -> piece k, ASSUMED), the resting hands otherwise.
    bool hitting[2] = {false, false}, sounding[12] = {};
    for (int v = 0; v < 8; ++v)  // drum.cpp / dbox.cpp: display[0..7] = key of each sounding voice
      if (c.s.drum.display[v] >= 0 && c.s.drum.display[v] < 24) sounding[c.s.drum.display[v] % 12] = true;
    // and the last key played (drum.cpp display[41], dbox.cpp [40]) for a moment after it was hit, so
    // a hit shows even when the voice is shorter than a frame (or the kit has no sample)
    const int lk = c.s.drum.display[c.s.drum.type == InstrumentType::Dbox ? 40 : 41];
    if (c.t - c.ui.key_t < 0.15 && lk >= 0 && lk < 24) sounding[lk % 12] = true;
    for (int k = 0; k < 12; ++k) {
      for (int a = 0; a < 2; ++a) {
        const bool on = sounding[k] && (hand[a] >= 0 || !playing);
        hitting[a] |= on;
        std::snprintf(id, sizeof id, a ? "bd%d" : "ad%d", k);
        for (svg::Node* n : c.all(id)) show(n, on);
      }
    }
    // faces: rest face while idle, changing with the steps while the artist plays (ASSUMED)
    one_of(c, {"face00", "face01", "face02", "face03", "face04", "face05"}, hand[0] >= 0 && cur >= 0 ? 1 + cur % 5 : 0);
    one_of(c, {"face10", "face11", "face12", "face13", "face14", "face15"}, hand[1] >= 0 && cur >= 0 ? 1 + (cur + 2) % 5 : 0);
    const char* hands[2][2] = {{"ahand_left", "ahand_right"}, {"bhand_left", "bhand_right"}};
    for (int a = 0; a < 2; ++a) {
      show(c.n(hands[a][0]), !hitting[a]);
      show(c.n(hands[a][1]), !hitting[a]);
    }
  }
}

// ================================================================ sketch (sketch.svg; EtchASketch*.cpp)
// Strings next to "sketch.svg": dot_white hold erase grid_icon line_grey pos_mark grid_dot dotblue dots
// mult_num mult_times mult_div (and sketchgridtype sketchspeed length speed gridtype points). The drawn
// notes are line_grey (re-pathed), pos_mark/dotblue the play position, mult_* the speed factor.
constexpr float kSkX0 = 6.8f, kSkX1 = 312.6f, kSkY0 = 6.9f, kSkY1 = 115.6f;

float sk_x(int pos, int len) { return kSkX0 + (kSkX1 - kSkX0) * float(pos) / float(std::max(1, len - 1)); }
float sk_y(int note) { return kSkY1 - (kSkY1 - kSkY0) * float(note) / 23.f; }

// t5: the sketch has 288 positions per bar (EtchASketchData NotePosCount, seq_engines.h); the view reports the
// notes at every 12th position ([6..29]), the pen ([30]) and the grid type ([31]); speed is the knob 0..10
// (I: (speed + 1) / 3 bars per bar, shown as "x n" / "/ n").
constexpr int kSkPos = 288;

void bind_sketch(BindCtx& c) {
  const int32_t* d = view_of(c, kSketch);
  std::string path;
  char b[48];
  bool pen = false;
  for (int k = 0; k < 24; ++k) {
    const int n = d[6 + k];
    if (n < 0) { pen = false; continue; }
    std::snprintf(b, sizeof b, "%c%.2f,%.2f", pen ? 'L' : 'M', sk_x(k * 12, kSkPos), sk_y(n));
    path += b;
    pen = true;
  }
  svg::Node* line = c.n("line_grey");
  show(line, !path.empty());
  if (!path.empty()) line->set("d", path);
  // the pen: where the next stroke starts (white dot)
  const int penpos = std::clamp<int>(d[30], 0, kSkPos - 1);
  const int pn = d[6 + penpos / 12];
  move(c.n("dot_white"), sk_x(penpos, kSkPos), sk_y(pn >= 0 ? pn : 12));
  // play position while a key is held
  const int pos = d[2];
  const bool playing = d[3] && pos >= 0;
  const float px = playing ? sk_x(pos, kSkPos) : kSkX0;
  move(c.n("pos_mark"), px - 50.4f, 0);
  tint(c.n("pos_mark"), playing ? 0x808099 : 0x41414D);
  const int at = playing ? d[6 + std::clamp(pos / 12, 0, 23)] : -1;
  svg::Node* blue = c.n("dotblue");
  show(blue, at >= 0);
  if (at >= 0) move(blue, px - 50.4f, sk_y(at) - 40.1f);
  const int speed = std::clamp<int>(d[5], 0, 10);
  const bool faster = speed >= 2;
  show(c.n("mult_times"), faster);
  show(c.n("mult_div"), !faster);
  show(c.n("mult_num"), !c.font.ok());  // the factor is drawn by draw_sketch
  tint(c.n("grid_icon"), d[31] ? kColWhite : 0x808099);
  show(c.n("hold"), seqp(c, kSketch, 3) != 0);
}

void draw_sketch(BindCtx& c, svg::Image& img) {
  if (!c.font.ok()) return;
  const int32_t* d = view_of(c, kSketch);
  const int speed = std::clamp<int>(d[5], 0, 10);
  const int f = speed >= 2 ? std::max(1, int(std::lround((speed + 1) / 3.0))) : (speed == 0 ? 3 : 2);
  c.font.draw(img, 116.6f, 126.3f, 14.1f, std::to_string(f), kColGreen, 1.5f);
}

// ================================================================ etchasketch (etchasketch.svg)
// No "etchasketch.svg" string and none of its element names (hbars vbars quantize cursor) are in the
// binary: the R.246 sketch sequencer loads sketch.svg. This older canvas is bound to the sketch state
// for browse mode: the cursor sits on the playing (or last drawn) note, quantize shows a grid type.
void bind_etchasketch(BindCtx& c) {
  const int32_t* d = c.seq_display();
  const bool sk = d[1] == kSketch;  // only the sketch sequencer's view has this layout
  const int pos = sk && d[3] && d[2] >= 0 ? std::clamp<int>(d[2], 0, 287) : 0;
  int note = sk ? d[6 + pos / 12] : -1;
  if (note < 0) note = 12;
  const float x = 5.f + 288.f * float(pos) / 287.f;
  const float y = 149.f - 144.f * float(note) / 23.f;
  move(c.n("cursor"), x, y);
  show(c.n("quantize"), sk && d[31] != 0);
}

// ================================================================ tempo (tempo.svg; TempoScreen.cpp)
// Strings next to "tempo.svg": tempobpm temposyncout temposynctype tempometronomelevel tempobpmdecimal bmls
// beatmatch syncmatch posync altsync bpmL bpmM bpmS blue metronomeicon metronomelevel pendulum tapespeedL
// tapespeedM left right, formats "%.1f bps" "%.1f" "%s:1" "1:%s"; assert TempoScreen.cpp:179 level 0..1.
svg::Box& big_bpm() {  // where draw_tempo writes a tempo of 200 and up (empty: not needed)
  static svg::Box b;
  return b;
}

svg::Box& big_speed() {
  static svg::Box b;
  return b;
}

void bind_tempo(BindCtx& c) {
  pending().clear();
  big_bpm() = svg::Box{};
  big_speed() = svg::Box{};
  const TempoView& tv = c.s.tempo;
  const int bpm10 = std::max<int>(400, tv.bpm_tenths);
  const double b = beats(c);
  const double frac = b - std::floor(b);
  // the number: three sizes; a short pulse on each beat (S, M, then L at rest: ASSUMED)
  const int size = frac < 0.08 ? 2 : (frac < 0.16 ? 1 : 0);
  one_of(c, {"bpmL", "bpmM", "bpmS"}, size);
  const char* grp[3] = {"bpmL", "bpmM", "bpmS"};
  // the slots are laid out for a narrow leading '1' (40..199): 200 and up is drawn by code (draw_tempo)
  svg::Node* num = c.n(grp[size]);
  if (bpm10 / 10 < 200) text_slots(c, slots_lr(c, num), std::to_string(bpm10 / 10), kColBlue);
  else if (num) {
    show(num, false);
    big_bpm() = c.doc.bounds(num, false);
  }
  // beats per second: drawn by code ("%.1f bps"), the authored digit hidden
  if (svg::Node* bps = c.n("bps"); bps && !bps->kids.empty()) show(&bps->kids[0], false);
  // pendulum: swings once per beat, at the extremes on the beat
  const float deg = 24.f * float(std::cos(b * 3.14159265));
  rotate(c, c.n("pendulum"), deg, 299.253f, 28.422f);
  tint(c.n("metronomeicon"), tv.metronome ? kColRed : 0x4E2832);
  // metronome level line
  const float lvl = clamp01(float(c.ui.metronome_level) / 32767.f);
  if (svg::Node* ml = c.n("metronomelevel")) {
    set_line(ml, 299.275f, 144.551f, 299.275f, 144.551f - lvl * (144.551f - 38.043f));
    show(ml, lvl > 0.f);
  }
  // sync type (green encoder): beat match or sync match (ASSUMED); clock source: external or tap
  one_of(c, {"beatmatch", "syncmatch", "posync", "altsync"}, c.ui.sync ? 1 : 0);
  show(c.n("ext"), tv.external_clock);
  show(c.n("tap"), !tv.external_clock);
  show(c.n("po"), false);
  show(c.n("left"), false);
  show(c.n("right"), false);
  // tape speed: percent of normal (ASSUMED reading of T.SPEED), and as cm/s and in/s (19 cm/s = 7.5 ips)
  const int sp = c.s.tape.speed > 0 ? c.s.tape.speed : 1000;
  show(c.n("tapespeedL"), false);
  const int pct = std::clamp((sp + 5) / 10, 0, 999);
  svg::Node* tsm = c.n("tapespeedM");
  if (pct < 200) text_slots(c, slots_lr(c, tsm), std::to_string(pct), kColWhite);
  else if (tsm) {
    show(tsm, false);
    big_speed() = c.doc.bounds(tsm, false);
  }
  const int cms = std::clamp((19 * sp + 500) / 1000, 0, 99);
  svg::Node* cmsn = c.n("cms");
  if (cms < 10 || cms >= 20) {  // the slots fit "1x" only (a narrow '1'): other numbers are drawn by code
    const svg::Box cb = c.doc.bounds(cmsn, false);
    show(cmsn, false);
    const std::string t = std::to_string(cms);
    for (size_t i = 0; i < t.size(); ++i)
      pending().push_back({svg::Box{cb.x1 - 11.f * float(t.size() - i), cb.y0, cb.x1 - 11.f * float(t.size() - i - 1), cb.y1}, t[i], kColWhite});
  } else {
    text_slots(c, slots_lr(c, cmsn), std::to_string(cms), kColWhite);
  }
  const auto ips = slots_lr(c, c.n("ips"));  // [7] [.] [5]
  if (ips.size() == 3) {
    const int t10 = std::clamp((75 * sp + 500) / 1000, 0, 999);  // tenths of an inch per second
    if (t10 < 100) {
      glyph(c, ips[0], char('0' + t10 / 10));
      glyph(c, ips[2], char('0' + t10 % 10));
    } else {
      show(ips[1], false);
      glyph(c, ips[0], char('0' + t10 / 100 % 10));
      glyph(c, ips[2], char('0' + t10 / 10 % 10));
    }
  }
}

void draw_tempo(BindCtx& c, svg::Image& img) {
  if (!c.font.ok()) return;
  flush_slots(c, img);
  const int bpm10 = std::max<int>(400, c.s.tempo.bpm_tenths);
  if (const svg::Box& bb = big_bpm(); !bb.empty()) {
    const std::string t = std::to_string(bpm10 / 10);
    float h = bb.h(), w = text_w(c, t, h);
    if (w > 84.f) { h *= 84.f / w; w = 84.f; }  // fit the box (8..112)
    draw_str(c, &img, 60.f - w / 2, bb.y1 - h, h, t, kColBlue);
  }
  if (const svg::Box& bb = big_speed(); !bb.empty()) {
    const std::string t = std::to_string(std::clamp((std::max<int>(1, c.s.tape.speed) + 5) / 10, 0, 999));
    float h = bb.h(), w = text_w(c, t, h);
    if (w > 84.f) { h *= 84.f / w; w = 84.f; }
    draw_str(c, &img, 228.f - w / 2, bb.y1 - h, h, t, kColWhite);
  }
  char b[16];
  // beats per second ("%.1f bps"; a whole number without the decimal, as authored), right-aligned where
  // the authored digit ends
  const int bps10 = (bpm10 + 30) / 60;
  if (bps10 % 10 == 0) std::snprintf(b, sizeof b, "%d", bps10 / 10);
  else std::snprintf(b, sizeof b, "%d.%d", bps10 / 10, bps10 % 10);
  draw_str(c, &img, 41.f - text_w(c, b, 20.f), 119.3f, 20.f, b, kColWhite);
  // the tenths of the tempo, small, at the number's lower right (ASSUMED: shown when not .0)
  if (bpm10 % 10) {
    std::snprintf(b, sizeof b, ".%d", bpm10 % 10);
    draw_str(c, &img, 98.f, 86.f, 13.f, b, kColBlue);
  }
}

// ================================================================ simple = arpeggio (simple.svg)
// "simple.svg" sits right after the arpeggio's own strings: the table "simple" (UPDATE simple SET data ...,
// its defaults {"length":12,"op":0,"sm":0,"octave":1,"swing":50,"timing":48,"skip":1}), then timing octave
// octavepattern length skip pause %i%% swing none once each trig "type %d" "l %d" hold. Template: staff
// with notes n0..n31 and sharps s0..s31, timing, octave (none/once/each), p0..p3/a0..a3 (octave pattern),
// hold, length, op (type), skip/pause, swing.
int dia(int n) {  // diatonic index of a MIDI note (C = 0)
  static const int idx[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
  return (n / 12) * 7 + idx[((n % 12) + 12) % 12];
}
bool sharp(int n) { return n % 12 == 1 || n % 12 == 3 || n % 12 == 6 || n % 12 == 8 || n % 12 == 10; }

constexpr float kArpNoteX = 302.077f, kArpNoteY = 96.041f, kArpStep = 9.f;  // authored note head: F4

// t5 (2026-10-23): the screen as the firmware's arp screen update 0x01A474B8 draws it (decode notes §3): the view
// (seq_engines.h arpeggio: [4] notes in the set, [5] op, [6] octave mode, [7..22] the set in the order the keys
// were pressed, [23] timing, [24] skip, [25] sm, [26] hold, [27] length, [28] swing, [29] skip counter); the staff
// shows the set (the note playing now in green), octave = none / once / each / all / trig, "type %d" = op (the
// octave pattern), "l %d" = length, skip or pause from sm, the four mask cells p%i / a%i from the 4-step mask
// 16 - skip (I: p = plays, a = masked), hold in red.
void bind_simple(BindCtx& c) {
  const int32_t* d = c.seq_display();
  const int n = std::clamp<int>(d[4], 0, 16);
  const int cur = d[2];
  char id[8];
  for (int i = 0; i < 32; ++i) {
    std::snprintf(id, sizeof id, "n%d", i);
    svg::Node* nn = c.n(id);
    std::snprintf(id, sizeof id, "s%d", i);
    svg::Node* sn = c.n(id);
    const int k = i - (32 - n);  // the set fills the right end of the staff
    const int note = k >= 0 && k < n ? d[7 + k] : -1;
    show(nn, note >= 0);
    show(sn, note >= 0 && sharp(note));
    if (note < 0) continue;
    float y = 101.333f - float(dia(note) - dia(64)) * 5.333f;  // bottom staff line = E4
    while (y > 125.f) y -= 7 * 5.333f;
    while (y < 40.f) y += 7 * 5.333f;
    const float dx = -float(31 - i) * kArpStep, dy = y - kArpNoteY;
    move(nn, dx, dy);
    move(sn, dx, dy);
    if (cur >= 0 && k == cur % std::max(1, n)) tint(nn, kColGreen);
  }
  const int mask = 16 - std::clamp<int>(d[24], 1, 15);
  for (int k = 0; k < 4; ++k) {
    const bool plays = ((mask >> (3 - k)) & 1) != 0;
    std::snprintf(id, sizeof id, "p%d", k);  // a dot: the step plays (p1 is authored dim as a sample)
    show(c.n(id), plays);
    tint(c.n(id), kColWhite);
    std::snprintf(id, sizeof id, "a%d", k);  // a dash: the step is masked
    show(c.n(id), !plays);
  }
  tint(c.n("hold"), d[26] ? kColRed : 0x512132);
  // text parts drawn by code (draw_simple)
  show(c.n("timing"), false);
  show(c.n("octave"), false);
  show(c.n("length"), false);
  show(c.n("octavepattern"), false);
  const bool sh = c.ui.shift;  // shift: the second row lights up (the screen swaps the bright and dim sets)
  tint(c.n("op"), sh ? kColGreen : 0x00475B);
  show(c.n("pause"), d[25] != 0);
  show(c.n("skip"), d[25] == 0);
  tint(c.n("pause"), sh ? kColWhite : 0xAEB1DC);
  tint(c.n("skip"), sh ? kColWhite : 0xAEB1DC);
  svg::Node* sw = c.n("swing");
  percent_digits(c, sw, d[28]);
  if (sh) tint(sw, kColRed);
}

void draw_simple(BindCtx& c, svg::Image& img) {
  if (!c.font.ok()) return;
  const int32_t* d = c.seq_display();
  const bool sh = c.ui.shift;
  c.font.draw(img, 31.9f, 22.4f, 14.2f, timing_name(d[23]), kColBlue, 1.5f);
  static const char* om[] = {"none", "once", "each", "all", "trig"};
  const std::string o = om[std::clamp<int>(d[6], 0, 4)];
  draw_str(c, &img, 124.f - text_w(c, o, 14.2f) / 2, 22.4f, 14.2f, o, kColGreen, 1.5f);
  char b[16];
  std::snprintf(b, sizeof b, "l %d", std::clamp<int>(d[27], 1, 64));
  c.font.draw(img, 31.9f, 125.2f, 14.2f, b, sh ? kColBlue : 0x383572, 1.5f);
  std::snprintf(b, sizeof b, "type %d", std::clamp<int>(d[5], 0, 3));
  draw_str(c, &img, 123.9f - text_w(c, b, 8.5f, 1.2f) / 2, 128.2f, 8.5f, b, sh ? kColGreen : 0x00475B, 1.2f);
}

}  // namespace

OP1_SCREEN("pattern", bind_pattern, nullptr);
OP1_SCREEN("endless", bind_endless, draw_endless);
OP1_SCREEN("tombola", bind_tombola, nullptr);
OP1_SCREEN("ok", bind_ok, nullptr);
OP1_SCREEN("sketch", bind_sketch, draw_sketch);
OP1_SCREEN("etchasketch", bind_etchasketch, nullptr);
OP1_SCREEN("tempo", bind_tempo, draw_tempo);
OP1_SCREEN("simple", bind_simple, draw_simple);  // the arpeggio

}  // namespace op1
