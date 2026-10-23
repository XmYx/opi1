// Shared helpers of the synth engine screens (src/gui/engines/*.cpp, one file per firmware screen
// template; moved from bind_engines1.cpp and bind_engines2.cpp by t3). Engine state comes from
// InstrumentView::display (instrument_base.h slots 0..15, src/dsp/eng_*.cpp header comments for 16..) and
// the stored knobs (knobs[0..3] = blue, green, white, orange; 4..7 the shift page).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../bind.h"

namespace op1 {
namespace eng {


inline constexpr float kPi = 3.14159265f;

// ---- shared helpers for the engine screens

// Loudest voice envelope 0..1 (display[6..11], Q15); 0 when nothing sounds.
inline float voice_env(const InstrumentView& iv) {
  int m = 0;
  for (int v = 0; v < kSynthVoices; ++v)
    if ((iv.voices_active >> v) & 1) m = std::max(m, int(iv.display[6 + v]));
  return clamp01(float(m) / 32767.f);
}
inline bool sounding(const InstrumentView& iv) { return iv.voices_active != 0; }
inline float knob(const InstrumentView& iv, int i) { return q15(iv.knobs[i & 7]); }

// "#RRGGBB" -> 0xRRGGBB (0 if absent).
inline uint32_t colour_of(const svg::Node* n, const char* attr = "stroke") {
  if (!n) return 0;
  const std::string* s = n->get(attr);
  if (!s || s->size() < 7 || (*s)[0] != '#') return 0;
  return uint32_t(std::strtoul(s->c_str() + 1, nullptr, 16));
}

// The authored points of a polyline/polygon.
inline svg::Points points_of(const svg::Node* n) {
  svg::Points p;
  if (!n) return p;
  const std::string* s = nullptr;
  for (const auto& kv : n->attr)
    if (kv.first == "points") s = &kv.second;  // authored, not an earlier override
  if (!s) return p;
  const char* c = s->c_str();
  char* end = nullptr;
  for (;;) {
    const float x = std::strtof(c, &end);
    if (end == c) break;
    c = end;
    while (*c == ',' || *c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') ++c;
    const float y = std::strtof(c, &end);
    if (end == c) break;
    c = end;
    p.emplace_back(x, y);
  }
  return p;
}

inline float attr_f(const svg::Node* n, const char* name, float dflt = 0) {
  if (!n) return dflt;
  for (const auto& kv : n->attr)
    if (kv.first == name) return std::strtof(kv.second.c_str(), nullptr);
  return dflt;
}

// tint() on a group does not reach children that carry their own stroke/fill (svg.cpp apply_style
// re-reads each child's colours), so recolour every node of the subtree.
inline void tint_tree(svg::Node* n, uint32_t rgb) {
  if (!n) return;
  tint(n, rgb);
  for (svg::Node& k : n->kids) tint_tree(&k, rgb);
}

// Colour between a and b (t 0..1).
inline uint32_t mix_rgb(uint32_t a, uint32_t b, float t) {
  uint32_t o = 0;
  for (int sh = 0; sh < 24; sh += 8) {
    const float x = float((a >> sh) & 255), y = float((b >> sh) & 255);
    o |= uint32_t(std::lround(x + (y - x) * clamp01(t))) << sh;
  }
  return o;
}

// Snaps v to a grid of `step` (step <= 1: unchanged).
inline float snap(float v, float step) { return step > 1.f ? std::round(v / step) * step : v; }



inline constexpr uint32_t kEncCol[4] = {kColBlue, kColGreen, kColWhite, kColRed};
inline constexpr uint32_t kDimGrey = 0x514959, kDimTeal = 0x00676C, kBrightGreen = 0x00EE92, kRingWhite = 0xDED7FF,
                   kDimRed = 0x522132, kDialDim = 0x38354D, kMarkerDim = 0x272F66;

// An authored attribute as a number (0 if missing).
inline float attr(const svg::Node* n, const char* a) {
  if (!n) return 0;
  const std::string* v = n->get(a);
  return v ? std::strtof(v->c_str(), nullptr) : 0.f;
}

// Output peak of the last block, 0..1 (instrument_base.h display[14], Q15).
inline float peak(const InstrumentView& iv) { return clamp01(float(iv.display[14]) / 32767.f); }

// Right-aligned text with the firmware font (measured on a scratch image first).
inline void text_right(const svg::Font& f, svg::Image& img, float right, float top, float h, const std::string& s,
                uint32_t rgb, float width = 1.5f) {
  static svg::Image scratch(64, 16);
  const float w = f.draw(scratch, 0, 0, h, s, rgb, width);
  f.draw(img, right - w, top, h, s, rgb, width);
}

// Recolours only the stroke (tint() would also paint a black-filled box).
inline void stroke_col(svg::Node* n, uint32_t rgb) {
  if (!n) return;
  char b[8];
  std::snprintf(b, sizeof b, "#%06X", unsigned(rgb & 0xFFFFFFu));
  n->set("stroke", b);
}


inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}


}  // namespace eng
}  // namespace op1
