// Live screen bindings (owner: t6). The firmware's screen classes (TapeScreen.cpp, TempoScreen.cpp,
// PlsScreen.cpp, ... in src/gui/screens of the binary) look up named elements of their SVG template and
// move, rotate, hide, recolour or re-path them from engine state each frame, and draw some parts by code
// (dr_wave_screen.cpp, the sampler wave). This header is our equivalent: one ScreenBinding per template
// name, registered from the bind_*.cpp files with OP1_SCREEN(...). The per-screen element map with its
// evidence is docs/ui-bindings.md in the shared folder.
//
// A binding has two halves, both called on the UI thread every frame the screen is shown:
//   bind(ctx)        sets node overrides (svg::Node: visible, tint, alpha, xf, dx/dy, set("d", ...), glyph).
//                    Overrides are cleared before each call, so a binding always describes the full state.
//                    The document is re-rasterised only when the overrides change.
//   draw(ctx, img)   draws code-drawn parts on top of the raster, in display pixels (320 x 160).
// Neither may block; they read the snapshot, which the audio thread publishes lock-free.
#pragma once

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "op1/config.h"
#include "op1/snapshot.h"
#include "svg.h"
#include "ui_model.h"

namespace op1 {

// The template colours (as authored in the firmware's SVGs).
inline constexpr uint32_t kColBlue = 0x698EFF, kColGreen = 0x00ED95, kColWhite = 0xFFFFFF, kColRed = 0xFF3A5D,
                          kColOrange = 0xFF8C00, kColGrey = 0x656579, kColDark = 0x2B2338, kColDim = 0x3B2D49;

struct BindCtx {
  svg::Document& doc;
  const svg::Font& font;           // the firmware's opfont (empty if the image has none)
  const CoreSnapshot& s;
  const UiState& ui;
  double t;                        // seconds (UI clock)
  const std::string& name;         // template name
  // the side the screen belongs to: 0 synth, 1 drum (instrument pages, sequencer)
  int side() const { return (ui.mode == Mode::Drum || (ui.mode != Mode::Synth && ui.inst_mode == Mode::Drum)) ? 1 : 0; }
  const InstrumentView& inst() const { return side() ? s.drum : s.synth; }
  const int32_t* seq_display() const { return s.seq.display + 32 * side(); }

  // ---- lookups (clean names: "frame_lsd" finds id "frame_x5F_lsd")
  svg::Node* n(const std::string& id) const { return doc.find(id); }
  std::vector<svg::Node*> all(const std::string& id) const { return doc.find_all(id); }
  svg::Box box(const std::string& id) const { return doc.bounds(doc.find(id), true); }
};

// ---- small helpers for bindings
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float q15(int v) { return clamp01(float(v) / 32767.f); }   // a stored parameter 0..32767 as 0..1
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

void show(svg::Node* n, bool on);
void tint(svg::Node* n, uint32_t rgb);
// Rotates n by deg (clockwise on screen) about (cx, cy) in its inner coordinates; NaN = centre of its bounds.
void rotate(BindCtx& c, svg::Node* n, float deg, float cx = NAN, float cy = NAN);
// Scales n about (cx, cy) in its inner coordinates.
void scale(svg::Node* n, float sx, float sy, float cx, float cy);
void move(svg::Node* n, float dx, float dy);
// Replaces a line's end points / a polyline's points / a path (attribute overrides).
void set_line(svg::Node* n, float x1, float y1, float x2, float y2);
void set_points(svg::Node* n, const svg::Points& pts);
void set_path(svg::Node* n, const svg::Points& pts, bool closed = false);
// Writes text into digit slots (one character per slot, left to right). ' ' hides a slot.
void digits(BindCtx& c, std::initializer_list<const char*> slots, const std::string& text);
// The drawable children of a group, left to right (by the centre of their bounds): digit slots of a number.
std::vector<svg::Node*> slots_lr(BindCtx& c, svg::Node* group);
// Writes text right-aligned into slots (left to right); unused leading slots are hidden.
void text_into(BindCtx& c, const std::vector<svg::Node*>& slots, const std::string& text);
// Shows exactly one of `ids` (index i; -1 = none).
void one_of(BindCtx& c, std::initializer_list<const char*> ids, int i);
// Seconds since encoder i last turned (for value pop-ups); large if never.
inline double since_enc(const BindCtx& c, int i) { return c.t - c.ui.enc_t[i & 3]; }
// Most recent encoder turned within `window` seconds, else -1.
int recent_enc(const BindCtx& c, double window = 1.5);

// Procedural drawing helpers (display pixels).
// A waveform of n samples (values -1..1) across the box (x, y, w, h).
void draw_wave(svg::Image& img, const float* v, int n, float x, float y, float w, float h, uint32_t rgb, float width = 1.5f);
// The last kDisplayW output samples as a scope in the box.
void draw_scope(svg::Image& img, const CoreSnapshot& s, float x, float y, float w, float h, uint32_t rgb, float gain = 1.f);

using BindFn = void (*)(BindCtx&);
using DrawFn = void (*)(BindCtx&, svg::Image&);

struct ScreenBinding {
  const char* name;
  BindFn bind;
  DrawFn draw;  // may be null
};

const ScreenBinding* find_binding(const std::string& name);
int register_binding(const ScreenBinding& b);
std::vector<std::string> bound_screens();

#define OP1_SCREEN_CAT2(a, b) a##b
#define OP1_SCREEN_CAT(a, b) OP1_SCREEN_CAT2(a, b)
// OP1_SCREEN("tape", bind_tape, draw_tape);   (draw may be nullptr)
#define OP1_SCREEN(name, bind, draw) \
  [[maybe_unused]] static const int OP1_SCREEN_CAT(op1_screen_reg_, __LINE__) = ::op1::register_binding({name, bind, draw})

}  // namespace op1
