// A small SVG renderer for the OG screen templates (owner: t6). The firmware draws its screens from SVG
// files that it moves, shows, hides and recolours at run time (firmware-behaviour.md §10, Screen.cpp /
// svg_loader.cpp with AGG). This is our own implementation of the subset those files use: g, path
// (M L H V C S Q T Z, absolute and relative), line, polyline, polygon, rect, circle, ellipse, transform,
// fill/stroke/stroke-width/linecap/linejoin/dasharray, opacity and display. Anti-aliased scanline fill
// with 4 sub-scanlines and exact horizontal coverage. No text (the screens contain none), no clip-path
// (3 uses in the 61 files, ignored), no gradients (none used).
//
// The SVG text itself comes from the user's firmware image through AssetProvider at run time; nothing
// here contains Teenage Engineering artwork.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace op1 {
namespace svg {

struct Image {
  int w = 0, h = 0;
  std::vector<uint32_t> px;  // 0xAARRGGBB, opaque
  Image() = default;
  Image(int w_, int h_, uint32_t c = 0xFF000000u) : w(w_), h(h_), px(size_t(w_) * size_t(h_), c) {}
  uint32_t& at(int x, int y) { return px[size_t(y) * size_t(w) + size_t(x)]; }
};

// An axis-aligned box in some coordinate space. Empty when x0 > x1.
struct Box {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  bool empty() const { return x0 > x1 || y0 > y1; }
  float w() const { return empty() ? 0 : x1 - x0; }
  float h() const { return empty() ? 0 : y1 - y0; }
  float cx() const { return (x0 + x1) / 2; }
  float cy() const { return (y0 + y1) / 2; }
  void add(float x, float y) { x0 = x < x0 ? x : x0; y0 = y < y0 ? y : y0; x1 = x > x1 ? x : x1; y1 = y > y1 ? y : y1; }
  void add(const Box& b) { if (!b.empty()) { add(b.x0, b.y0); add(b.x1, b.y1); } }
};

// 2x3 affine matrix, SVG order (a b c d e f): x' = a x + c y + e, y' = b x + d y + f.
struct Affine {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  static Affine translate(float x, float y) { return {1, 0, 0, 1, x, y}; }
  static Affine scale(float sx, float sy, float cx = 0, float cy = 0) { return {sx, 0, 0, sy, cx - sx * cx, cy - sy * cy}; }
  static Affine rotate(float deg, float cx = 0, float cy = 0);
  Affine then(const Affine& m) const;  // this first, then m
  bool identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0; }
};

struct Node {
  std::string tag, id;
  std::vector<std::pair<std::string, std::string>> attr;
  std::vector<Node> kids;
  Node* parent = nullptr;
  // run-time overrides, the way the firmware drives its templates (all cleared by reset_overrides)
  int visible = -1;          // -1 = as authored, 0 = hide, 1 = show
  float dx = 0, dy = 0;      // extra translation
  bool tinted = false;
  uint32_t tint = 0;         // replaces fill and stroke colours (RGB) of this subtree
  float alpha = 1;           // multiplies the authored opacity
  Affine xf;                 // extra transform in the node's own (inner) coordinates, applied after dx/dy
  std::vector<std::pair<std::string, std::string>> over;  // attribute overrides (e.g. a new "d" or "points")
  const Node* glyph = nullptr;  // draw this geometry (a font glyph) instead of the node's, in the node's style
  Affine glyph_xf;              // glyph coordinates -> the node's inner coordinates
  const std::string* get(const char* name) const;
  void set(const char* name, std::string value);  // attribute override
};

// Strips the Illustrator export decorations from an id: "_x5F_" -> "_", "_x3C_" -> "<", and a trailing
// copy counter "_12_". "frame_x5F_lsd" -> "frame_lsd", "box0_1_" -> "box0". The firmware asks for the
// clean names (strings next to each screen in the binary), so lookups use them.
std::string clean_id(const std::string& id);

class Document {
 public:
  Document() = default;
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  bool load(const std::string& text, std::string& err);
  // Draws the document scaled to fit dst (the templates are 320x160). Clears dst to black first.
  void draw(Image& dst) const;
  // Raw id first, then the clean name (first match in document order).
  Node* find(const std::string& id);
  // Every node whose clean name is `name`, in document order.
  std::vector<Node*> find_all(const std::string& name);
  // Geometry bounds (without stroke width) of a node's content. inner: in the node's own coordinates
  // (where xf applies); otherwise in document (viewBox) coordinates.
  Box bounds(const Node* n, bool inner = false) const;
  // Maps a point in a node's inner coordinates to document coordinates.
  void to_doc(const Node* n, float& x, float& y) const;
  // Document -> display pixel scale of draw() for a dst of w x h.
  float px_scale(int w, int h) const;
  const Node& root() const { return root_; }
  size_t shapes() const { return shapes_; }  // drawable elements, a cheap "did anything parse" check
  void reset_overrides();

 private:
  Node root_;
  std::map<std::string, Node*> ids_;
  std::multimap<std::string, Node*> clean_;
  size_t shapes_ = 0;
};

// The firmware's vector font (opfont.svg): one glyph per character id, on a grid of cells. Font.cpp in the
// firmware does the same lookup ("Two font symbols with the same name: %c Repair font svg!").
class Font {
 public:
  bool load(Document& doc);  // keeps pointers into doc: doc must outlive the font
  bool ok() const { return !glyphs_.empty(); }
  bool has(char c) const;
  // Puts glyph c into slot (a digit/letter path or group of a screen), sized and placed like the glyph
  // authored there: same height and baseline, left edge of its cell at the authored glyph's left edge.
  // The slot keeps its own stroke colour and width. Returns false if there is no such glyph.
  bool set(Document& screen, Node* slot, char c, float cell_cx = 1e30f) const;
  // Width of a glyph cell for a slot of this height (a digit's width), in the slot's units.
  float cell_w(float slot_h) const { return pitch_ * 0.5f * slot_h / cap_; }
  // Draws text directly onto an image in display pixels (top-left x, y, cap height h), for screens
  // whose numbers are not template slots. Returns the advance width.
  float draw(Image& dst, float x, float y, float h, const std::string& s, uint32_t rgb, float width = 1.5f) const;

 private:
  // metric: the glyph carries a yellow (#FFFF00) advance box in opfont.svg (".", "-", "+", ":", some
  // letters). The box is not drawn; its width (mw, from its left edge mx0) is the glyph's advance.
  struct G { const Node* n; Box b; float cell_x; float base; bool metric = false; float mx0 = 0, mw = 0; };
  std::map<char, G> glyphs_;
  std::deque<Node> owned_;  // copies of the metric glyphs without their advance box (G::n points here)
  Document* doc_ = nullptr;
  float cap_ = 56.69f, pitch_ = 56.693f, row_ = 85.04f, origin_x_ = -9.917f, origin_base_ = 84.334f;
};

// Procedural drawing in display pixels, with the renderer's anti-aliasing (for the parts the firmware
// draws by code: waveforms, scopes, meters, envelopes).
using Points = std::vector<std::pair<float, float>>;
void stroke(Image& dst, const Points& pts, float width, uint32_t rgb, bool closed = false, float opacity = 1, bool round = true);
void fill(Image& dst, const Points& pts, uint32_t rgb, float opacity = 1);
void circle(Image& dst, float cx, float cy, float r, uint32_t rgb, bool filled, float width = 1.5f, float opacity = 1);

}  // namespace svg
}  // namespace op1
