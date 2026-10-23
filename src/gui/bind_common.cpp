// Shared helpers and the registry for the live screen bindings (owner: t6). See bind.h.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "bind.h"

namespace op1 {

namespace {
std::map<std::string, ScreenBinding>& registry() {
  static std::map<std::string, ScreenBinding> r;  // function-local: safe from static-init order
  return r;
}

std::string fmt_points(const svg::Points& pts) {
  std::string s;
  char b[48];
  for (const auto& p : pts) {
    std::snprintf(b, sizeof b, "%.2f,%.2f ", p.first, p.second);
    s += b;
  }
  return s;
}
// Cell centres of a row of digit slots (left to right). Templates set numbers with their own spacing, and a
// narrow glyph such as '1' sits wherever the designer put it, so the cells come from the wide glyphs: their
// centres and the pitch between them. Slots without that information keep the font's own placement.
std::vector<float> cell_centres(BindCtx& c, const std::vector<svg::Node*>& slots) {
  const size_t n = slots.size();
  std::vector<float> out(n, 1e30f);
  std::vector<svg::Box> b(n);
  for (size_t i = 0; i < n; ++i) b[i] = c.doc.bounds(slots[i], true);
  std::vector<size_t> wide;
  for (size_t i = 0; i < n; ++i)
    if (!b[i].empty() && b[i].w() >= 0.6f * c.font.cell_w(b[i].h())) wide.push_back(i);
  if (wide.empty()) return out;
  float pitch = 0;
  if (wide.size() >= 2) pitch = (b[wide.back()].cx() - b[wide.front()].cx()) / float(wide.back() - wide.front());
  else pitch = c.font.cell_w(b[wide[0]].h()) * 1.25f;
  const size_t k = wide.front();
  for (size_t i = 0; i < n; ++i) {
    if (b[i].empty()) continue;
    const bool is_wide = std::find(wide.begin(), wide.end(), i) != wide.end();
    out[i] = is_wide ? b[i].cx() : b[k].cx() + pitch * (float(i) - float(k));
  }
  return out;
}
}  // namespace

int register_binding(const ScreenBinding& b) {
  registry()[b.name] = b;
  return 1;
}

const ScreenBinding* find_binding(const std::string& name) {
  const auto& r = registry();
  const auto it = r.find(name);
  return it == r.end() ? nullptr : &it->second;
}

std::vector<std::string> bound_screens() {
  std::vector<std::string> v;
  for (const auto& kv : registry()) v.push_back(kv.first);
  return v;
}

void show(svg::Node* n, bool on) {
  if (n) n->visible = on ? 1 : 0;
}

void tint(svg::Node* n, uint32_t rgb) {
  if (!n) return;
  n->tinted = true;
  n->tint = rgb & 0xFFFFFFu;
}

void rotate(BindCtx& c, svg::Node* n, float deg, float cx, float cy) {
  if (!n) return;
  if (std::isnan(cx) || std::isnan(cy)) {
    const svg::Box b = c.doc.bounds(n, true);
    if (b.empty()) return;
    cx = b.cx();
    cy = b.cy();
  }
  n->xf = n->xf.then(svg::Affine::rotate(deg, cx, cy));
}

void scale(svg::Node* n, float sx, float sy, float cx, float cy) {
  if (n) n->xf = n->xf.then(svg::Affine::scale(sx, sy, cx, cy));
}

void move(svg::Node* n, float dx, float dy) {
  if (!n) return;
  n->dx += dx;
  n->dy += dy;
}

void set_line(svg::Node* n, float x1, float y1, float x2, float y2) {
  if (!n) return;
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", x1); n->set("x1", b);
  std::snprintf(b, sizeof b, "%.2f", y1); n->set("y1", b);
  std::snprintf(b, sizeof b, "%.2f", x2); n->set("x2", b);
  std::snprintf(b, sizeof b, "%.2f", y2); n->set("y2", b);
}

void set_points(svg::Node* n, const svg::Points& pts) {
  if (n) n->set("points", fmt_points(pts));
}

void set_path(svg::Node* n, const svg::Points& pts, bool closed) {
  if (!n || pts.empty()) return;
  std::string d;
  char b[48];
  for (size_t i = 0; i < pts.size(); ++i) {
    std::snprintf(b, sizeof b, "%c%.2f,%.2f", i ? 'L' : 'M', pts[i].first, pts[i].second);
    d += b;
  }
  if (closed) d += "Z";
  n->set("d", d);
}

void digits(BindCtx& c, std::initializer_list<const char*> slots, const std::string& text) {
  std::vector<svg::Node*> nodes;
  for (const char* id : slots) nodes.push_back(c.n(id));
  std::vector<svg::Node*> present;
  for (svg::Node* n : nodes)
    if (n) present.push_back(n);
  const std::vector<float> cx = c.font.ok() ? cell_centres(c, present) : std::vector<float>(present.size(), 1e30f);
  size_t i = 0, k = 0;
  for (svg::Node* n : nodes) {
    const char ch = i < text.size() ? text[i] : ' ';
    ++i;
    if (!n) continue;
    const float x = cx[k++];
    if (!c.font.ok()) continue;  // no font: leave the authored glyph
    if (!c.font.set(c.doc, n, ch, x)) n->visible = 0;
  }
}

std::vector<svg::Node*> slots_lr(BindCtx& c, svg::Node* group) {
  std::vector<std::pair<float, svg::Node*>> v;
  if (!group) return {};
  for (svg::Node& k : group->kids) {
    const svg::Box b = c.doc.bounds(&k, false);
    if (!b.empty()) v.emplace_back(b.cx(), &k);
  }
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<svg::Node*> out;
  for (auto& p : v) out.push_back(p.second);
  return out;
}

void text_into(BindCtx& c, const std::vector<svg::Node*>& slots, const std::string& text) {
  const size_t n = slots.size(), off = text.size() < n ? n - text.size() : 0;
  const std::vector<float> cx = c.font.ok() ? cell_centres(c, slots) : std::vector<float>(n, 1e30f);
  for (size_t i = 0; i < n; ++i) {
    svg::Node* s = slots[i];
    if (i < off) { s->visible = 0; continue; }
    const char ch = text[i - off + (text.size() > n ? text.size() - n : 0)];
    if (!c.font.ok() || !c.font.set(c.doc, s, ch, cx[i])) s->visible = 0;
  }
}

void one_of(BindCtx& c, std::initializer_list<const char*> ids, int i) {
  int k = 0;
  for (const char* id : ids) {
    for (svg::Node* n : c.all(id)) show(n, k == i);
    ++k;
  }
}

int recent_enc(const BindCtx& c, double window) {
  int best = -1;
  double bt = window;
  for (int i = 0; i < 4; ++i) {
    const double d = c.t - c.ui.enc_t[i];
    if (d >= 0 && d < bt) bt = d, best = i;
  }
  return best;
}

void draw_wave(svg::Image& img, const float* v, int n, float x, float y, float w, float h, uint32_t rgb, float width) {
  if (n < 2) return;
  svg::Points pts;
  pts.reserve(size_t(n));
  for (int i = 0; i < n; ++i) pts.emplace_back(x + w * float(i) / float(n - 1), y + h * 0.5f * (1.f - std::clamp(v[i], -1.f, 1.f)));
  svg::stroke(img, pts, width, rgb);
}

void draw_scope(svg::Image& img, const CoreSnapshot& s, float x, float y, float w, float h, uint32_t rgb, float gain) {
  const int n = std::max(2, int(w));
  std::vector<float> v(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) v[size_t(i)] = float(s.scope[i * (kDisplayW - 1) / (n - 1)]) / 2147483648.f * gain;
  draw_wave(img, v.data(), n, x, y, w, h, rgb);
}

}  // namespace op1
