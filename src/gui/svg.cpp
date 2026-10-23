#include "svg.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace op1 {
namespace svg {
namespace {

struct Pt { float x, y; };
using Poly = std::vector<Pt>;

struct Mat {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  Pt apply(Pt p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
  Mat then(const Mat& m) const {  // this applied first, then m
    return {m.a * a + m.c * b, m.b * a + m.d * b, m.a * c + m.c * d, m.b * c + m.d * d,
            m.a * e + m.c * f + m.e, m.b * e + m.d * f + m.f};
  }
  float scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
};
Mat mat(const Affine& x) { return {x.a, x.b, x.c, x.d, x.e, x.f}; }

// ------------------------------------------------------------------ XML

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_name(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.'; }

struct XmlParser {
  const std::string& s;
  size_t i = 0;
  std::string err;
  explicit XmlParser(const std::string& t) : s(t) {}

  bool skip_to(const char* tok) {
    const size_t p = s.find(tok, i);
    if (p == std::string::npos) return false;
    i = p + std::strlen(tok);
    return true;
  }
  // Parses the content of an element up to its end tag. Returns false on malformed input.
  bool parse_children(Node& parent, int depth) {
    if (depth > 64) { err = "svg: nesting too deep"; return false; }
    while (i < s.size()) {
      const size_t lt = s.find('<', i);
      if (lt == std::string::npos) return true;
      i = lt + 1;
      if (i >= s.size()) return true;
      if (s.compare(i, 3, "!--") == 0) { if (!skip_to("-->")) return false; continue; }
      if (s[i] == '?') { if (!skip_to("?>")) return false; continue; }
      if (s[i] == '!') {  // DOCTYPE, possibly with an internal subset
        int br = 0;
        while (i < s.size() && !(s[i] == '>' && br == 0)) { if (s[i] == '[') ++br; if (s[i] == ']') --br; ++i; }
        ++i;
        continue;
      }
      if (s[i] == '/') { skip_to(">"); return true; }  // end tag of the parent
      Node n;
      const size_t b = i;
      while (i < s.size() && is_name(s[i])) ++i;
      n.tag = s.substr(b, i - b);
      bool self_close = false;
      for (;;) {
        while (i < s.size() && is_space(s[i])) ++i;
        if (i >= s.size()) { err = "svg: unexpected end inside a tag"; return false; }
        if (s[i] == '/') { self_close = true; ++i; continue; }
        if (s[i] == '>') { ++i; break; }
        const size_t nb = i;
        while (i < s.size() && is_name(s[i])) ++i;
        if (i == nb) { err = "svg: bad attribute"; return false; }
        std::string name = s.substr(nb, i - nb);
        while (i < s.size() && is_space(s[i])) ++i;
        if (i >= s.size() || s[i] != '=') { err = "svg: attribute without value"; return false; }
        ++i;
        while (i < s.size() && is_space(s[i])) ++i;
        if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) { err = "svg: unquoted attribute"; return false; }
        const char q = s[i++];
        const size_t vb = i;
        while (i < s.size() && s[i] != q) ++i;
        if (i >= s.size()) { err = "svg: unterminated attribute"; return false; }
        n.attr.emplace_back(std::move(name), s.substr(vb, i - vb));
        ++i;
      }
      if (const std::string* id = n.get("id")) n.id = *id;
      if (!self_close && !parse_children(n, depth + 1)) return false;
      parent.kids.push_back(std::move(n));
    }
    return true;
  }
};

// ------------------------------------------------------------------ attribute helpers

float num(const std::string* v, float def) { return v ? std::strtof(v->c_str(), nullptr) : def; }

std::vector<float> floats(const std::string& s) {
  std::vector<float> out;
  const char* p = s.c_str();
  for (;;) {
    while (*p && (is_space(*p) || *p == ',')) ++p;
    if (!*p) break;
    char* e = nullptr;
    const float v = std::strtof(p, &e);
    if (e == p) { ++p; continue; }
    out.push_back(v);
    p = e;
  }
  return out;
}

Mat parse_transform(const std::string& s) {
  Mat m;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (is_space(s[i]) || s[i] == ',')) ++i;
    const size_t b = i;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
    const std::string fn = s.substr(b, i - b);
    const size_t open = s.find('(', i), close = s.find(')', i);
    if (fn.empty() || open == std::string::npos || close == std::string::npos) break;
    const std::vector<float> v = floats(s.substr(open + 1, close - open - 1));
    Mat t;
    if (fn == "matrix" && v.size() >= 6) t = {v[0], v[1], v[2], v[3], v[4], v[5]};
    else if (fn == "translate" && !v.empty()) t.e = v[0], t.f = v.size() > 1 ? v[1] : 0;
    else if (fn == "scale" && !v.empty()) t.a = v[0], t.d = v.size() > 1 ? v[1] : v[0];
    else if (fn == "rotate" && !v.empty()) {
      const float r = v[0] * 3.14159265f / 180.f, cs = std::cos(r), sn = std::sin(r);
      t = {cs, sn, -sn, cs, 0, 0};
      if (v.size() >= 3) t.e = v[1] - cs * v[1] + sn * v[2], t.f = v[2] - sn * v[1] - cs * v[2];
    }
    m = t.then(m);  // the list is a product read left to right: the last entry applies to the point first
    i = close + 1;
  }
  return m;
}

bool parse_color(const std::string& s, uint32_t& rgb) {
  if (s.empty() || s == "none" || s.compare(0, 3, "url") == 0) return false;
  if (s[0] == '#') {
    const unsigned long v = std::strtoul(s.c_str() + 1, nullptr, 16);
    if (s.size() == 4) rgb = uint32_t(((v >> 8 & 0xF) * 17) << 16 | ((v >> 4 & 0xF) * 17) << 8 | (v & 0xF) * 17);
    else rgb = uint32_t(v & 0xFFFFFF);
    return true;
  }
  static const struct { const char* n; uint32_t c; } names[] = {
      {"white", 0xFFFFFF}, {"black", 0}, {"red", 0xFF0000}, {"green", 0x008000}, {"blue", 0x0000FF},
      {"yellow", 0xFFFF00}, {"gray", 0x808080}, {"grey", 0x808080}};
  for (const auto& n : names)
    if (s == n.n) { rgb = n.c; return true; }
  return false;
}

// ------------------------------------------------------------------ paths

void cubic(Poly& out, Pt p0, Pt p1, Pt p2, Pt p3, float scale) {
  const float len = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y) + std::hypot(p3.x - p2.x, p3.y - p2.y);
  const int n = std::clamp(int(len * scale / 2.0f) + 4, 6, 48);
  for (int k = 1; k <= n; ++k) {
    const float t = float(k) / float(n), u = 1 - t;
    out.push_back({u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
                   u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y});
  }
}

