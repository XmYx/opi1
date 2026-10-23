// The OG display window and overlays (owner: t6).
//
// The window shows the 320x160 OP-1 display and nothing else by default; with --keyboard
// (UiOptions::keyboard_overlay) an on-screen keyboard and a key-mapping legend appear below it. What is on
// the display, and what keys and encoders do, is the UI model (ui_model.h); the key mapping is in
// ui_input.h. The overlay text uses a 5x7 bitmap font drawn for this project, not a TE font.
//
// Input latency (t7 finding, REPORT.md 4.6): the loop sleeps in SDL_WaitEventTimeout and handles every key
// the moment it arrives (notes go to the audio thread at once), and draws at most every 16.7 ms. Present
// does not wait for vsync, so a compositor that throttles an unfocused window cannot hold input back. At
// exit it prints how long input events waited before they were handled (event timestamp -> handling).
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bitmap_text.h"
#include "op1/core.h"
#include "op1/ui.h"
#include "svg.h"
#include "ui_input.h"
#include "ui_model.h"

namespace op1 {
namespace {

using bitmap::rect;
using bitmap::text;
using bitmap::text_w;
using svg::Image;

constexpr int kW = kDisplayW, kH = kDisplayH, kKbH = 52, kRowH = 11, kTextLines = 3,
              kLegendH = 6 * kRowH + 4 + kTextLines * 9 + 2;
constexpr uint32_t kBlue = 0xFF698EFF, kGreen = 0xFF00ED95, kWhite = 0xFFF0F0F0, kOrange = 0xFFFF6A1F, kDim = 0xFF606070,
                   kKeyDown = 0xFFFF6A1F;
constexpr uint32_t kEnc[4] = {kBlue, kGreen, kWhite, kOrange};

// The PC key that plays musical key idx (0..23 from the lowest F), as a one-character label.
std::string note_label(int idx) {
  for (const PanelKey& k : panel_keys())
    if (k.kind == KeyKind::Note && k.arg == idx) return SDL_GetScancodeName(SDL_Scancode(k.scancode));
  return "";
}

// The OP-1 keyboard: 24 keys F..E (14 white, 10 black), each with the PC key that plays it.
void draw_keyboard(Image& im, int y0, const bool held[128], int base) {
  static const int white_semis[7] = {0, 2, 4, 6, 7, 9, 11};  // F G A B C D E
  static const int black_after[5] = {0, 1, 2, 4, 5};
  static const int black_semis[5] = {1, 3, 5, 8, 10};       // F# G# A# C# D#
  const int kw = kW / 14, kh = kKbH - 6;
  rect(im, 0, y0, kW, kKbH, 0xFF101018u);
  for (int i = 0; i < 14; ++i) {
    const int idx = (i / 7) * 12 + white_semis[i % 7];
    rect(im, i * kw + 1, y0 + 3, kw - 2, kh, held[(base + idx) & 127] ? kKeyDown : 0xFFD8D8E0u);
    text(im, i * kw + kw / 2 - 2, y0 + 3 + kh - 10, note_label(idx), 0xFF202030u);
  }
  for (int o = 0; o < 2; ++o)
    for (int b = 0; b < 5; ++b) {
      const int idx = o * 12 + black_semis[b];
      const int x = (o * 7 + black_after[b] + 1) * kw - kw / 3;
      rect(im, x, y0 + 3, 2 * kw / 3, kh * 3 / 5, held[(base + idx) & 127] ? kKeyDown : 0xFF202030u);
      text(im, x + kw / 3 - 3, y0 + 5, note_label(idx), 0xFFB0B0C0u);
    }
}

// The legend: a picture of the PC keyboard, each key coloured like its OP-1 group (mode keys blue, T1-T4
// yellow, tape edits salmon, encoder keys in their encoder's colour, the musical keys white/black) and
// labelled with what it does, so the map reads like the device's panel.
void draw_legend(Image& im, int y0, const UiState& ui, const std::string& screen) {
  rect(im, 0, y0, kW, kLegendH, 0xFF08080Cu);
  const int base = 53 + 12 * ui.octave;
  auto px = [](int q) { return 2 + q * (kW - 4) / 75; };
  for (const PanelKey& k : panel_keys()) {
    uint32_t fill = 0xFF26262Eu, ink = 0xFFA0A0B8u;
    switch (k.kind) {
      case KeyKind::Note: {
        const int semi = k.arg % 12;
        const bool black = semi == 1 || semi == 3 || semi == 5 || semi == 8 || semi == 10;
        fill = ui.held[(base + k.arg) & 127] ? kKeyDown : black ? 0xFF3A3A48u : 0xFFD8D8E0u;
        break;
      }
      case KeyKind::Gap: fill = 0xFF16161Cu; break;
      case KeyKind::Mode: fill = k.arg == int8_t(Mode::Communication) || k.arg == int8_t(Mode::Tempo) ? 0xFFB8B8C4u : 0xFF4A86C8u;
        ink = 0xFF101018u; break;
      case KeyKind::Key: {
        const OpKey o = OpKey(k.arg);
        if (o >= OpKey::T1 && o <= OpKey::T4) fill = 0xFFE8C34Au;
        else if (o == OpKey::Lift || o == OpKey::Drop || o == OpKey::Split) fill = 0xFFE88A70u;
        else if (o == OpKey::Record) fill = 0xFFFF3A5Du;
        else fill = 0xFFB8B8C4u;
        if (o == OpKey::Shift && ui.shift) fill = kKeyDown;
        ink = 0xFF101018u;
        break;
      }
      case KeyKind::Turn: case KeyKind::Push:
        fill = kEnc[k.arg & 3];
        if (k.kind == KeyKind::Push && ui.t - ui.push_t[k.arg & 3] < 0.25) fill = kKeyDown;
        ink = 0xFF101018u;
        break;
      case KeyKind::Volume: case KeyKind::Power: fill = 0xFF8A8A98u; ink = 0xFF101018u; break;
      default: break;
    }
    const int x = px(k.x), w = px(k.x + k.w) - x - 1, y = y0 + 2 + k.row * kRowH;
    rect(im, x, y, w, kRowH - 1, fill);
    if (*k.label) text(im, x + (w - text_w(k.label)) / 2 + 1, y + 2, k.label, ink);
  }
  const int ty = y0 + 4 + 6 * kRowH;
  text(im, 4, ty, "F1-F4 PUSH  F5-F12 TURN  CTRL+1-8 OR KP SOUND 1-8", kWhite);
  text(im, 4, ty + 9, "CTRL+K KEYS F FULL ,. ENGINE [ ] SCREENS  ESC QUIT", 0xFFA0A0B8u);
  char st[80];
  std::snprintf(st, sizeof st, "%s OCT%+d VOL%d%s%s", screen.c_str(), ui.octave, ui.volume * 100 / 32767,
                ui.shift ? " SHIFT" : "", ui.power_off ? " OFF" : "");
  const int last = ty + 18;
  text(im, 4, last, st, kDim);
  for (int i = 0; i < 4; ++i)
    rect(im, kW - 4 - 4 * 9 + i * 9, last + (i == ui.last_enc ? 0 : 2), 7, i == ui.last_enc ? 7 : 3, kEnc[i]);
}

}  // namespace

int run_ui(Core& core, AssetProvider& assets, Worker* worker, const UiOptions& opt) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_Log("op1-og: no display (%s)", SDL_GetError());
    return 2;
  }
  const bool kb = opt.keyboard_overlay;
  const int ch = kH + (kb ? kKbH + kLegendH : 0);
  const char* scale_env = std::getenv("OP1_OG_SCALE");  // run.sh --scale; main.cpp does not parse it
  const int scale = opt.scale > 0 ? opt.scale : (scale_env && std::atoi(scale_env) > 0 ? std::atoi(scale_env) : 3);
  const char* fs_env = std::getenv("OP1_OG_FULLSCREEN");
  SDL_Window* win = SDL_CreateWindow("OP-1", kW * scale, ch * scale,
                                     SDL_WINDOW_RESIZABLE | (fs_env && *fs_env == '1' ? SDL_WINDOW_FULLSCREEN : 0));
  SDL_Renderer* ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
  if (!ren) {
    SDL_Log("op1-og: cannot open a window (%s)", SDL_GetError());
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return 2;
  }
  SDL_SetRenderVSync(ren, 0);  // never block the input loop on the compositor (see the header)
  SDL_SetRenderLogicalPresentation(ren, kW, ch, SDL_LOGICAL_PRESENTATION_LETTERBOX);
  SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, ch);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);

  UiModel ui(core, assets);
  ui.set_worker(worker);  // sound keys 1-8 load factory presets
  Script script;
  std::string spec = opt.script;
  if (spec.empty())
    if (const char* sc = std::getenv("OP1_OG_SCRIPT")) spec = sc;
  if (!spec.empty()) {
    std::string err;
    if (!script.load(spec, err)) SDL_Log("op1-og: script: %s", err.c_str());
  }

  Image canvas(kW, ch), disp(kW, kH);
  bool legend = true;
  std::string title;
  std::vector<double> waits_ms;  // input event age when handled
  waits_ms.reserve(4096);
  const Uint64 t0 = SDL_GetTicksNS();
  const Uint64 t_end = opt.duration_s > 0 ? t0 + Uint64(opt.duration_s * 1e9) : 0;
  const Uint64 frame_ns = 16666667;
  Uint64 next_frame = t0;
  std::vector<std::string> shots, due;
  bool run = true, quit_script = false, window_shot_done = false;
  uint64_t frames = 0;

  auto handle = [&](const SDL_Event& e) {
    if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) run = false;
    if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0) {
      turn_last(ui, (e.wheel.y > 0 ? 1 : -1) * ((SDL_GetModState() & SDL_KMOD_CTRL) ? 8 : 1));
      waits_ms.push_back(double(SDL_GetTicksNS() - e.wheel.timestamp) / 1e6);
    }
    if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP) return;
    const KeyAction a = map_key(ui, e.key.scancode, e.key.mod, e.type == SDL_EVENT_KEY_DOWN, e.key.repeat);
    if (!e.key.repeat && e.key.timestamp) waits_ms.push_back(double(SDL_GetTicksNS() - e.key.timestamp) / 1e6);
    if (a.quit) run = false;
    if (a.legend && kb) legend = !legend;
    if (a.fullscreen) SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) == 0);
  };

  while (run) {
    // wait for input until the next frame is due, and handle each event as soon as it arrives
    for (;;) {
      const Uint64 now = SDL_GetTicksNS();
      if (now >= next_frame) break;
      SDL_Event e;
      const Sint32 ms = Sint32((next_frame - now + 999999) / 1000000);
      if (SDL_WaitEventTimeout(&e, std::max<Sint32>(1, ms))) {
        handle(e);
        while (SDL_PollEvent(&e)) handle(e);
      }
      if (!run) break;
    }
    if (!run) break;
    const Uint64 now = SDL_GetTicksNS();
    next_frame = std::max(next_frame + frame_ns, now);
    const double t = double(now - t0) / 1e9;

    due.clear();
    script.run(ui, t, due, quit_script);
    const std::string& name = ui.render(disp, t);
    for (const std::string& p : shots)
      if (!save_image(p, disp)) SDL_Log("op1-og: cannot write %s", p.c_str());
    shots = due;
    if (quit_script && shots.empty()) run = false;

    const std::string want_title = "OP-1 - " + (name.empty() ? std::string("(no screen)") : name);
    if (want_title != title) {
      title = want_title;
      SDL_SetWindowTitle(win, title.c_str());
    }
    std::copy(disp.px.begin(), disp.px.end(), canvas.px.begin());
    if (kb) {
      draw_keyboard(canvas, kH, ui.state().held, 53 + 12 * ui.state().octave);
      if (legend) draw_legend(canvas, kH + kKbH, ui.state(), name);
      else rect(canvas, 0, kH + kKbH, kW, kLegendH, 0xFF08080Cu);
    }
    SDL_UpdateTexture(tex, nullptr, canvas.px.data(), kW * 4);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, nullptr, nullptr);
    if (!opt.screenshot.empty() && !window_shot_done && t >= 1.5) {  // --screenshot: the window, once
      window_shot_done = true;
      if (SDL_Surface* su = SDL_RenderReadPixels(ren, nullptr)) {
        if (!SDL_SaveBMP(su, opt.screenshot.c_str())) SDL_Log("op1-og: screenshot failed: %s", SDL_GetError());
        SDL_DestroySurface(su);
      }
    }
    SDL_RenderPresent(ren);
    ++frames;
    if (t_end && now >= t_end) run = false;
  }
  ui.release_all();
  if (!waits_ms.empty()) {
    std::sort(waits_ms.begin(), waits_ms.end());
    auto q = [&](double p) { return waits_ms[std::min(waits_ms.size() - 1, size_t(double(waits_ms.size()) * p))]; };
    std::fprintf(stderr, "op1-og: input event wait ms: n=%zu p50=%.2f p95=%.2f max=%.2f (%llu frames)\n", waits_ms.size(),
                 q(0.5), q(0.95), waits_ms.back(), (unsigned long long)frames);
  }
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}

}  // namespace op1
