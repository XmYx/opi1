// System screens (owner: t10 "system" group): clock, album, presetbrowser, com, tune, signalflow, lander, help,
// in, micline, tapeconfig, save, colors, simple, subscreenhand. Each binding names the firmware evidence it
// follows: the element names listed next to the screen's "<name>.svg" string in the binary and the
// "..\src\gui\screens\X.cpp:LINE" asserts near it. Where the port has no data for a screen (album files,
// preset lists, OP-Z link, battery, the input source), the screen shows one coherent idle state and animates
// what the snapshot can drive. The per-element table with evidence levels is in the t10 notes (system.md).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "bind.h"

namespace op1 {
namespace {

constexpr float kPi = 3.14159265f;

// ---------------------------------------------------------------- shared helpers

// Puts character ch into dst (a leaf shape with a stroke), sized and baselined like the full-height authored
// glyph `ref`, centred at cx (dst and ref share a coordinate space). ' ' hides dst.
bool put_glyph(BindCtx& c, svg::Node* dst, svg::Node* ref, char ch, float cx) {
  if (!dst || !ref || !c.font.ok()) return false;
  if (ch == ' ') { dst->visible = 0; return true; }
  const svg::Node* g0 = ref->glyph;
  const svg::Affine x0 = ref->glyph_xf;
  const int v0 = ref->visible;
  const bool ok = c.font.set(c.doc, ref, ch);
  const svg::Node* g = ref->glyph;
  svg::Affine xf = ref->glyph_xf;
  ref->glyph = g0;
  ref->glyph_xf = x0;
  ref->visible = v0;
  if (!ok || !g) { dst->visible = 0; return false; }
  xf.e += cx - c.doc.bounds(ref, true).cx();
  dst->glyph = g;
  dst->glyph_xf = xf;
  if (dst->visible == 0) dst->visible = -1;
  return true;
}

// Recolours a node and every node under it. svg::Node::tint on a group only sets the group's own style and
// its children re-apply their authored colours, so the binding tints the whole subtree.
void tintg(svg::Node* n, uint32_t rgb) {
  if (!n) return;
  tint(n, rgb);
  for (svg::Node& k : n->kids) tintg(&k, rgb);
}

// Glyph width (in the slot's units) of ch when sized like ref.
float glyph_w(BindCtx& c, svg::Node* ref, char ch) {
  if (!ref || !c.font.ok() || ch == ' ') return 0;
  const svg::Node* g0 = ref->glyph;
  const svg::Affine x0 = ref->glyph_xf;
  const int v0 = ref->visible;
  float w = 0;
  if (c.font.set(c.doc, ref, ch) && ref->glyph) w = c.doc.bounds(ref->glyph, true).w() * ref->glyph_xf.a;
  ref->glyph = g0;
  ref->glyph_xf = x0;
  ref->visible = v0;
  return w;
}

// Lays text out proportionally (glyph boxes separated by gap) into slots, left-aligned at x, or
// right-aligned when right = true. Unused slots are hidden.
void put_text(BindCtx& c, const std::vector<svg::Node*>& slots, svg::Node* ref, const std::string& text, float x,
              float gap, bool right = false) {
  std::vector<float> w;
  float total = 0;
  for (char ch : text) {
    w.push_back(glyph_w(c, ref, ch));
    total += w.back();
  }
  if (!text.empty()) total += gap * float(text.size() - 1);
  float at = right ? x - total : x;
  for (size_t i = 0; i < slots.size(); ++i) {
    if (i >= text.size()) { show(slots[i], false); continue; }
    put_glyph(c, slots[i], ref, text[i], at + w[i] / 2);
    at += w[i] + gap;
  }
}

// The i-th child of a node (nullptr if none).
svg::Node* kid(svg::Node* n, size_t i) { return n && i < n->kids.size() ? &n->kids[i] : nullptr; }

void show_all(BindCtx& c, const char* id, bool on) {
  for (svg::Node* n : c.all(id)) show(n, on);
}

int held_count(const UiState& ui) {
  int k = 0;
  for (bool h : ui.held) k += h ? 1 : 0;
  return k;
}

// Seconds per beat at the current tempo.
float beat_s(const CoreSnapshot& s) { return 600.f / float(std::max<int>(400, s.tempo.bpm_tenths)); }

// Counts encoder events per encoder (the UI model reports when each encoder turned, not by how much):
// a screen that keeps a cursor without a UiModel mapping steps it once per event. ASSUMED forward only.
struct EncCounter {
  double last[4] = {-1e9, -1e9, -1e9, -1e9};
  int count[4] = {};
  void update(const UiState& ui) {
    for (int i = 0; i < 4; ++i)
      if (ui.enc_t[i] != last[i]) {
        if (last[i] > -1e8 || ui.enc_t[i] > -1e8) ++count[i];
        last[i] = ui.enc_t[i];
      }
  }
};

// Peak follower for meters (UI thread only).
struct Follow {
  float v = 0, hold = 0;
  double hold_t = 0;
  float step(float x, double t, float fall = 0.08f) {
    v = x > v ? x : std::max(x, v - fall);
    if (v >= hold || t - hold_t > 1.2) { hold = v; hold_t = t; }
    return v;
  }
};

float meter01(const CoreSnapshot& s) { return clamp01(float(std::max<int>(s.mixer.meter_l, s.mixer.meter_r)) / 32767.f); }

// ================================================================ clock (clock.svg; firmware "clock.svg"
// with "%i-%02i-%02i", "%02i", hour, hand_hour, hand_min, hand_sec next to it). The date and time are
// digit groups ymd/year, month, day and hs/hour, min (outline glyphs); the analog clock rotates the three
// hands about the dial centre. Wall clock (localtime) of the host.
constexpr float kDialX = 89.06f, kDialY = 101.2f;

void bind_clock(BindCtx& c) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  const double frac = std::fmod(c.t, 1.0);
  char b[16];
  // ---- date and time: proportional outline digits (gap 9 at this size, as authored)
  constexpr float kGap = 9.f;
  svg::Node* year = c.n("year");
  svg::Node* yref = kid(year, 0);  // a full-height "0"
  std::snprintf(b, sizeof b, "%04d", (tm.tm_year + 1900) % 10000);
  put_text(c, {kid(year, 2), kid(year, 0), kid(year, 1), kid(year, 3)}, yref, b, 25.3f, kGap);
  show(kid(year, 4), false);
  svg::Node* month = c.n("month");
  std::snprintf(b, sizeof b, "%02d", tm.tm_mon + 1);
  put_text(c, {kid(month, 0), kid(month, 1)}, yref, b, 168.4f, kGap);
  svg::Node* day = c.n("day");
  std::snprintf(b, sizeof b, "%02d", tm.tm_mday);
  put_text(c, {kid(day, 0), kid(day, 1)}, yref, b, 255.5f, kGap);
  show(kid(day, 2), false);
  svg::Node* hour = c.n("hour");
  svg::Node* mins = c.n("min");
  svg::Node* href = kid(hour, 1);  // the full-height "4"
  std::snprintf(b, sizeof b, "%02d", tm.tm_hour);
  put_text(c, {kid(hour, 0), kid(hour, 1)}, href, b, 169.0f, kGap, true);
  std::snprintf(b, sizeof b, "%02d", tm.tm_min);
  put_text(c, {kid(mins, 0), kid(mins, 1)}, href, b, 190.6f, kGap);
  // the colon blinks with the seconds (ASSUMED)
  svg::Node* st = c.n("static");
  show(kid(st, 1), frac < 0.5);
  show(kid(st, 2), frac < 0.5);
  // ---- hands (authored pointing at 12)
  const float sec = float(tm.tm_sec), mn = float(tm.tm_min) + sec / 60.f, hr = float(tm.tm_hour % 12) + mn / 60.f;
  rotate(c, c.n("hand_hour"), hr * 30.f, kDialX, kDialY);
  rotate(c, c.n("hand_min"), mn * 6.f, kDialX, kDialY);
  rotate(c, c.n("hand_sec"), sec * 6.f, kDialX, kDialY);
}

// ================================================================ album (album.svg; firmware strings next to
// "album.svg": turna, turnb, rec_dot, arm_c, turn_c, trace%i, timemin, timesec0, timesec1, play, stop, side,
// turn%i). The port has no album recorder: the turntable follows the tape transport (it plays/records what
// the tape does), side A. The record's light reflection steps through turn0..turn11 while playing, the tone
// arm swings from its rest onto the record and across it with the position, the counter shows m:ss.
constexpr float kArmPX = 208.195f, kArmPY = 34.635f;     // arm_c
constexpr float kRecCX = 147.735f, kRecCY = 60.583f;     // turn_c
constexpr float kArmLen = 70.f;                          // pivot to stylus (inferred from the arm group)
constexpr float kArmRestDeg = 100.4f;                    // authored stylus direction from the pivot (y down)

float arm_dir_for_radius(float r) {
  const float dx = kRecCX - kArmPX, dy = kRecCY - kArmPY, pc = std::sqrt(dx * dx + dy * dy);
  const float cs = std::clamp((kArmLen * kArmLen + pc * pc - r * r) / (2 * kArmLen * pc), -1.f, 1.f);
  return std::atan2(dy, dx) * 180.f / kPi - std::acos(cs) * 180.f / kPi;
}

void bind_album(BindCtx& c) {
  const TapeView& tv = c.s.tape;
  const bool playing = tv.playing, rec = tv.recording;
  const float sec = float(tv.position) / float(kSampleRate);
  // turntable reflection: 12 frames, 33 1/3 rpm = 0.555 turns/s
  svg::Node* t0 = c.n("turn0");
  if (t0 && t0->parent) show(t0->parent, true);
  char id[16];
  const int frame = playing ? int(std::fmod(sec * 0.555f * 12.f, 12.f)) : 0;
  for (int i = 0; i < 12; ++i) {
    std::snprintf(id, sizeof id, "turn%d", i);
    show(c.n(id), i == frame);
  }
  // label of the side (A) turns with the record
  show(c.n("turnb"), false);
  if (playing) rotate(c, c.n("turna"), sec * 0.555f * 360.f, kRecCX, kRecCY);
  // tone arm: rest when stopped, on the record when playing; outer groove (r 45) -> inner (r 20) over 20 min
  float deg = 0;
  if (playing || rec) {
    const float r = 45.f - 25.f * clamp01(sec / 1200.f);
    deg = arm_dir_for_radius(r) - kArmRestDeg;
  }
  rotate(c, c.n("arm"), deg, kArmPX, kArmPY);
  // counter m:ss
  svg::Node* tm = c.n("timemin");
  const int isec = int(sec);
  put_glyph(c, tm, tm, char('0' + (isec / 60) % 10), c.box("timemin").cx());
  put_glyph(c, c.n("timesec1"), tm, char('0' + (isec % 60) / 10), c.box("timesec1").cx());
  put_glyph(c, c.n("timesec0"), tm, char('0' + isec % 10), c.box("timesec0").cx());
  // recording: the dot at the end of the cue line and its scribble trace
  show(c.n("rec_dot"), rec);
  const int tr = rec ? int(c.t * 12.0) % 16 : -1;
  for (int i = 0; i < 16; ++i) {
    std::snprintf(id, sizeof id, "trace%d", i);
    show(c.n(id), i == tr);
  }
  tintg(c.n("box"), rec ? kColRed : 0x435569);
  // the transport words: the active one bright
  constexpr uint32_t kDimLbl = 0x435569, kLit = 0xAEB1DC;
  tintg(c.n("rec"), rec ? kColRed : kDimLbl);
  tintg(c.n("play"), playing && !rec ? kLit : kDimLbl);
  tintg(c.n("stop"), !playing && !rec ? kLit : kDimLbl);
  // side A selected
  tintg(c.n("s0"), kLit);
  tintg(c.n("s1"), 0x514959);
  tintg(c.n("t0"), rec ? kColRed : kLit);
  tintg(c.n("t1"), 0x514959);
}

// ================================================================ presetbrowser (presetbrowser.svg;
// PresetBrowser.cpp: mode, slot, type1..6, preset1..6, typecursor, presetcursor, typebar, presetbar,
// typeslider, presetslider; m_parent_bar / m_child_bar / m_parent_slider / m_child_slider asserts). The port
// has no preset store of its own; with the firmware image the lists are op1_factory.db's (t6 review, Sir: an
// encoder push loads the preset onto the slot): the folders of the side (synth: one per engine) on blue, the
// folder's presets on green, six rows per page, drawn in the firmware font over the hidden authored rows; the
// big digit is the sound slot. Without the library the authored lists stay (the older picture-only binding).
EncCounter g_pb_enc;

constexpr float kPbRowTop = 9.333f, kPbRowH = 13.333f, kPbPitch = 23.2f;  // authored rows type1..6 / preset1..6

void bind_presetbrowser_lists(BindCtx& c) {
  const UiState& u = c.ui;
  const int nf = int(u.pb_folders.size());
  const int f = std::clamp(u.pb_folder, 0, nf - 1);
  const int np = int(u.pb_names[size_t(f)].size());
  const int p = std::clamp(u.pb_preset, 0, std::max(0, np - 1));
  char id[16];
  for (int i = 1; i <= 6; ++i) {
    std::snprintf(id, sizeof id, "type%d", i);
    show(c.n(id), false);
    std::snprintf(id, sizeof id, "preset%d", i);
    show(c.n(id), false);
  }
  move(c.n("typecursor"), 0, float(f % 6) * kPbPitch);
  move(c.n("presetcursor"), 0, float(p % 6) * kPbPitch);
  if (svg::Node* slot = c.n("slot")) text_into(c, {slot}, std::to_string((u.pb_slot & 7) + 1));
  // bars: the cursor's share of the list, at its place
  const float y0 = 6.6f, y1 = 136.6f;
  auto bar = [&](const char* name, float x, int cur, int n) {
    const float seg = (y1 - y0) / float(std::max(1, n));
    set_line(c.n(name), x, y0 + seg * float(cur), x, y0 + seg * float(cur + 1));
  };
  bar("typebar", 173.8f, f, nf);
  bar("presetbar", 306.6f, p, np);
}

void draw_presetbrowser(BindCtx& c, svg::Image& img) {
  const UiState& u = c.ui;
  if (u.pb_folders.empty() || !c.font.ok()) return;
  const float s = float(img.w) / 320.f;
  // lower case: the firmware font draws upper-case letters as pictograms (FX, LFO, keys...), and its
  // lower-case letters look like the screens' capitals; cut to the column (left 104 px, right 112 px)
  auto label = [&](std::string t, float width) {
    for (char& ch : t) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    while (t.size() > 1 && c.font.cell_w(kPbRowH) * 1.25f * float(t.size()) > width) t.pop_back();
    return t;
  };
  const int nf = int(u.pb_folders.size());
  const int f = std::clamp(u.pb_folder, 0, nf - 1);
  const auto& names = u.pb_names[size_t(f)];
  const int p = std::clamp(u.pb_preset, 0, std::max(0, int(names.size()) - 1));
  for (int r = 0; r < 6; ++r) {
    const float y = (kPbRowTop + kPbPitch * float(r)) * s;
    const int fi = f / 6 * 6 + r, pi = p / 6 * 6 + r;
    if (fi < nf) c.font.draw(img, 66.f * s, y, kPbRowH * s, label(u.pb_folders[size_t(fi)], 104.f), fi == f ? 0x000000 : kColBlue, 1.5f * s);
    if (pi < int(names.size())) c.font.draw(img, 190.7f * s, y, kPbRowH * s, label(names[size_t(pi)], 112.f), kColGreen, 1.5f * s);
  }
}

void bind_presetbrowser(BindCtx& c) {
  g_pb_enc.update(c.ui);
  if (!c.ui.pb_folders.empty()) { bind_presetbrowser_lists(c); return; }
  int trow = -1;
  switch (c.s.synth.type) {  // FM, PULSE, STRING, OCS, WAVES, SAMPL (ASSUMED engine <-> row)
    case InstrumentType::Fm: trow = 0; break;
    case InstrumentType::Pulse: trow = 1; break;
    case InstrumentType::String: trow = 2; break;
    case InstrumentType::Cluster: trow = 3; break;
    case InstrumentType::DrWave: trow = 4; break;
    case InstrumentType::Sampler: trow = 5; break;
    default: trow = ((c.ui.enc_acc[0] % 6) + 6) % 6; break;
  }
  const int prow = ((c.ui.enc_acc[1] % 6) + 6) % 6;  // UiState::enc_acc: signed detents
  const float pitch = (c.box("type6").cy() - c.box("type1").cy()) / 5.f;
  move(c.n("typecursor"), 0, float(trow) * (std::isfinite(pitch) ? pitch : 23.2f));
  move(c.n("presetcursor"), 0, float(prow) * (std::isfinite(pitch) ? pitch : 23.2f));
  // the row under the cursor is drawn black on the cursor, the others in the list colour
  char id[16];
  for (int i = 0; i < 6; ++i) {
    std::snprintf(id, sizeof id, "type%d", i + 1);
    tintg(c.n(id), i == trow ? 0x000000 : kColBlue);
    std::snprintf(id, sizeof id, "preset%d", i + 1);
    tintg(c.n(id), i == prow ? kColGreen : kColGreen);
  }
  // bars: one sixth of the slider per row (ASSUMED list length 6)
  const float y0 = 6.6f, y1 = 136.6f, seg = (y1 - y0) / 6.f;
  set_line(c.n("typebar"), 173.8f, y0 + seg * float(trow), 173.8f, y0 + seg * float(trow + 1));
  set_line(c.n("presetbar"), 306.6f, y0 + seg * float(prow), 306.6f, y0 + seg * float(prow + 1));
}

// ================================================================ com (com.svg; strings next to "com.svg":
// t%itext, t%ibox, ssicon%i, cable, usbtext, diskicon, midi, wire, synth, laptop, opset, midiset, opmidich,
// txta, txtb, chrgon, chrgoff, trans, reason, ableton, ff%i, tr%i, tr%iw, tr%ig, pg%i, mg%i, midich; the
// OP-Z group's x/d, a/b, p%i, k1..k4, bpm). Four pages on T1..T4 (UiState::com_page, the encoders step it):
// 0 OP-Z link (the authored page), 1 CTRL (MIDI mode: laptop + midi settings), 2 DISK (disk mode),
// 3 OPT (OP-1 settings, USB charge). Selected T box bright. OP-Z page animates from the held keys and the
// sixteenth-note clock; there is no OP-Z link in the port.
void bind_com(BindCtx& c) {
  const int page = std::clamp(c.ui.com_page, 0, 3);
  char id[16];
  for (int i = 1; i <= 4; ++i) {
    std::snprintf(id, sizeof id, "t%dtext", i);
    show(c.n(id), true);
    tintg(c.n(id), i - 1 == page ? kColWhite : 0x6E6CA8);
    std::snprintf(id, sizeof id, "t%dbox", i);
    tintg(c.n(id), i - 1 == page ? 0xADADFF : 0x383572);
  }
  show(c.n("opz"), page == 0);
  show(c.n("laptop"), page == 2);
  show(c.n("midiset"), page == 1);
  show(c.n("ssicon1"), page == 1);
  show(c.n("ssicon2"), page == 2);
  show(c.n("opset"), false);
  show(c.n("ssicon3"), page == 3);
  show(c.n("synth"), false);
  show(c.n("ssicon0"), false);
  show(c.n("reason"), false);
  show(c.n("ableton"), false);
  if (page == 2) {
    show(c.n("midi"), false);
    show(c.n("diskicon"), std::fmod(c.t, 1.0) < 0.5);  // disk activity blink (ASSUMED)
  }
  if (page == 3) {
    const bool charging = true;  // no battery model: USB power assumed
    show(c.n("chrgon"), charging);
    show(c.n("chrgoff"), !charging);
  }
  if (page != 0) return;
  // ---- OP-Z: 16 tracks. Track i lit (green) when a key i of the octave is held, the current one white.
  const int base = 60 + 12 * c.ui.octave;
  int cur = c.ui.last_note >= 0 ? ((c.ui.last_note - base) % 16 + 16) % 16 : 0;
  for (int i = 0; i < 16; ++i) {
    const int n = base + i;
    const bool on = n >= 0 && n < 128 && c.ui.held[n];
    std::snprintf(id, sizeof id, "tr%d", i);
    show(c.n(id), !on && i != cur);
    std::snprintf(id, sizeof id, "tr%dw", i);
    show(c.n(id), !on && i == cur);
    std::snprintf(id, sizeof id, "tr%dg", i);
    show(c.n(id), on);
    // mute rows: x (red) = track idle, d (green) = playing; a/b likewise for the second row
    std::snprintf(id, sizeof id, "x%d", i);
    show(c.n(id), !on);
    std::snprintf(id, sizeof id, "d%d", i);
    show(c.n(id), on);
  }
  // step chase: the sixteenth under the tempo clock (tape position when playing, else UI time)
  const double t = c.s.tape.playing ? double(c.s.tape.position) / kSampleRate : c.t;
  const int step = int(t / (beat_s(c.s) / 4.f)) % 16;
  for (int i = 0; i < 16; ++i) {
    std::snprintf(id, sizeof id, "p%d", i);
    tintg(c.n(id), i == step ? kColWhite : 0x1F7FD8);
  }
  // ff%i: the big yellow number (pattern 0..23 ASSUMED): the current bar
  const int bar = int(t / (beat_s(c.s) * 4.f)) % 24;
  for (int i = 0; i < 24; ++i) {
    std::snprintf(id, sizeof id, "ff%d", i);
    show(c.n(id), i == bar);
  }
}

// ================================================================ tune (tune.svg; strings next to "tune.svg":
// notestune, centstune, pmval, portval, "detune notes", port, "detune cents", dtval, gval). Rows coloured for
// the encoders: play mode (blue), portamento (green), g-bend (white), detune (orange). Play mode and
// portamento are the synth patch's adsr slots 4/5 (synth_instrument.h); g-bend and detune have no engine
// parameter in the port and keep their authored values.
const char* play_mode_name(int m) {
  static const char* n[] = {"poly", "mono", "legato", "unison"};
  return n[std::clamp(m, 0, 3)];
}

void bind_tune(BindCtx& c) {
  show(c.n("pmval"), false);  // drawn by code: the name has 4..6 letters
  const int port = c.s.synth.adsr[5] <= 64 ? 0 : std::clamp(int((c.s.synth.adsr[5] - 64) * 99 / (12864 - 64)) + 1, 1, 99);
  char b[8];
  std::snprintf(b, sizeof b, "%02d", port);
  svg::Node* pv = c.n("portval");
  svg::Node* ref = kid(pv, 0);
  put_glyph(c, kid(pv, 0), ref, b[0], 249.2f);
  put_glyph(c, kid(pv, 1), ref, b[1], 261.7f);
  // the row of the encoder turned last pulses (value pop-up feel)
  const int e = recent_enc(c, 0.6);
  const char* rows[4] = {"pmval", "portval", "gval", "dtval"};
  for (int i = 1; i < 4; ++i)
    if (svg::Node* r = c.n(rows[i]); r && e == i) r->alpha = 0.6f;
}

void draw_tune(BindCtx& c, svg::Image& img) {
  const int m = int(c.s.synth.adsr[4]) >> 12;
  const bool pulse = recent_enc(c, 0.6) == 0;
  c.font.draw(img, 214.7f, 17.5f, 19.8f, play_mode_name(m), pulse ? kColWhite : kColBlue, 2.f);
}

// ================================================================ signalflow (signalflow.svg; strings next to
// "signalflow.svg": synth, drum, lfoon/lfooff, instrfxon/off, masterfxon/off, keyson/off, seqon/off, tapetrack,
// tapeinput, instrrouting, reclevelwarn, mixerwarn, eqwarn, drivewarn, instr, out_l, out_r, midiin, midiout,
// midisyncin, midisyncout, tapein%i, tapevu%i, tapevubase%i, tape%i, reelleft, reelright, drive, seqtoinstr,
// keytoseq, keytoinstr, lfotoinstr, lfotofx). Shows what is active on the instrument side last played.
Follow g_sf_vu[4];

void bind_signalflow(BindCtx& c) {
  const bool drum = c.side() == 1;
  const InstrumentView& iv = c.inst();
  const int32_t* sd = c.seq_display();
  const bool seq = sd[0] != 0;
  const bool keys = held_count(c.ui) > 0;
  show(c.n("synth"), !drum);
  show(c.n("drum"), drum);
  show(c.n("lfoon"), iv.lfo_on);
  show(c.n("lfooff"), !iv.lfo_on);
  show(c.n("instrfxon"), iv.fx_on);
  show(c.n("instrfxoff"), !iv.fx_on);
  show(c.n("masterfxon"), c.s.mixer.master_fx_on);
  show(c.n("masterfxoff"), !c.s.mixer.master_fx_on);
  show(c.n("seqon"), seq);
  show(c.n("seqoff"), !seq);
  show(c.n("keyson"), keys);
  show(c.n("keysoff"), !keys);
  constexpr uint32_t kOn = 0xAEB1DC, kOff = 0x534468;
  tintg(c.n("keytoinstr"), seq ? kOff : kOn);
  tintg(c.n("keytoseq"), seq ? kOn : kOff);
  tintg(c.n("seqtoinstr"), seq ? kOn : kOff);
  // the LFO arrows: into the instrument for most LFOs, into the effect for "element" when it targets fx (ASSUMED)
  tintg(c.n("lfotoinstr"), iv.lfo_on ? kOn : kOff);
  tintg(c.n("lfotofx"), iv.lfo_on && iv.fx_on ? kOn : kOff);
  // tape: reels turn with the tape, the record line lights while recording
  const TapeView& tv = c.s.tape;
  const float deg = float(tv.position) / float(kSampleRate) * 120.f;
  rotate(c, c.n("reelleft"), deg, 118.984f, 97.284f);
  rotate(c, c.n("reelright"), deg, 146.229f, 97.284f);
  show(c.n("tapetrack"), tv.recording);
  show(c.n("tapeinput"), false);  // no audio input in the port
  // per-track VU needles swing about their base (authored pointing up): -70..+70 degrees
  char id[16];
  for (int t = 0; t < 4; ++t) {
    const float l = g_sf_vu[t].step(clamp01(float(tv.level[t]) / 32767.f * q15(c.s.mixer.volume[t]) * 1.5f), c.t);
    std::snprintf(id, sizeof id, "tapevubase%d", t + 1);
    const svg::Box b = c.box(id);
    std::snprintf(id, sizeof id, "tapevu%d", t + 1);
    rotate(c, c.n(id), -70.f + 140.f * l, b.cx(), b.cy());
    std::snprintf(id, sizeof id, "tapein%d", t + 1);
    tintg(c.n(id), tv.recording && (tv.track & 3) == t ? kColRed : kOn);
  }
  // outputs light with signal; warnings on clipping
  const float m = meter01(c.s);
  tintg(c.n("out_l"), c.s.mixer.meter_l > 300 ? kColWhite : kOff);
  tintg(c.n("out_r"), c.s.mixer.meter_r > 300 ? kColWhite : kOff);
  show(c.n("mixerwarn"), m > 0.97f);
  int tl = 0;
  for (int t = 0; t < 4; ++t) tl = std::max<int>(tl, tv.level[t]);
  show(c.n("reclevelwarn"), tv.recording && tl > 32000);
  show(c.n("eqwarn"), std::max({c.s.mixer.eq[0], c.s.mixer.eq[1], c.s.mixer.eq[2]}) > 31000);
  show(c.n("drivewarn"), c.s.mixer.master[0] > 31000 && m > 0.8f);  // master slot 0 = drive (ASSUMED)
  // midi dots: external clock in, sync out (UiState), notes: none in the snapshot
  tintg(c.n("midisyncin"), c.s.tempo.external_clock ? kColRed : kColDim);
  tintg(c.n("midisyncout"), c.ui.sync_out ? kColRed : kColDim);
  tintg(c.n("midiin"), kColDim);
  tintg(c.n("midiout"), keys ? kColRed : kColDim);  // the OP-1 sends its keys over USB MIDI
}

// ================================================================ lander (lander.svg, 1000x700 world; strings
// next to "lander.svg": face0%i, ahand_left, ahand_right, ad10, ad11, title, titleship, titletext, chopper,
// faceleft, faceright, righthook, lefthook, swing*, scale*, objects, bgobjects, score, fuelarea, score%hd,
// ground, factory, hookcenter, shiphookcenter, boardjoint, bicycle*, lefthatch, righthatch, physics; "physics",
// "wheel", "circle" and the jsoncpp strings = a Box2D-style world). Not the full game: a title screen until a
// key or encoder, then the chopper flies under a 320x160 camera that follows it. Keys = thrust (left part of
// the keyboard also pushes left, right part right); fuel burns with thrust and refills on the gas station.
// The physics shapes (collision outlines) are hidden; their bounding boxes (ground, start tower, gas tower,
// house, factory) are a crude floor. The drumming ape
// in the background plays to the tempo.
struct Lander {
  const svg::Document* doc = nullptr;
  double last_t = -1, enter_t = 0;
  bool game = false;
  float x = 0, y = 0, vx = 0, vy = 0, fuel = 1;  // offset of the chopper from its authored place
  bool left = false;
  std::vector<svg::Box> floor;
} g_ld;

constexpr float kChopX = 168.3f, kChopY = 278.3f;  // authored chopper centre (start tower)
constexpr float kChopFoot = 26.f;                   // centre to the bottom of the hook (lefthook/righthook)

void hide_debug_red(svg::Node* n) {
  if (!n) return;
  for (const char* a : {"stroke", "fill"})
    if (const std::string* v = n->get(a); v && (*v == "#FF0000" || *v == "#ff0000")) n->visible = 0;
  for (svg::Node& k : n->kids) hide_debug_red(&k);
}

void lander_floor(BindCtx& c) {
  g_ld.floor.clear();
  std::vector<svg::Node*> st;
  for (const char* g : {"ground", "start_tower", "gastower", "house", "factory"})
    if (svg::Node* n = c.n(g)) st.push_back(n);
  while (!st.empty()) {
    svg::Node* n = st.back();
    st.pop_back();
    if (svg::clean_id(n->id) == "physics") {
      const svg::Box b = c.doc.bounds(n, false);
      if (!b.empty() && b.w() > 4) g_ld.floor.push_back(b);
      continue;
    }
    for (svg::Node& k : n->kids) st.push_back(&k);
  }
}

void bind_lander(BindCtx& c) {
  Lander& L = g_ld;
  if (L.doc != &c.doc) { L.doc = &c.doc; lander_floor(c); }
  if (c.t - L.last_t > 0.5) {  // (re-)entered the screen: title
    L.enter_t = c.t;
    L.game = false;
    L.x = L.y = L.vx = L.vy = 0;
    L.fuel = 1;
  }
  const float dt = float(std::clamp(c.t - L.last_t, 0.0, 0.05));
  L.last_t = c.t;
  const double last_in = std::max({c.ui.key_t, c.ui.enc_t[0], c.ui.enc_t[1], c.ui.enc_t[2], c.ui.enc_t[3]});
  if (!L.game && last_in > L.enter_t + 0.05) L.game = true;

  // collision outlines (pure red #FF0000 under the "physics" nodes) are never drawn; physics groups that
  // carry an object's own artwork keep it
  for (svg::Node* p : c.all("physics")) hide_debug_red(p);
  for (const char* s : {"score-200", "score-100", "score50", "score25", "score35", "score5"}) show_all(c, s, false);

  // ape drummer: hands alternate on the beat, the face changes every bar
  const double beat = c.t / beat_s(c.s);
  const int par = int(beat) & 1;
  char id[16];
  for (int k = 0; k < 6; ++k) {
    std::snprintf(id, sizeof id, "ad%d", 2 * k);
    show(c.n(id), par == 0);
    std::snprintf(id, sizeof id, "ad%d", 2 * k + 1);
    show(c.n(id), par == 1);
  }
  const int face = int(beat / 4) % 6;
  for (int k = 0; k < 6; ++k) {
    std::snprintf(id, sizeof id, "face0%d", k);
    show(c.n(id), k == face);
  }

  // input: any held key = thrust; keys of the left/right third push sideways (ASSUMED controls)
  int up = 0, side = 0;
  const int base = 60 + 12 * c.ui.octave;
  for (int n = 0; n < 128; ++n)
    if (c.ui.held[n]) {
      ++up;
      const int k = n - base;
      side += k < 8 ? -1 : (k >= 16 ? 1 : 0);
    }
  if (L.game) {
    const bool thrust = up > 0 && L.fuel > 0;
    L.vy += 60.f * dt;
    if (thrust) {
      L.vy -= 130.f * dt;
      L.vx += float(std::clamp(side, -1, 1)) * 70.f * dt;
      L.fuel = std::max(0.f, L.fuel - 0.04f * dt);
    }
    L.vx *= 1.f - 0.6f * dt;
    if (side) L.left = side < 0;
    float nx = L.x + L.vx * dt, ny = L.y + L.vy * dt;
    // world limits and the floor boxes (the hook hangs kChopFoot below the centre)
    const float cx = kChopX + nx;
    float bottom = kChopY + ny + kChopFoot;
    for (const svg::Box& b : L.floor)
      if (cx > b.x0 && cx < b.x1 && bottom > b.y0 && kChopY + L.y + kChopFoot <= b.y0 + 2.f) {
        ny = b.y0 - kChopFoot - kChopY;
        L.vy = 0;
        L.vx *= 0.8f;
        bottom = b.y0;
      }
    nx = std::clamp(nx, 20.f - kChopX, 1240.f - kChopX);
    ny = std::clamp(ny, 20.f - kChopY, 680.f - kChopY);
    L.x = nx;
    L.y = ny;
    // refuel at the gas station (fuelarea marker)
    const svg::Box fa = c.box("fuelarea");
    if (!fa.empty() && std::fabs(kChopX + L.x - fa.cx()) < 40.f && std::fabs(bottom - fa.cy()) < 30.f && L.vy == 0)
      L.fuel = std::min(1.f, L.fuel + 0.3f * dt);
  }
  // chopper and its hook move together; the face shows the flying direction
  for (const char* s : {"chopper", "righthook", "lefthook", "hookcenter"}) move(c.n(s), L.x, L.y);
  show(c.n("faceleft"), L.left);
  show(c.n("faceright"), !L.left);
  // title vs game
  show(c.n("title"), !L.game);
  show(c.n("titletext"), !L.game && std::fmod(c.t, 1.0) < 0.7);
  show(c.n("titleship"), !L.game);
  if (!L.game) move(c.n("titleship"), 0, 2.f * std::sin(float(c.t) * 3.f));
  // camera: a 320x160 window at 1:1 (the title sits at the world's top-left window)
  float cx0 = 0, cy0 = 0;
  if (L.game) {
    cx0 = std::clamp(kChopX + L.x - 160.f, 0.f, 1264.f - 320.f);
    cy0 = std::clamp(kChopY + L.y - 80.f, 0.f, 700.f - 160.f);
  }
  const float s0 = c.doc.px_scale(kDisplayW, kDisplayH);
  const svg::Affine cam = svg::Affine::translate(-cx0, -cy0).then(svg::Affine::scale(1.f / s0, 1.f / s0));
  // a node's dx/dy apply after its xf, so the moves are folded into xf before the camera
  for (svg::Node& k : const_cast<svg::Node&>(c.doc.root()).kids) {
    k.xf = k.xf.then(svg::Affine::translate(k.dx, k.dy)).then(cam);
    k.dx = k.dy = 0;
  }
}

void draw_lander(BindCtx& c, svg::Image& img) {
  if (!g_ld.game) return;
  // fuel gauge (top left): code-drawn, the firmware's own gauge element is not identified
  const float w = 60.f;
  svg::stroke(img, {{8, 8}, {8 + w, 8}}, 3.f, kColDim, false, 1.f, false);
  svg::stroke(img, {{8, 8}, {8 + w * g_ld.fuel, 8}}, 3.f, g_ld.fuel < 0.2f ? kColRed : kColGreen, false, 1.f, false);
  c.font.draw(img, 8 + w + 6, 4, 8, "gas", kColWhite, 1.f);
}

// ================================================================ help (help.svg; HelpScreen.cpp strings: tail,
// leftpart, rightpart, leftarc, drumtail..., seqtail..., synth, drum, tape, mixer, tempo, album, browser,
// communication, clock, lander, tune, battery, debug, codec, gain, signalflow, fxon, fxoff, lfoon, lfooff,
// t%dsynth, synthpatch, synthicon, synthnumber, synthname, t%ddrum, drumpatch, drumicon, ..., t%dtape,
// t%dmixer, tapetrix, tapetrix%d, musical, musicaltext, transport, transport%d, seqname; "HelpScreen.cpp:88
// no name for edit mode %i"). One speech bubble for the mode the help key was pressed in (UiState::mode),
// with that mode's T1..T4 row; synth/drum also mark whether FX and LFO are on.
void bind_help(BindCtx& c) {
  for (svg::Node& k : const_cast<svg::Node&>(c.doc.root()).kids) k.visible = 0;
  const char* group = nullptr;
  bool fxlfo = false;
  const bool inst = c.ui.mode == Mode::Synth || c.ui.mode == Mode::Drum;
  if (inst && c.ui.page == Page::Seq) group = "seq";
  else switch (c.ui.mode) {
      case Mode::Synth: group = "synth"; fxlfo = true; break;
      case Mode::Drum: group = "drum"; fxlfo = true; break;
      case Mode::Tape: group = "tape"; break;
      case Mode::Mixer: group = "mixer"; break;
      case Mode::Tempo: group = "tempo"; break;
      case Mode::Album: group = "album"; break;
      case Mode::Browser: group = "browser"; break;
      default: group = "musical"; break;  // the other modes have no bubble of their own (ASSUMED)
    }
  show(c.n(group), true);
  if (std::string(group) == "browser") show(c.n("browser"), true);
  // patch names are text nodes the firmware fills in: the port shows the mode-word bubble instead
  show(c.n("synthname"), false);
  show(c.n("drumname"), false);
  for (const char* lp : {"leftpart", "drumleftpart"})  // the patch-number disc that goes with the name
    if (svg::Node* n = c.n(lp))
      for (svg::Node& k : n->kids)
        if (k.tag == "circle") k.visible = 0;
  if (fxlfo) {
    const InstrumentView& iv = c.inst();
    show(c.n("fxon"), iv.fx_on);
    show(c.n("fxoff"), !iv.fx_on);
    show(c.n("lfoon"), iv.lfo_on);
    show(c.n("lfooff"), !iv.lfo_on);
  }
}

// ================================================================ in (in.svg; strings next to "in.svg": line,
// radio, whatuhear (+ off, mic, usb groups)). The input source icon. The port has no input source state:
// the internal microphone (ASSUMED default) is shown.
void bind_in(BindCtx& c) { one_of(c, {"off", "line", "radio", "whatuhear", "mic", "usb"}, 4); }

// ================================================================ micline (micline.svg; mic_line_screen.cpp
// strings: micinput, micgain, miclinegain, micusbgain, micradiogain, threshold, radiomhz, outsideeu,
// whatuhear, line, radio, keytorec, input, triglevel, rec%i, inputlevel, slowinputlevel, gain, standby,
// recording, idle, recordok; asserts input < bf::kAiCount, record_pos in 0..1, s_rec_lines[line_it]).
// Idle shows the sources and the level meter; "recording!" spins rec0..15 while the tape records; "record
// ok!" for 1.5 s after it stops. No audio input in the port: the meter shows the output level (ASSUMED).
Follow g_ml;
bool g_ml_rec = false;
double g_ml_stop_t = -1e9;

void bind_micline(BindCtx& c) {
  const bool rec = c.s.tape.recording;
  if (g_ml_rec && !rec) g_ml_stop_t = c.t;
  g_ml_rec = rec;
  const bool ok = !rec && c.t - g_ml_stop_t < 1.5;
  show(c.n("idle"), true);
  show(c.n("standby"), false);
  show(c.n("recording"), rec);
  show(c.n("recordok"), ok);
  show(c.n("input"), !rec && !ok);
  // while the record state box shows, its spinner takes the place of the source icons
  if (rec || ok)
    for (const char* s : {"mic_1_", "line", "radio", "whatuhear", "usb", "mhz"}) show(c.n(s), false);
  show(c.n("keytorec"), false);
  // sources: the mic selected (blue as authored), the others dim
  for (const char* s : {"line", "radio", "whatuhear", "usb"}) tintg(c.n(s), 0x555566);
  // level meter: the line grows from the bottom, the slow peak marker follows
  constexpr float kTop = 39.6f, kBot = 113.2f;
  const float l = g_ml.step(meter01(c.s), c.t, 0.04f);
  set_line(c.n("inputlevel"), 296.2f, kBot, 296.2f, kBot - l * (kBot - kTop));
  move(c.n("slowinputlevel"), 0, -g_ml.hold * (kBot - kTop));
  // the recording spinner: one bright line runs round, the rest dim
  char id[8];
  const int head = int(c.t * 16.0) % 16;
  for (int i = 0; i < 16; ++i) {
    std::snprintf(id, sizeof id, "rec%d", i);
    const int d = (head - i + 16) % 16;
    tintg(c.n(id), d == 0 ? kColRed : (d < 4 ? 0xA02840 : 0x512132));
  }
}

// ================================================================ tapeconfig (tapeconfig.svg;
// tape_config_screen.cpp: grid, left_mid, right_mid, m_reel_left, m_reel_right, "key >= 0 && key < 4",
// m_bpm, bar_length, and the tape elements track_inactive, track_active, minute/second/frame _msd/_lsd,
// reelleftmid, reelrightmid, reelleft, reelright, whitetapeparts, erase). The erase page: "erase tape?
// press all track keys 1-4" (group txt, white) and the erasing state (group tape, red: counter, reels,
// grid, the hatched erased part, "erasing"). There is no erase command in the port, so the prompt is shown
// with the key box of the selected track lit; the erasing state's bindings (counter = tape position, reels
// turn with it, grid one bar per 4 beats) are kept for when it can be entered.
void bind_tapeconfig(BindCtx& c) {
  const bool erasing = false;
  show(c.n("txt"), !erasing);
  show(c.n("tape"), erasing);
  show(c.n("track_active"), erasing);
  show(c.n("track_inactive"), erasing);
  const TapeView& tv = c.s.tape;
  const int64_t pos = tv.position;
  const int cs = int(pos * 100 / kSampleRate);
  const int mm = cs / 6000, ss = cs / 100 % 60, ff = cs % 100;
  char t[16];
  std::snprintf(t, sizeof t, "%d%d%02d%02d", mm / 10 % 10, mm % 10, ss, ff);
  digits(c, {"minute_msd", "minute_lsd", "second_msd", "second_lsd", "frame_msd", "frame_lsd"}, t);
  const float deg = float(pos) / float(kSampleRate) * 120.f;
  const svg::Box lm = c.box("reelleftmid"), rm = c.box("reelrightmid");
  rotate(c, c.n("reelleft"), deg, lm.cx(), lm.cy());
  rotate(c, c.n("reelright"), deg, rm.cx(), rm.cy());
  const float bar = float(kSampleRate) * 4.f * beat_s(c.s);
  move(c.n("grid"), -std::fmod(float(pos), bar) * (55.363f / bar), 0);
  // the warning sign blinks
  show(c.n("_x21__4_"), std::fmod(c.t, 1.0) < 0.6);
  // track key boxes 1..4 (txt group, first child: the four key glyph groups): the selected track bright
  svg::Node* txt = c.n("txt");
  if (svg::Node* keys = kid(txt, 0)) {
    const char* ids[4] = {"_x31_", "_x32_", "_x33_", "_x34_"};
    for (int i = 0; i < 4; ++i)
      for (svg::Node& k : keys->kids)
        if (k.id == ids[i]) tintg(&k, i == (tv.track & 3) ? kColWhite : 0x6E6A86);
  }
}

// ================================================================ save (save.svg; strings next to it: synth,
// drum, save, clock, full). The "saving" pop-up: the clock arm turns while it saves, then the green tick;
// synth or drum icon by the side. "full" (out of slots) is never the case in the port.
void bind_save(BindCtx& c) {
  const bool drum = c.side() == 1;
  show(c.n("synth"), !drum);
  show(c.n("drum"), drum);
  show(c.n("full_1_"), false);
  show(c.n("full"), false);
  const double ph = std::fmod(c.t, 3.0);
  const bool done = ph > 2.0;
  show(c.n("clock"), !done);
  show(c.n("ok_1_"), done);
  show(c.n("ok"), done);
  if (!done) rotate(c, c.n("arm"), float(ph) * 360.f, 247.758f, 81.741f);
}

// ================================================================ colors (colors.svg; Colors.cpp:44 names[it],
// :47 obj, :49 fillstyle_id with the 20 colour names pink .. lightpurplegrey). The colour test pattern: the
// firmware looks up each named swatch and fills it with its palette entry, which is the authored fill.
// Bound as authored.
void bind_colors(BindCtx&) {}

// ================================================================ simple (simple.svg; strings: timing, swing,
// octave, length, skip, octavepattern, pause, none, once, each, trig, hold, n%i/s%i note heads and sharps,
// p0..p3 / a0..a3 octave dots). The arpeggio sequencer's screen (seq_engines.h ArpSeq: [4] held count,
// [5] op, [6] octave, [7..22] held notes, [2] step). Notes of the running arpeggio on the staff (sharps on
// black keys), the playing one green; octave dots; hold lit when latched.
std::vector<int> arp_sequence(const int32_t* d, int latch_octave) {
  std::vector<int> held;
  const int n = std::clamp<int>(d[4], 0, 16);
  for (int i = 0; i < n; ++i)
    if (d[7 + i] >= 0) held.push_back(d[7 + i]);
  std::vector<int> base = held;
  const int op = d[5];
  if (op == 0 || op == 2) std::sort(base.begin(), base.end());
  if (op == 1) std::sort(base.rbegin(), base.rend());
  std::vector<int> out;
  for (int o = 0; o <= latch_octave; ++o)
    for (int v : base) out.push_back(v + 12 * o);
  if (op == 2 && out.size() > 2)
    for (int i = int(out.size()) - 2; i > 0; --i) out.push_back(out[size_t(i)]);
  if (out.size() > 32) out.resize(32);
  return out;
}

int diatonic(int note, bool& sharp) {
  static const int step[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
  static const bool sh[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
  const int pc = ((note % 12) + 12) % 12;
  sharp = sh[pc];
  return (note / 12) * 7 + step[pc];
}

[[maybe_unused]] void bind_simple(BindCtx& c) {
  const int32_t* d = c.seq_display();
  const bool arp = d[0] != 0 && d[1] == int(SeqType::Arpeggio);
  const int side = c.side();
  const int octave = arp ? std::clamp<int>(d[6], 0, 3) : std::clamp(c.ui.seq_param[side][1], 0, 3);
  const bool latch = c.ui.seq_param[side][6] != 0;
  tintg(c.n("hold"), latch ? kColRed : 0x512132);
  char id[16];
  for (int i = 0; i < 4; ++i) {
    std::snprintf(id, sizeof id, "p%d", i);
    tintg(c.n(id), i <= octave ? kColWhite : 0x373672);
  }
  const std::vector<int> seq = arp ? arp_sequence(d, octave) : std::vector<int>{};
  const int n = int(seq.size());
  const int cur = (arp && d[2] >= 0 && n) ? d[2] % n : -1;
  // staff: bottom line E4 at y 101.33, 5.333 per diatonic step; authored head at (302.08, 96.04)
  const float x0 = 20.f, x1 = 300.f, pitch = n > 1 ? std::min(18.f, (x1 - x0) / float(n - 1)) : 0.f;
  bool tmp;
  const int e4 = diatonic(64, tmp);
  for (int i = 0; i < 32; ++i) {
    std::snprintf(id, sizeof id, "n%d", i);
    svg::Node* nn = c.n(id);
    std::snprintf(id, sizeof id, "s%d", i);
    svg::Node* ss = c.n(id);
    if (i >= n) { show(nn, false); show(ss, false); continue; }
    bool sharp = false;
    int dstep = diatonic(seq[size_t(i)], sharp) - e4;
    while (dstep > 10) dstep -= 7;   // fold into the staff (two octaves of positions)
    while (dstep < -2) dstep += 7;
    const float y = 101.33f - 5.333f * float(dstep);
    const float x = x0 + pitch * float(i);
    move(nn, x - 302.08f, y - 96.04f);
    tintg(nn, i == cur ? kColGreen : kColWhite);
    show(ss, sharp);
    if (sharp) move(ss, x - 10.f - 291.41f, y - 96.04f);
  }
}

// ================================================================ subscreenhand (subscreenhand.svg; strings
// near it: blue0..2, green, white, line; no screen class of its own was identified). Three pointing hands
// at encoder positions 1, 3 and 4: an overlay hint. They tap (move down) in turn (ASSUMED animation).
void bind_subscreenhand(BindCtx& c) {
  const char* ids[3] = {"_x31_", "_x33_", "_x34_"};
  const int k = int(c.t * 2.0) % 3;
  for (int i = 0; i < 3; ++i)
    move(c.n(ids[i]), 0, i == k ? 3.f : 0.f);
}

}  // namespace

OP1_SCREEN("clock", bind_clock, nullptr);
OP1_SCREEN("album", bind_album, nullptr);
OP1_SCREEN("presetbrowser", bind_presetbrowser, draw_presetbrowser);
OP1_SCREEN("com", bind_com, nullptr);
OP1_SCREEN("tune", bind_tune, draw_tune);
OP1_SCREEN("signalflow", bind_signalflow, nullptr);
OP1_SCREEN("lander", bind_lander, draw_lander);
OP1_SCREEN("help", bind_help, nullptr);
OP1_SCREEN("in", bind_in, nullptr);
OP1_SCREEN("micline", bind_micline, nullptr);
OP1_SCREEN("tapeconfig", bind_tapeconfig, nullptr);
OP1_SCREEN("save", bind_save, nullptr);
OP1_SCREEN("colors", bind_colors, nullptr);
// "simple" (the arpeggio) is bound in bind_seq_tempo.cpp, which draws its values; this earlier version is kept unregistered.
OP1_SCREEN("subscreenhand", bind_subscreenhand, nullptr);

}  // namespace op1