struct SubPath {
  Poly pts;
  bool closed = false;
};

std::vector<SubPath> parse_path(const std::string& d, float scale) {
  std::vector<SubPath> out;
  const char* p = d.c_str();
  char cmd = 0;
  Pt cur{0, 0}, start{0, 0}, last_c{0, 0};
  char prev = 0;
  auto number = [&](float& v) {
    while (*p && (is_space(*p) || *p == ',')) ++p;
    char* e = nullptr;
    v = std::strtof(p, &e);
    if (e == p) return false;
    p = e;
    return true;
  };
  auto begin = [&](Pt at) {
    out.emplace_back();
    out.back().pts.push_back(at);
  };
  for (;;) {
    while (*p && (is_space(*p) || *p == ',')) ++p;
    if (!*p) break;
    if (std::isalpha(static_cast<unsigned char>(*p))) cmd = *p++;
    else if (!cmd) break;
    const bool rel = std::islower(static_cast<unsigned char>(cmd));
    const char up = char(std::toupper(static_cast<unsigned char>(cmd)));
    float v[7];
    auto want = [&](int n) {
      for (int k = 0; k < n; ++k) if (!number(v[k])) return false;
      return true;
    };
    if (up == 'Z') {
      if (!out.empty()) out.back().closed = true;
      cur = start;
      prev = 'Z';
      cmd = 0;
      continue;
    }
    if (out.empty() && up != 'M') break;
    switch (up) {
      case 'M': {
        if (!want(2)) return out;
        cur = rel ? Pt{cur.x + v[0], cur.y + v[1]} : Pt{v[0], v[1]};
        start = cur;
        begin(cur);
        cmd = rel ? 'l' : 'L';  // further pairs are implicit line-tos
        break;
      }
      case 'L': {
        if (!want(2)) return out;
        cur = rel ? Pt{cur.x + v[0], cur.y + v[1]} : Pt{v[0], v[1]};
        if (out.back().closed) begin(start);
        out.back().pts.push_back(cur);
        break;
      }
      case 'H': {
        if (!want(1)) return out;
        cur.x = rel ? cur.x + v[0] : v[0];
        if (out.back().closed) begin(start);
        out.back().pts.push_back(cur);
        break;
      }
      case 'V': {
        if (!want(1)) return out;
        cur.y = rel ? cur.y + v[0] : v[0];
        if (out.back().closed) begin(start);
        out.back().pts.push_back(cur);
        break;
      }
      case 'C': case 'S': {
        Pt c1, c2, e;
        if (up == 'C') {
          if (!want(6)) return out;
          c1 = rel ? Pt{cur.x + v[0], cur.y + v[1]} : Pt{v[0], v[1]};
          c2 = rel ? Pt{cur.x + v[2], cur.y + v[3]} : Pt{v[2], v[3]};
          e = rel ? Pt{cur.x + v[4], cur.y + v[5]} : Pt{v[4], v[5]};
        } else {
          if (!want(4)) return out;
          c1 = (prev == 'C' || prev == 'S') ? Pt{2 * cur.x - last_c.x, 2 * cur.y - last_c.y} : cur;
          c2 = rel ? Pt{cur.x + v[0], cur.y + v[1]} : Pt{v[0], v[1]};
          e = rel ? Pt{cur.x + v[2], cur.y + v[3]} : Pt{v[2], v[3]};
        }
        if (out.back().closed) begin(start);
        cubic(out.back().pts, cur, c1, c2, e, scale);
        last_c = c2;
        cur = e;
        break;
      }
      case 'Q': case 'T': {
        Pt q, e;
        if (up == 'Q') {
          if (!want(4)) return out;
          q = rel ? Pt{cur.x + v[0], cur.y + v[1]} : Pt{v[0], v[1]};
          e = rel ? Pt{cur.x + v[2], cur.y + v[3]} : Pt{v[2], v[3]};
        } else {
          if (!want(2)) return out;
          q = (prev == 'Q' || prev == 'T') ? Pt{2 * cur.x - last_c.x, 2 * cur.y - last_c.y} : cur;
          e = rel ? Pt{cur.x + v[0], cur.y + v[1]} : Pt{v[0], v[1]};
        }
        if (out.back().closed) begin(start);
        cubic(out.back().pts, cur, {cur.x + 2.f / 3 * (q.x - cur.x), cur.y + 2.f / 3 * (q.y - cur.y)},
              {e.x + 2.f / 3 * (q.x - e.x), e.y + 2.f / 3 * (q.y - e.y)}, e, scale);
        last_c = q;
        cur = e;
        break;
      }
      case 'A': {  // not used by the firmware's files; approximated by a straight line to the end point
        if (!want(7)) return out;
        cur = rel ? Pt{cur.x + v[5], cur.y + v[6]} : Pt{v[5], v[6]};
        if (out.back().closed) begin(start);
        out.back().pts.push_back(cur);
        break;
      }
      default:
        return out;
    }
    prev = up;
  }
  return out;
}

