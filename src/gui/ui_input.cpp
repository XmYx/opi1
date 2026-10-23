// Keyboard mapping, scripted input and the headless runner (owner: t6). See ui_input.h.
#include "ui_input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "op1/core.h"
#include "op1/ui.h"

namespace op1 {

namespace {

using K = KeyKind;
constexpr int8_t kk(OpKey k) { return int8_t(k); }
constexpr int8_t mm(Mode m) { return int8_t(m); }

// The PC keyboard (ANSI geometry, quarter key widths) and what each key is on the OG panel. Device rows:
// top = the encoders (F-row), then synth/drum/tape/mixer + T1-T4 (number and Q rows, left), lift/drop/split
// (A row), rec/play/stop (Z row), left/right/shift (arrows, Shift). The 24 keys run F..E: the upper octave
// on the Q row (whites T..[) with its black keys on the number row, the lower octave on the Z row (whites
// V../) with its black keys on the A row, each black key on the boundary of its two white keys.
const std::vector<PanelKey> kPanel = {
    // F-row: the four encoders (push, then -/+ per encoder)
    {0, 0, 4, SDL_SCANCODE_ESCAPE, K::Quit, 0, 0, "X"},
    {0, 8, 4, SDL_SCANCODE_F1, K::Push, 0, 0, "P"}, {0, 12, 4, SDL_SCANCODE_F2, K::Push, 1, 0, "P"},
    {0, 16, 4, SDL_SCANCODE_F3, K::Push, 2, 0, "P"}, {0, 20, 4, SDL_SCANCODE_F4, K::Push, 3, 0, "P"},
    {0, 26, 4, SDL_SCANCODE_F5, K::Turn, 0, -1, "-"}, {0, 30, 4, SDL_SCANCODE_F6, K::Turn, 0, 1, "+"},
    {0, 34, 4, SDL_SCANCODE_F7, K::Turn, 1, -1, "-"}, {0, 38, 4, SDL_SCANCODE_F8, K::Turn, 1, 1, "+"},
    {0, 44, 4, SDL_SCANCODE_F9, K::Turn, 2, -1, "-"}, {0, 48, 4, SDL_SCANCODE_F10, K::Turn, 2, 1, "+"},
    {0, 52, 4, SDL_SCANCODE_F11, K::Turn, 3, -1, "-"}, {0, 56, 4, SDL_SCANCODE_F12, K::Turn, 3, 1, "+"},
    // number row: help, the mode keys, tempo; upper black keys; input, com
    {1, 0, 4, SDL_SCANCODE_GRAVE, K::Key, kk(OpKey::Help), 0, "?"},
    {1, 4, 4, SDL_SCANCODE_1, K::Mode, mm(Mode::Synth), 0, "SY"}, {1, 8, 4, SDL_SCANCODE_2, K::Mode, mm(Mode::Drum), 0, "DR"},
    {1, 12, 4, SDL_SCANCODE_3, K::Mode, mm(Mode::Tape), 0, "TP"}, {1, 16, 4, SDL_SCANCODE_4, K::Mode, mm(Mode::Mixer), 0, "MX"},
    {1, 20, 4, SDL_SCANCODE_5, K::Mode, mm(Mode::Tempo), 0, "TM"},
    {1, 24, 4, SDL_SCANCODE_6, K::Note, 13, 0, ""}, {1, 28, 4, SDL_SCANCODE_7, K::Note, 15, 0, ""},
    {1, 32, 4, SDL_SCANCODE_8, K::Note, 17, 0, ""}, {1, 36, 4, SDL_SCANCODE_9, K::Gap, 0, 0, ""},
    {1, 40, 4, SDL_SCANCODE_0, K::Note, 20, 0, ""}, {1, 44, 4, SDL_SCANCODE_MINUS, K::Note, 22, 0, ""},
    {1, 48, 4, SDL_SCANCODE_EQUALS, K::Key, kk(OpKey::Input), 0, "IN"},
    {1, 52, 8, SDL_SCANCODE_BACKSPACE, K::Mode, mm(Mode::Communication), 0, "COM"},
    // Q row: sequencer, T1-T4, upper white keys F4..E5, sequencer
    {2, 0, 6, SDL_SCANCODE_TAB, K::Key, kk(OpKey::Seq), 0, "SQ"},
    {2, 6, 4, SDL_SCANCODE_Q, K::Key, kk(OpKey::T1), 0, "T1"}, {2, 10, 4, SDL_SCANCODE_W, K::Key, kk(OpKey::T2), 0, "T2"},
    {2, 14, 4, SDL_SCANCODE_E, K::Key, kk(OpKey::T3), 0, "T3"}, {2, 18, 4, SDL_SCANCODE_R, K::Key, kk(OpKey::T4), 0, "T4"},
    {2, 22, 4, SDL_SCANCODE_T, K::Note, 12, 0, ""}, {2, 26, 4, SDL_SCANCODE_Y, K::Note, 14, 0, ""},
    {2, 30, 4, SDL_SCANCODE_U, K::Note, 16, 0, ""}, {2, 34, 4, SDL_SCANCODE_I, K::Note, 18, 0, ""},
    {2, 38, 4, SDL_SCANCODE_O, K::Note, 19, 0, ""}, {2, 42, 4, SDL_SCANCODE_P, K::Note, 21, 0, ""},
    {2, 46, 4, SDL_SCANCODE_LEFTBRACKET, K::Note, 23, 0, ""}, {2, 50, 4, SDL_SCANCODE_RIGHTBRACKET, K::None, 0, 0, ""},
    {2, 54, 6, SDL_SCANCODE_BACKSLASH, K::Key, kk(OpKey::Seq), 0, "SQ"},
    // A row: tape edits; lower black keys; record
    {3, 0, 7, SDL_SCANCODE_CAPSLOCK, K::None, 0, 0, ""},
    {3, 7, 4, SDL_SCANCODE_A, K::Key, kk(OpKey::Lift), 0, "LF"}, {3, 11, 4, SDL_SCANCODE_S, K::Key, kk(OpKey::Drop), 0, "DP"},
    {3, 15, 4, SDL_SCANCODE_D, K::Key, kk(OpKey::Split), 0, "SP"}, {3, 19, 4, SDL_SCANCODE_F, K::Gap, 0, 0, ""},
    {3, 23, 4, SDL_SCANCODE_G, K::Note, 1, 0, ""}, {3, 27, 4, SDL_SCANCODE_H, K::Note, 3, 0, ""},
    {3, 31, 4, SDL_SCANCODE_J, K::Note, 5, 0, ""}, {3, 35, 4, SDL_SCANCODE_K, K::Gap, 0, 0, ""},
    {3, 39, 4, SDL_SCANCODE_L, K::Note, 8, 0, ""}, {3, 43, 4, SDL_SCANCODE_SEMICOLON, K::Note, 10, 0, ""},
    {3, 47, 4, SDL_SCANCODE_APOSTROPHE, K::None, 0, 0, ""},
    {3, 51, 9, SDL_SCANCODE_RETURN, K::Key, kk(OpKey::Record), 0, "REC"},
    // Z row: shift, rec/play/stop, lower white keys F3..E4, shift
    {4, 0, 9, SDL_SCANCODE_LSHIFT, K::Key, kk(OpKey::Shift), 0, "SH"},
    {4, 9, 4, SDL_SCANCODE_Z, K::Key, kk(OpKey::Record), 0, "RC"}, {4, 13, 4, SDL_SCANCODE_X, K::Key, kk(OpKey::Play), 0, "PL"},
    {4, 17, 4, SDL_SCANCODE_C, K::Key, kk(OpKey::Stop), 0, "ST"},
    {4, 21, 4, SDL_SCANCODE_V, K::Note, 0, 0, ""}, {4, 25, 4, SDL_SCANCODE_B, K::Note, 2, 0, ""},
    {4, 29, 4, SDL_SCANCODE_N, K::Note, 4, 0, ""}, {4, 33, 4, SDL_SCANCODE_M, K::Note, 6, 0, ""},
    {4, 37, 4, SDL_SCANCODE_COMMA, K::Note, 7, 0, ""}, {4, 41, 4, SDL_SCANCODE_PERIOD, K::Note, 9, 0, ""},
    {4, 45, 4, SDL_SCANCODE_SLASH, K::Note, 11, 0, ""},
    {4, 49, 11, SDL_SCANCODE_RSHIFT, K::Key, kk(OpKey::Shift), 0, "SH"},
    // bottom row
    {5, 0, 6, SDL_SCANCODE_LCTRL, K::Layer, 0, 0, "CT"},
    {5, 16, 24, SDL_SCANCODE_SPACE, K::Key, kk(OpKey::Play), 0, "PLAY"},
    {5, 54, 6, SDL_SCANCODE_RCTRL, K::Layer, 0, 0, "CT"},
    // to the right: volume knob, power switch, the arrow keys (left/right, lift/drop)
    {1, 63, 4, SDL_SCANCODE_PAGEUP, K::Volume, 0, 1, "V+"}, {2, 63, 4, SDL_SCANCODE_PAGEDOWN, K::Volume, 0, -1, "V-"},
    {1, 68, 4, SDL_SCANCODE_END, K::Power, 0, 0, "PW"},
    {4, 67, 4, SDL_SCANCODE_UP, K::Key, kk(OpKey::Lift), 0, "LF"},
    {5, 63, 4, SDL_SCANCODE_LEFT, K::Key, kk(OpKey::Left), 0, "<"},
    {5, 67, 4, SDL_SCANCODE_DOWN, K::Key, kk(OpKey::Drop), 0, "DP"},
    {5, 71, 4, SDL_SCANCODE_RIGHT, K::Key, kk(OpKey::Right), 0, ">"},
};

const PanelKey* find_key(SDL_Scancode sc) {
  for (const PanelKey& k : kPanel)
    if (k.scancode == sc) return &k;
  return nullptr;
}

// Sound keys 1-8 (a Ctrl layer on the digits, and the keypad digits): the main block has no room left for
// eight more keys next to the encoders without giving up a playable two-octave keyboard (KEYMAP.md).
int sound_key(SDL_Scancode sc, bool ctrl) {
  if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_8) return int(sc - SDL_SCANCODE_KP_1);
  if (ctrl && sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_8) return int(sc - SDL_SCANCODE_1);
  return -1;
}

}  // namespace

