#include "json_read.h"

#include <algorithm>
#include <cstdlib>

namespace op1::state {
namespace {

struct Parser {
  const std::string& s;
  size_t p = 0;
  std::string& err;

  void ws() {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
  }
  bool fail(const char* what) {
    err = std::string("json: ") + what + " at " + std::to_string(p);
    return false;
  }
  bool lit(const char* w) {
    size_t n = 0;
    while (w[n]) ++n;
    if (s.compare(p, n, w) != 0) return false;
    p += n;
    return true;
  }
  bool string(std::string& out) {
    if (p >= s.size() || s[p] != '"') return fail("expected string");
    ++p;
    out.clear();
    while (p < s.size() && s[p] != '"') {
      char c = s[p++];
      if (c == '\\' && p < s.size()) {
        const char e = s[p++];
        switch (e) {
          case 'n': c = '\n'; break;
          case 't': c = '\t'; break;
          case 'r': c = '\r'; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'u': c = '?'; p = std::min(s.size(), p + 4); break;  // names are ASCII; keep a placeholder
          default: c = e; break;
        }
      }
      out.push_back(c);
    }
    if (p >= s.size()) return fail("unterminated string");
    ++p;
    return true;
  }
  bool value(Json& v, int depth) {
    if (depth > 16) return fail("nested too deep");
    ws();
    if (p >= s.size()) return fail("unexpected end");
    const char c = s[p];
    if (c == '{') {
      v.kind = Json::Object;
      ++p;
      ws();
      if (p < s.size() && s[p] == '}') { ++p; return true; }
      for (;;) {
        ws();
        std::string k;
        if (!string(k)) return false;
        ws();
        if (p >= s.size() || s[p] != ':') return fail("expected ':'");
        ++p;
        Json child;
        if (!value(child, depth + 1)) return false;
        v.obj.emplace_back(std::move(k), std::move(child));
        ws();
        if (p < s.size() && s[p] == ',') { ++p; continue; }
        if (p < s.size() && s[p] == '}') { ++p; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      v.kind = Json::Array;
      ++p;
      ws();
      if (p < s.size() && s[p] == ']') { ++p; return true; }
      for (;;) {
        Json child;
        if (!value(child, depth + 1)) return false;
        v.arr.push_back(std::move(child));
        ws();
        if (p < s.size() && s[p] == ',') { ++p; continue; }
        if (p < s.size() && s[p] == ']') { ++p; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { v.kind = Json::String; return string(v.str); }
    if (lit("true")) { v.kind = Json::Bool; v.b = true; return true; }
    if (lit("false")) { v.kind = Json::Bool; return true; }
    if (lit("null")) return true;
    char* end = nullptr;
    v.num = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return fail("unexpected character");
    v.kind = Json::Number;
    p = size_t(end - s.c_str());
    return true;
  }
};

}  // namespace

bool json_parse(const std::string& text, Json& out, std::string& err) {
  if (text.size() > (size_t(1) << 20)) { err = "json: text too large"; return false; }
  out = Json{};
  Parser ps{text, 0, err};
  if (!ps.value(out, 0)) return false;
  ps.ws();
  if (ps.p != text.size()) return ps.fail("trailing characters");
  return true;
}

}  // namespace op1::state