// ------------------------------------------------------------------ rasterizer

constexpr int kSub = 4;

struct Edge { float x0, y0, x1, y1; int dir; };

void fill_polys(Image& img, const std::vector<Poly>& polys, bool even_odd, uint32_t rgb, float alpha) {
  if (alpha <= 0) return;
  std::vector<Edge> edges;
  float ymin = 1e30f, ymax = -1e30f, xmin = 1e30f, xmax = -1e30f;
  for (const Poly& pl : polys) {
    const size_t n = pl.size();
    if (n < 3) continue;
    for (size_t i = 0; i < n; ++i) {
      const Pt a = pl[i], b = pl[(i + 1) % n];
      xmin = std::min({xmin, a.x, b.x});
      xmax = std::max({xmax, a.x, b.x});
      ymin = std::min({ymin, a.y, b.y});
      ymax = std::max({ymax, a.y, b.y});
      if (a.y == b.y) continue;
      edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y, 1} : Edge{b.x, b.y, a.x, a.y, -1});
    }
  }
  if (edges.empty() || xmax < 0 || xmin > float(img.w)) return;
  const int r0 = std::max(0, int(std::floor(ymin))), r1 = std::min(img.h - 1, int(std::floor(ymax)));
  std::vector<float> cov(size_t(img.w));
  std::vector<std::pair<float, int>> xs;
  const uint32_t sr = rgb >> 16 & 0xFF, sg = rgb >> 8 & 0xFF, sb = rgb & 0xFF;
  for (int row = r0; row <= r1; ++row) {
    std::fill(cov.begin(), cov.end(), 0.f);
    bool any = false;
    for (int k = 0; k < kSub; ++k) {
      const float y = float(row) + (float(k) + 0.5f) / kSub;
      xs.clear();
      for (const Edge& e : edges)
        if (y >= e.y0 && y < e.y1) xs.emplace_back(e.x0 + (y - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir);
      if (xs.size() < 2) continue;
      std::sort(xs.begin(), xs.end());
      int wind = 0;
      for (size_t j = 0; j + 1 < xs.size(); ++j) {
        wind += xs[j].second;
        const bool inside = even_odd ? (j % 2 == 0) : (wind != 0);
        if (!inside) continue;
        const float xa = std::max(0.f, xs[j].first), xb = std::min(float(img.w), xs[j + 1].first);
        if (xb <= xa) continue;
        any = true;
        const int ia = int(xa), ib = std::min(img.w - 1, int(xb));
        if (ia == ib) cov[size_t(ia)] += (xb - xa) / kSub;
        else {
          cov[size_t(ia)] += (float(ia + 1) - xa) / kSub;
          for (int x = ia + 1; x < ib; ++x) cov[size_t(x)] += 1.f / kSub;
          cov[size_t(ib)] += (xb - float(ib)) / kSub;
        }
      }
    }
    if (!any) continue;
    for (int x = 0; x < img.w; ++x) {
      const float a = std::min(1.f, cov[size_t(x)]) * alpha;
      if (a <= 0) continue;
      uint32_t& d = img.at(x, row);
      const float ia = 1.f - a;
      const uint32_t r = uint32_t(float(d >> 16 & 0xFF) * ia + float(sr) * a + 0.5f);
      const uint32_t g = uint32_t(float(d >> 8 & 0xFF) * ia + float(sg) * a + 0.5f);
      const uint32_t b = uint32_t(float(d & 0xFF) * ia + float(sb) * a + 0.5f);
      d = 0xFF000000u | r << 16 | g << 8 | b;
    }
  }
}