const std::vector<PanelKey>& panel_keys() { return kPanel; }

void turn_last(UiModel& ui, int det) { ui.encoder(ui.state().last_enc, det); }

KeyAction map_key(UiModel& ui, uint32_t scancode, uint16_t mod, bool down, bool repeat) {
  KeyAction a;
  a.handled = true;
  const SDL_Scancode sc = SDL_Scancode(scancode);
  const bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
  if (const int s = sound_key(sc, ctrl); s >= 0) {
    if (!repeat) ui.key(OpKey(int(OpKey::S1) + s), down);
    return a;
  }
  const PanelKey* k = find_key(sc);
  // the Ctrl layer: window and the port's extra screens. Only presses: a release always reaches its key, so a
  // note or panel key held while Ctrl goes down still lets go (t6 review: before, the note stayed on)
  if (ctrl && down && (!k || k->kind != K::Turn)) {
    if (repeat) return a;
    switch (sc) {
      case SDL_SCANCODE_K: a.legend = true; return a;
      case SDL_SCANCODE_F: a.fullscreen = true; return a;
      case SDL_SCANCODE_Q: a.quit = true; return a;
      case SDL_SCANCODE_LEFTBRACKET: ui.browse(-1); return a;
      case SDL_SCANCODE_RIGHTBRACKET: ui.browse(1); return a;
      case SDL_SCANCODE_BACKSLASH: ui.browse(0); return a;
      case SDL_SCANCODE_COMMA: ui.engine_step(-1); return a;
      case SDL_SCANCODE_PERIOD: ui.engine_step(1); return a;
      case SDL_SCANCODE_A: ui.set_mode(Mode::Album); return a;
      case SDL_SCANCODE_B: ui.set_mode(Mode::Browser); return a;
      case SDL_SCANCODE_C: ui.set_mode(Mode::Clock); return a;
      case SDL_SCANCODE_T: ui.set_mode(Mode::Tune); return a;
      case SDL_SCANCODE_G: ui.set_mode(Mode::SignalFlow); return a;
      case SDL_SCANCODE_L: ui.set_mode(Mode::Lander); return a;
      case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return a;
      default: break;
    }
    if (!k) { a.handled = false; return a; }
  }
  if (!k) { a.handled = false; return a; }
  switch (k->kind) {
    case K::Note:
      if (!repeat) ui.note(k->arg, down);
      return a;
    case K::Turn:  // auto-repeat allowed: holding the key keeps turning
      if (down) ui.encoder(k->arg, k->dir * (ctrl ? 8 : 1));
      return a;
    case K::Key:
      if (OpKey(k->arg) == OpKey::Shift) { ui.key(OpKey::Shift, down); return a; }
      if (!repeat) ui.key(OpKey(k->arg), down);
      return a;
    case K::Volume:  // a knob: repeat keeps turning
      if (down) ui.volume(k->dir);
      return a;
    default: break;
  }
  if (!down || repeat) return a;
  switch (k->kind) {
    case K::Push: ui.push(k->arg); return a;
    case K::Mode: ui.set_mode(Mode(k->arg)); return a;
    case K::Power: ui.power(); return a;
    case K::Quit: a.quit = true; return a;
    default: a.handled = k->kind != K::None; return a;
  }
}