float area(const Poly& p) {
  float a = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    const Pt u = p[i], v = p[(i + 1) % p.size()];
    a += u.x * v.y - v.x * u.y;
  }
  return a;
}

void add_oriented(std::vector<Poly>& out, Poly p) {
  if (area(p) < 0) std::reverse(p.begin(), p.end());
  out.push_back(std::move(p));
}

void add_circle(std::vector<Poly>& out, Pt c, float r) {
  const int n = r < 1.5f ? 8 : (r < 4 ? 14 : 28);
  Poly p;
  for (int i = 0; i < n; ++i) {
    const float a = 6.2831853f * float(i) / float(n);
    p.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
  }
  add_oriented(out, std::move(p));
}

struct StrokeStyle {
  float width = 1;
  int cap = 0;   // 0 butt, 1 round, 2 square
  int join = 0;  // 0 miter, 1 round, 2 bevel
};

void stroke_polyline(std::vector<Poly>& out, const Poly& in, bool closed, const StrokeStyle& st) {
  Poly pts;
  for (const Pt& p : in)
    if (pts.empty() || std::hypot(p.x - pts.back().x, p.y - pts.back().y) > 1e-4f) pts.push_back(p);
  if (closed && pts.size() > 1 && std::hypot(pts.front().x - pts.back().x, pts.front().y - pts.back().y) <= 1e-4f) pts.pop_back();
  const float r = st.width / 2;
  if (pts.size() == 1) {  // a point: only round and square caps draw something
    if (st.cap == 1) add_circle(out, pts[0], r);
    else if (st.cap == 2) add_oriented(out, {{pts[0].x - r, pts[0].y - r}, {pts[0].x + r, pts[0].y - r}, {pts[0].x + r, pts[0].y + r}, {pts[0].x - r, pts[0].y + r}});
    return;
  }
  if (pts.size() < 2) return;
  const size_t n = pts.size(), segs = closed ? n : n - 1;
  for (size_t i = 0; i < segs; ++i) {
    Pt a = pts[i], b = pts[(i + 1) % n];
    const float len = std::hypot(b.x - a.x, b.y - a.y);
    const float ux = (b.x - a.x) / len, uy = (b.y - a.y) / len;
    if (!closed && st.cap == 2) {
      if (i == 0) a = {a.x - ux * r, a.y - uy * r};
      if (i + 1 == segs) b = {b.x + ux * r, b.y + uy * r};
    }
    const float nx = -uy * r, ny = ux * r;
    add_oriented(out, {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}});
  }
  if (st.cap == 1 && !closed) {
    add_circle(out, pts.front(), r);
    add_circle(out, pts.back(), r);
  }
  // joins
  const size_t j0 = closed ? 0 : 1, j1 = closed ? n : n - 1;
  for (size_t j = j0; j < j1; ++j) {
    const Pt p0 = pts[(j + n - 1) % n], p1 = pts[j], p2 = pts[(j + 1) % n];
    if (st.join == 1) { add_circle(out, p1, r); continue; }
    const float d1x = p1.x - p0.x, d1y = p1.y - p0.y, d2x = p2.x - p1.x, d2y = p2.y - p1.y;
    const float l1 = std::hypot(d1x, d1y), l2 = std::hypot(d2x, d2y);
    const float cross = d1x * d2y - d1y * d2x;
    if (std::fabs(cross) < 1e-6f * l1 * l2) continue;
    const float s = cross > 0 ? -1.f : 1.f;  // outer side of the turn
    const Pt a{p1.x + s * (-d1y / l1) * r, p1.y + s * (d1x / l1) * r}, b{p1.x + s * (-d2y / l2) * r, p1.y + s * (d2x / l2) * r};
    add_oriented(out, {p1, a, b});  // bevel wedge
    if (st.join == 0) {             // miter apex (limit 4, like SVG's default)
      const float dot = (d1x * d2x + d1y * d2y) / (l1 * l2);
      const float cos_half = std::sqrt(std::max(0.f, (1 + dot) / 2));  // cos of half the turn angle
      if (cos_half > 0.25f) {
        const float mx = (a.x + b.x) / 2 - p1.x, my = (a.y + b.y) / 2 - p1.y, ml = std::hypot(mx, my);
        if (ml > 1e-5f) {
          const float k = r / cos_half / ml;
          add_oriented(out, {a, {p1.x + mx * k, p1.y + my * k}, b, p1});
        }
      }
    }
  }
}

std::vector<Poly> dashed(const Poly& in, bool closed, const std::vector<float>& dash, float scale) {
  std::vector<Poly> out;
  Poly pts = in;
  if (closed && !pts.empty()) pts.push_back(pts.front());
  float total = 0;
  for (float d : dash) total += d * scale;
  if (total <= 1e-4f || pts.size() < 2) { out.push_back(in); return out; }
  size_t di = 0;
  float left = std::max(1e-3f, dash[0] * scale);
  bool on = true;
  Poly cur;
  cur.push_back(pts[0]);
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    Pt a = pts[i];
    const Pt b = pts[i + 1];
    float seg = std::hypot(b.x - a.x, b.y - a.y);
    while (seg > left) {
      const float t = left / seg;
      const Pt m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
      if (on) { cur.push_back(m); out.push_back(cur); cur.clear(); }
      else { cur.clear(); cur.push_back(m); }
      on = !on;
      seg -= left;
      a = m;
      di = (di + 1) % dash.size();
      left = std::max(1e-3f, dash[di] * scale);
    }
    left -= seg;
    if (on) cur.push_back(b);
  }
  if (on && cur.size() > 1) out.push_back(cur);
  return out;
}

// ------------------------------------------------------------------ drawing

struct Style {
  bool fill_on = true, stroke_on = false;
  uint32_t fill = 0, stroke = 0;
  float sw = 1;
  int cap = 0, join = 0;
  float opacity = 1;
  std::vector<float> dash;
  bool tinted = false;  // a tinted ancestor recolours the whole subtree, whatever the children set
  uint32_t tint = 0;
};

void apply_style(Style& s, const Node& n) {
  auto one = [&](const std::string& k, const std::string& v) {
    if (k == "fill") s.fill_on = parse_color(v, s.fill);
    else if (k == "stroke") s.stroke_on = parse_color(v, s.stroke);
    else if (k == "stroke-width") s.sw = std::strtof(v.c_str(), nullptr);
    else if (k == "stroke-linecap") s.cap = v == "round" ? 1 : (v == "square" ? 2 : 0);
    else if (k == "stroke-linejoin") s.join = v == "round" ? 1 : (v == "bevel" ? 2 : 0);
    else if (k == "opacity") s.opacity *= std::clamp(std::strtof(v.c_str(), nullptr), 0.f, 1.f);
    else if (k == "stroke-dasharray") s.dash = v == "none" ? std::vector<float>{} : floats(v);
  };
  for (const auto& kv : n.attr) one(kv.first, kv.second);
  for (const auto& kv : n.over) one(kv.first, kv.second);
  if (n.tinted) {
    s.tinted = true;
    s.tint = n.tint;
  }
  if (s.tinted) {
    if (s.fill_on) s.fill = s.tint;
    if (s.stroke_on) s.stroke = s.tint;
  }
  s.opacity *= std::clamp(n.alpha, 0.f, 1.f);
}