namespace {

int encoder_index(const std::string& e) {
  if (e == "blue") return 0;
  if (e == "green") return 1;
  if (e == "white") return 2;
  if (e == "orange") return 3;
  return std::atoi(e.c_str()) & 3;
}

SDL_Scancode scancode_from_name(const std::string& name) {
  // spelled-out names for keys a script cannot write (',' separates steps) or that are easier to read
  static const struct { const char* n; SDL_Scancode sc; } words[] = {
      {"comma", SDL_SCANCODE_COMMA}, {"period", SDL_SCANCODE_PERIOD}, {"slash", SDL_SCANCODE_SLASH},
      {"semicolon", SDL_SCANCODE_SEMICOLON}, {"apostrophe", SDL_SCANCODE_APOSTROPHE}, {"minus", SDL_SCANCODE_MINUS},
      {"equals", SDL_SCANCODE_EQUALS}, {"grave", SDL_SCANCODE_GRAVE}, {"backslash", SDL_SCANCODE_BACKSLASH},
      {"leftbracket", SDL_SCANCODE_LEFTBRACKET}, {"rightbracket", SDL_SCANCODE_RIGHTBRACKET}};
  for (const auto& w : words)
    if (SDL_strcasecmp(name.c_str(), w.n) == 0) return w.sc;
  SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
  if (sc == SDL_SCANCODE_UNKNOWN) {
    const SDL_Keycode kc = SDL_GetKeyFromName(name.c_str());
    if (kc != SDLK_UNKNOWN) sc = SDL_GetScancodeFromKey(kc, nullptr);
  }
  return sc;
}

// op=NAME: an OP-1 key by its panel name (independent of the PC map).
bool op_key(UiModel& ui, const std::string& name) {
  static const struct { const char* n; OpKey k; } keys[] = {
      {"t1", OpKey::T1}, {"t2", OpKey::T2}, {"t3", OpKey::T3}, {"t4", OpKey::T4}, {"seq", OpKey::Seq},
      {"help", OpKey::Help}, {"input", OpKey::Input}, {"mic", OpKey::Input}, {"rec", OpKey::Record},
      {"record", OpKey::Record}, {"play", OpKey::Play}, {"stop", OpKey::Stop}, {"left", OpKey::Left},
      {"right", OpKey::Right}, {"lift", OpKey::Lift}, {"drop", OpKey::Drop}, {"split", OpKey::Split},
      {"s1", OpKey::S1}, {"s2", OpKey::S2}, {"s3", OpKey::S3}, {"s4", OpKey::S4},
      {"s5", OpKey::S5}, {"s6", OpKey::S6}, {"s7", OpKey::S7}, {"s8", OpKey::S8}};
  static const struct { const char* n; Mode m; } modes[] = {
      {"synth", Mode::Synth}, {"drum", Mode::Drum}, {"tape", Mode::Tape}, {"mixer", Mode::Mixer},
      {"tempo", Mode::Tempo}, {"com", Mode::Communication}};
  for (const auto& k : keys)
    if (name == k.n) { ui.key(k.k, true); ui.key(k.k, false); return true; }
  for (const auto& m : modes)
    if (name == m.n) { ui.set_mode(m.m); return true; }
  if (name == "vol+") { ui.volume(1); return true; }
  if (name == "vol-") { ui.volume(-1); return true; }
  if (name == "power") { ui.power(); return true; }
  return false;
}

}  // namespace