// The geometry of one shape element (no children), in its own coordinates. sc = device scale, for
// curve flattening. Returns false for non-shape tags. `line` clears fill.
bool shape_geometry(const Node& n, float sc, std::vector<SubPath>& sub, bool& no_fill) {
  const std::string& t = n.tag;
  no_fill = false;
  if (t == "path") {
    if (const std::string* d = n.get("d")) sub = parse_path(*d, sc);
  } else if (t == "line") {
    sub.push_back({{{num(n.get("x1"), 0), num(n.get("y1"), 0)}, {num(n.get("x2"), 0), num(n.get("y2"), 0)}}, false});
    no_fill = true;
  } else if (t == "polyline" || t == "polygon") {
    const std::string* p = n.get("points");
    if (!p) return true;
    const std::vector<float> v = floats(*p);
    SubPath s;
    for (size_t i = 0; i + 1 < v.size(); i += 2) s.pts.push_back({v[i], v[i + 1]});
    s.closed = t == "polygon";
    sub.push_back(std::move(s));
  } else if (t == "rect") {
    const float x = num(n.get("x"), 0), y = num(n.get("y"), 0), w = num(n.get("width"), 0), h = num(n.get("height"), 0);
    sub.push_back({{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, true});
  } else if (t == "circle" || t == "ellipse") {
    const float cx = num(n.get("cx"), 0), cy = num(n.get("cy"), 0);
    const float rx = t == "circle" ? num(n.get("r"), 0) : num(n.get("rx"), 0), ry = t == "circle" ? rx : num(n.get("ry"), 0);
    const int segs = std::clamp(int(std::max(rx, ry) * sc * 2) + 12, 12, 96);
    SubPath s;
    for (int i = 0; i < segs; ++i) {
      const float a = 6.2831853f * float(i) / float(segs);
      s.pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
    s.closed = true;
    sub.push_back(std::move(s));
  } else {
    return false;
  }
  return true;
}

bool skipped_tag(const std::string& t) {
  return t == "defs" || t == "clipPath" || t == "title" || t == "desc" || t == "metadata" || t == "use";
}

// The node's own transform chain: authored transform, then the run-time translation and extra transform
// (both in the node's inner coordinates, so they apply to the geometry first).
Mat local_matrix(const Node& n, bool overrides) {
  Mat m;
  if (overrides) {
    m = mat(n.xf);
    if (n.dx != 0 || n.dy != 0) m = m.then(Mat{1, 0, 0, 1, n.dx, n.dy});
  }
  if (const std::string* tr = n.get("transform")) m = m.then(parse_transform(*tr));
  return m;
}

struct Painter {
  Image& img;

  // sw_scale: device pixels per user unit for the stroke width (differs from m for substituted glyphs)
  void paint(const std::vector<SubPath>& sub, const Style& st, const Mat& m, float sw_scale) {
    if (st.opacity <= 0) return;
    const float sc = m.scale();
    std::vector<Poly> device;
    for (const SubPath& sp : sub) {
      Poly dp;
      for (const Pt& p : sp.pts) dp.push_back(m.apply(p));
      device.push_back(std::move(dp));
    }
    if (st.fill_on) fill_polys(img, device, false, st.fill, st.opacity);
    if (!st.stroke_on || st.sw <= 0) return;
    StrokeStyle ss;
    ss.width = std::max(0.35f, st.sw * sw_scale);
    ss.cap = st.cap;
    ss.join = st.join;
    std::vector<Poly> pieces;
    for (size_t i = 0; i < sub.size(); ++i) {
      if (st.dash.empty()) stroke_polyline(pieces, device[i], sub[i].closed, ss);
      else
        for (const Poly& d : dashed(device[i], sub[i].closed, st.dash, sc)) stroke_polyline(pieces, d, false, ss);
    }
    fill_polys(img, pieces, false, st.stroke, st.opacity);
  }

  // A substituted glyph: geometry only, in the slot's style.
  void glyph(const Node& g, const Style& st, const Mat& m, float sw_scale) {
    if (skipped_tag(g.tag)) return;
    const Mat gm = local_matrix(g, false).then(m);
    if (g.tag == "g") {
      for (const Node& k : g.kids) glyph(k, st, gm, sw_scale);
      return;
    }
    std::vector<SubPath> sub;
    bool no_fill = false;
    if (!shape_geometry(g, gm.scale(), sub, no_fill) || sub.empty()) return;
    Style s2 = st;
    s2.fill_on = false;  // the font is drawn with strokes only
    paint(sub, s2, gm, sw_scale);
  }

  void node(const Node& n, Style st, Mat m) {
    const std::string* disp = n.get("display");
    if (n.visible == 0 || (n.visible < 0 && disp && *disp == "none")) return;
    if (skipped_tag(n.tag)) return;
    apply_style(st, n);
    m = local_matrix(n, true).then(m);
    if (n.glyph) {
      glyph(*n.glyph, st, mat(n.glyph_xf).then(m), m.scale());
      return;
    }
    if (n.tag == "g" || n.tag == "svg") {
      for (const Node& k : n.kids) node(k, st, m);
      return;
    }
    std::vector<SubPath> sub;
    bool no_fill = false;
    if (!shape_geometry(n, m.scale(), sub, no_fill) || sub.empty()) return;
    if (no_fill) st.fill_on = false;
    paint(sub, st, m, m.scale());
  }
};

void bounds_of(const Node& n, const Mat& m, Box& b) {
  if (skipped_tag(n.tag)) return;
  if (n.tag == "g" || n.tag == "svg") {
    for (const Node& k : n.kids) bounds_of(k, local_matrix(k, false).then(m), b);
    return;
  }
  std::vector<SubPath> sub;
  bool no_fill = false;
  if (!shape_geometry(n, 4.f, sub, no_fill)) return;
  for (const SubPath& sp : sub)
    for (const Pt& p : sp.pts) {
      const Pt q = m.apply(p);
      b.add(q.x, q.y);
    }
}

size_t count_shapes(const Node& n) {
  static const char* shapes[] = {"path", "line", "polyline", "polygon", "rect", "circle", "ellipse"};
  if (n.tag == "defs" || n.tag == "clipPath") return 0;
  size_t c = 0;
  for (const char* s : shapes)
    if (n.tag == s) c = 1;
  for (const Node& k : n.kids) c += count_shapes(k);
  return c;
}

void index_ids(Node& n, std::map<std::string, Node*>& ids, std::multimap<std::string, Node*>& clean) {
  if (!n.id.empty()) {
    ids.emplace(n.id, &n);
    clean.emplace(clean_id(n.id), &n);
  }
  for (Node& k : n.kids) {
    k.parent = &n;
    index_ids(k, ids, clean);
  }
}

void reset(Node& n) {
  n.visible = -1;
  n.dx = n.dy = 0;
  n.tinted = false;
  n.alpha = 1;
  n.xf = Affine{};
  n.over.clear();
  n.glyph = nullptr;
  for (Node& k : n.kids) reset(k);
}

}  // namespace

const std::string* Node::get(const char* name) const {
  for (const auto& kv : over)
    if (kv.first == name) return &kv.second;
  for (const auto& kv : attr)
    if (kv.first == name) return &kv.second;
  return nullptr;
}

bool Document::load(const std::string& text, std::string& err) {
  root_ = Node();
  ids_.clear();
  shapes_ = 0;
  XmlParser xp(text);
  Node holder;
  if (!xp.parse_children(holder, 0)) { err = xp.err.empty() ? "svg: malformed" : xp.err; return false; }
  for (Node& k : holder.kids)
    if (k.tag == "svg") { root_ = std::move(k); break; }
  if (root_.tag != "svg") { err = "svg: no <svg> element"; return false; }
  clean_.clear();
  index_ids(root_, ids_, clean_);
  shapes_ = count_shapes(root_);
  return true;
}

Node* Document::find(const std::string& id) {
  const auto it = ids_.find(id);
  if (it != ids_.end()) return it->second;
  const auto c = clean_.find(id);
  return c == clean_.end() ? nullptr : c->second;
}

std::vector<Node*> Document::find_all(const std::string& name) {
  std::vector<Node*> out;
  const auto r = clean_.equal_range(name);
  for (auto it = r.first; it != r.second; ++it) out.push_back(it->second);
  return out;  // multimap keeps insertion (= document) order for equal keys
}

// The authored transforms of n's ancestors (not n itself), outermost last.
static Mat ancestors(const Node* n) {
  Mat m;
  for (const Node* p = n ? n->parent : nullptr; p; p = p->parent) m = m.then(local_matrix(*p, false));
  return m;
}

Box Document::bounds(const Node* n, bool inner) const {
  Box b;
  if (!n) return b;
  Mat m;
  if (!inner) m = local_matrix(*n, false).then(ancestors(n));
  if (n->tag == "g" || n->tag == "svg") {
    for (const Node& k : n->kids) bounds_of(k, local_matrix(k, false).then(m), b);
  } else {
    bounds_of(*n, m, b);
  }
  return b;
}

void Document::to_doc(const Node* n, float& x, float& y) const {
  if (!n) return;
  const Pt p = local_matrix(*n, false).then(ancestors(n)).apply({x, y});
  x = p.x;
  y = p.y;
}

float Document::px_scale(int w, int h) const {
  float vw = 0, vh = 0;
  if (const std::string* vb = root_.get("viewBox")) {
    const std::vector<float> v = floats(*vb);
    if (v.size() == 4) vw = v[2], vh = v[3];
  }
  if (vw <= 0 || vh <= 0) return 1;
  return std::min(float(w) / vw, float(h) / vh);
}

void Document::reset_overrides() { reset(root_); }

void Document::draw(Image& dst) const {
  std::fill(dst.px.begin(), dst.px.end(), 0xFF000000u);
  float vx = 0, vy = 0, vw = 0, vh = 0;
  if (const std::string* vb = root_.get("viewBox")) {
    const std::vector<float> v = floats(*vb);
    if (v.size() == 4) vx = v[0], vy = v[1], vw = v[2], vh = v[3];
  }
  if (vw <= 0 || vh <= 0) vw = float(dst.w), vh = float(dst.h);
  const float s = std::min(float(dst.w) / vw, float(dst.h) / vh);
  Painter p{dst};
  p.node(root_, Style{}, Mat{s, 0, 0, s, -vx * s, -vy * s});
}

Affine Affine::rotate(float deg, float cx, float cy) {
  const float r = deg * 3.14159265f / 180.f, cs = std::cos(r), sn = std::sin(r);
  return {cs, sn, -sn, cs, cx - cs * cx + sn * cy, cy - sn * cx - cs * cy};
}

Affine Affine::then(const Affine& m) const {
  const Mat r = mat(*this).then(mat(m));
  return {r.a, r.b, r.c, r.d, r.e, r.f};
}

void Node::set(const char* name, std::string value) {
  for (auto& kv : over)
    if (kv.first == name) { kv.second = std::move(value); return; }
  over.emplace_back(name, std::move(value));
}

std::string clean_id(const std::string& id) {
  std::string s;
  for (size_t i = 0; i < id.size(); ++i) {
    // "_xHH_": an escaped character (Illustrator writes "_" as "_x5F_")
    if (i + 4 < id.size() && id[i] == '_' && id[i + 1] == 'x' && id[i + 4] == '_' &&
        std::isxdigit(static_cast<unsigned char>(id[i + 2])) && std::isxdigit(static_cast<unsigned char>(id[i + 3]))) {
      s += char(std::strtol(id.substr(i + 2, 2).c_str(), nullptr, 16));
      i += 4;
      continue;
    }
    s += id[i];
  }
  // trailing copy counter "_12_"
  if (s.size() >= 3 && s.back() == '_') {
    size_t j = s.size() - 1;
    while (j > 0 && std::isdigit(static_cast<unsigned char>(s[j - 1]))) --j;
    if (j < s.size() - 1 && j >= 2 && s[j - 1] == '_') s.erase(j - 1);
  }
  return s;
}

// ------------------------------------------------------------------ font

bool Font::load(Document& doc) {
  glyphs_.clear();
  owned_.clear();
  doc_ = &doc;
  const auto is_metric = [](const Node& n) {
    const std::string* s = n.get("stroke");
    return s && (*s == "#FFFF00" || *s == "#ffff00");
  };
  for (const Node& k : doc.root().kids) {
    if (k.id.empty()) continue;
    const std::string name = clean_id(k.id);
    if (name.size() != 1) continue;  // single characters only ("tape1", "_x34_by4" are pictograms)
    G g{&k, Box{}, 0, 0};
    // A yellow box in a glyph is its advance metric, not ink: measure and draw the glyph without it.
    const Node* metric = nullptr;
    for (const Node& c : k.kids)
      if (is_metric(c)) metric = &c;
    if (metric) {
      for (const Node& c : k.kids)
        if (&c != metric) g.b.add(doc.bounds(&c));
      const Box mb = doc.bounds(metric);
      g.metric = true;
      g.mx0 = mb.x0;
      g.mw = mb.w();
      owned_.push_back(k);
      Node& copy = owned_.back();
      for (auto it = copy.kids.begin(); it != copy.kids.end();)
        it = is_metric(*it) ? copy.kids.erase(it) : it + 1;
      g.n = &copy;
    } else {
      g.b = doc.bounds(&k);
    }
    if (g.b.empty()) continue;
    glyphs_[name[0]] = g;
  }
  const auto zero = glyphs_.find('0');
  if (zero != glyphs_.end()) {
    cap_ = zero->second.b.h();
    origin_base_ = zero->second.b.y1;
  }
  for (auto& kv : glyphs_) {
    G& g = kv.second;
    g.cell_x = origin_x_ + pitch_ * std::floor((g.b.x0 - origin_x_) / pitch_ + 0.02f);
    g.base = origin_base_ + row_ * std::round((g.b.y1 - origin_base_) / row_);
  }
  return ok();
}

bool Font::has(char c) const { return glyphs_.count(c) != 0; }

bool Font::set(Document& screen, Node* slot, char c, float cell_cx) const {
  if (!slot) return false;
  const auto it = glyphs_.find(c);
  if (it == glyphs_.end()) {
    if (c == ' ') { slot->visible = 0; return true; }
    return false;
  }
  const Box b = screen.bounds(slot, true);
  if (b.empty() || b.h() <= 0) return false;
  const G& g = it->second;
  const float s = b.h() / cap_;
  // centred on the authored glyph (a '1' is a single line, a '4' is wider than its cell), on its baseline
  // the slot's cell centre: a narrow authored glyph (a '1') stands at its cell's left edge in this font
  // the slot's cell centre: given by the caller (from the digit pitch of a number's siblings), else the
  // authored glyph's centre; a lone narrow glyph (a '1') is taken to stand at its cell's left edge
  const float half_cell = pitch_ * 0.25f * s;
  const float cx = cell_cx < 1e29f ? cell_cx : (b.w() < half_cell * 0.6f ? b.x0 + half_cell : b.cx());
  slot->glyph = g.n;
  slot->glyph_xf = Affine{s, 0, 0, s, cx - g.b.cx() * s, b.y1 - g.base * s};
  if (slot->visible == 0) slot->visible = -1;
  return true;
}

float Font::draw(Image& dst, float x, float y, float h, const std::string& str, uint32_t rgb, float width) const {
  const float s = h / cap_, adv = pitch_ * 0.5f * s + width;
  float cx = x;
  for (char ch : str) {
    const auto it = glyphs_.find(ch);
    if (it != glyphs_.end()) {
      const G& g = it->second;
      Painter p{dst};
      Style st;
      st.fill_on = false;
      st.stroke_on = true;
      st.stroke = rgb;
      st.sw = width;
      st.cap = 2;
      if (g.metric) {  // placed by its advance box; the same trailing gap as the other glyphs (digits have
                       // no left bearing, so a narrower gap lets the next digit cover a '.')
        p.glyph(*g.n, st, Mat{s, 0, 0, s, cx - g.mx0 * s, y + h - g.base * s}, 1.f);
        cx += std::max(g.mw, g.b.x1 - g.mx0) * s + width * 2.5f;
        continue;
      }
      p.glyph(*g.n, st, Mat{s, 0, 0, s, cx - g.cell_x * s, y + h - g.base * s}, 1.f);
      cx += std::max(adv, (g.b.x1 - g.cell_x) * s + width * 2.5f);
    } else {
      cx += adv;
    }
  }
  return cx - x;
}

// ------------------------------------------------------------------ procedural drawing

void stroke(Image& dst, const Points& pts, float width, uint32_t rgb, bool closed, float opacity, bool round) {
  Poly p;
  for (const auto& q : pts) p.push_back({q.first, q.second});
  StrokeStyle ss;
  ss.width = width;
  ss.cap = round ? 1 : 0;
  ss.join = round ? 1 : 0;
  std::vector<Poly> pieces;
  stroke_polyline(pieces, p, closed, ss);
  fill_polys(dst, pieces, false, rgb, opacity);
}

void fill(Image& dst, const Points& pts, uint32_t rgb, float opacity) {
  Poly p;
  for (const auto& q : pts) p.push_back({q.first, q.second});
  fill_polys(dst, {p}, false, rgb, opacity);
}

void circle(Image& dst, float cx, float cy, float r, uint32_t rgb, bool filled, float width, float opacity) {
  Points pts;
  const int n = std::clamp(int(r * 2) + 12, 12, 96);
  for (int i = 0; i < n; ++i) {
    const float a = 6.2831853f * float(i) / float(n);
    pts.emplace_back(cx + r * std::cos(a), cy + r * std::sin(a));
  }
  if (filled) fill(dst, pts, rgb, opacity);
  else stroke(dst, pts, width, rgb, true, opacity);
}

}  // namespace svg
}  // namespace op1