// ------------------------------------------------------------------ script

bool Script::load(const std::string& spec, std::string& err) {
  steps_.clear();
  at_ = 0;
  wait_until_ = 0;
  std::string text = spec;
  if (!spec.empty() && spec[0] == '@') {
    std::ifstream f(spec.substr(1));
    if (!f) { err = "cannot read " + spec.substr(1); return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
  }
  std::string cur;
  auto flush = [&] {
    const size_t b = cur.find_first_not_of(" \t\r"), e = cur.find_last_not_of(" \t\r");
    if (b != std::string::npos && cur[b] != '#') steps_.push_back(cur.substr(b, e - b + 1));
    cur.clear();
  };
  for (char c : text) {
    if (c == ',' || c == '\n') flush();
    else cur += c;
  }
  flush();
  return true;
}

void Script::run(UiModel& ui, double t, std::vector<std::string>& shots, bool& quit) {
  while (at_ < steps_.size() && t >= wait_until_) {
    const std::string st = steps_[at_++];
    const size_t eq = st.find('=');
    const std::string op = st.substr(0, eq), arg = eq == std::string::npos ? "" : st.substr(eq + 1);
    if (op == "wait") {
      wait_until_ = t + std::atof(arg.c_str()) / 1000.0;
    } else if (op == "key" || op == "down" || op == "up" || op == "op") {
      std::string name = arg;
      uint16_t mod = 0;
      bool shift = false;
      for (;;) {
        if (name.rfind("shift+", 0) == 0) { shift = true; mod |= SDL_KMOD_LSHIFT; name = name.substr(6); }
        else if (name.rfind("ctrl+", 0) == 0) { mod |= SDL_KMOD_LCTRL; name = name.substr(5); }
        else break;
      }
      if (shift) ui.key(OpKey::Shift, true);
      if (op == "op") {
        if (!op_key(ui, name)) std::fprintf(stderr, "op1-og: script: unknown OP-1 key '%s'\n", name.c_str());
      } else if (const SDL_Scancode sc = scancode_from_name(name); sc == SDL_SCANCODE_UNKNOWN) {
        std::fprintf(stderr, "op1-og: script: unknown key '%s'\n", name.c_str());
      } else {
        if (op != "up") map_key(ui, sc, mod, true, false);
        if (op != "down") map_key(ui, sc, mod, false, false);
      }
      if (shift) ui.key(OpKey::Shift, false);
    } else if (op == "enc") {
      const size_t c = arg.find(':');
      ui.encoder(encoder_index(arg.substr(0, c)), c == std::string::npos ? 1 : std::atoi(arg.c_str() + c + 1));
    } else if (op == "press") {
      ui.push(encoder_index(arg));
    } else if (op == "shot") {
      shots.push_back(arg);
      wait_until_ = t + 0.05;  // the shot is taken after the next render
      return;
    } else if (op == "quit") {
      quit = true;
      return;
    } else {
      std::fprintf(stderr, "op1-og: script: unknown step '%s'\n", st.c_str());
    }
  }
}

// ------------------------------------------------------------------ images

namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  static uint32_t t[256];
  if (!t[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      t[i] = v;
    }
  for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  be32(out, uint32_t(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  be32(out, ~crc32(td.data(), td.size()));
}

}  // namespace

bool save_image(const std::string& path, const svg::Image& im, int scale) {
  const int w = im.w * scale, h = im.h * scale;
  auto px = [&](int x, int y) { return im.px[size_t(y / scale) * size_t(im.w) + size_t(x / scale)]; };
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool bmp = path.size() > 4 && path.compare(path.size() - 4, 4, ".bmp") == 0;
  if (bmp) {
    const uint32_t row = uint32_t(w) * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * uint32_t(h);
    uint8_t hd[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) hd[off + i] = uint8_t(v >> (8 * i)); };
    put32(2, size); put32(10, 54); put32(14, 40); put32(18, uint32_t(w)); put32(22, uint32_t(h));
    hd[26] = 1; hd[28] = 24; put32(34, size - 54);
    std::fwrite(hd, 1, 54, f);
    std::vector<uint8_t> line(row + pad, 0);
    for (int y = h - 1; y >= 0; --y) {
      for (int x = 0; x < w; ++x) {
        const uint32_t c = px(x, y);
        line[size_t(x) * 3] = uint8_t(c); line[size_t(x) * 3 + 1] = uint8_t(c >> 8); line[size_t(x) * 3 + 2] = uint8_t(c >> 16);
      }
      std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
    return true;
  }
  // PNG, RGB 8-bit, zlib stream of stored blocks
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      const uint32_t c = px(x, y);
      raw.push_back(uint8_t(c >> 16)); raw.push_back(uint8_t(c >> 8)); raw.push_back(uint8_t(c));
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t off = 0; off < raw.size();) {
    const size_t n = std::min<size_t>(65535, raw.size() - off);
    z.push_back(off + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + long(off), raw.begin() + long(off + n));
    off += n;
  }
  be32(z, (b << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  be32(ihdr, uint32_t(w)); be32(ihdr, uint32_t(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// ------------------------------------------------------------------ headless

int run_ui_headless(Core& core, AssetProvider& assets, const UiOptions& opt, Worker* worker) {
  std::string spec = opt.script;
  if (spec.empty())
    if (const char* sc = std::getenv("OP1_OG_SCRIPT")) spec = sc;
  const double duration_s = opt.duration_s;
  UiModel ui(core, assets);
  ui.set_worker(worker);
  Script script;
  std::string err;
  if (!spec.empty() && !script.load(spec, err)) {
    std::fprintf(stderr, "op1-og: script: %s\n", err.c_str());
    return 64;
  }
  svg::Image disp(kDisplayW, kDisplayH);
  const auto t0 = std::chrono::steady_clock::now();
  auto next = t0;
  std::vector<std::string> shots;
  bool quit = false;
  std::string last;
  for (;;) {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<std::string> due;
    script.run(ui, t, due, quit);
    const std::string& name = ui.render(disp, t);
    if (name != last) { std::fprintf(stderr, "op1-og: screen %s\n", name.c_str()); last = name; }
    for (const std::string& p : shots) {
      if (save_image(p, disp)) std::fprintf(stderr, "op1-og: shot %s (%s)\n", p.c_str(), name.c_str());
      else std::fprintf(stderr, "op1-og: cannot write %s\n", p.c_str());
    }
    shots = due;
    if (quit && shots.empty()) break;
    if (!spec.empty() && script.done() && shots.empty()) break;
    if (duration_s > 0 && t >= duration_s) break;
    next += std::chrono::microseconds(16667);
    std::this_thread::sleep_until(next);
  }
  ui.release_all();
  return 0;
}

}  // namespace op1
