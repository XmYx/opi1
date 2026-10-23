// Computer keyboard -> OP-1 mapping and the scripted-input player (owner: t6). The window (ui_sdl.cpp)
// and the headless runner use the same mapping, so a script exercises exactly what a person's key
// presses do. The map is by physical key position (SDL scancodes), so it is the same on any layout, and
// it mirrors the OG panel (TE OP-1 guide, chapter 2 "layout"; KEYMAP.md has both pictures):
//
//   Esc  [F1 F2 F3 F4]   [F5 F6][F7 F8]  [F9 F10][F11 F12]      the four encoders, as on the device's top row:
//   quit  push B G W O    blue -+ green-+  white -+ orange-+      F1-F4 push, F5-F12 turn (Ctrl = 8 steps)
//   `    1   2   3   4   5    6   7   8   9   0   -     =   Bksp
//   help syn drm tap mix tempo F#  G#  A#  .  C#  D#    in  com
//   Tab  Q   W   E   R    T   Y   U   I   O   P   [     ]   \ (backslash)
//   seq  T1  T2  T3  T4   F4  G4  A4  B4  C5  D5  E5    .   seq
//   Caps A    S    D     F   G   H   J   K   L   ;   '   Enter
//    .   lift drop split .   F#  G#  A#  .   C#  D#  .   rec
//   Shift Z   X    C     V   B   N   M   ,   .   /   Shift
//   shift rec play stop  F3  G3  A3  B3  C4  D4  E4  shift
//   Space play   <- -> left/right (synth/drum: octave -/+)   Up/Down lift/drop   PgUp/PgDn volume   End power
//   sound keys 1-8: Ctrl+1..8 or keypad 1-8 (tape/mixer: tape tricks)
//   window   Ctrl+K legend (with --keyboard)  Ctrl+F fullscreen  Esc quit  Ctrl+[ ] browse all 61 screens, Ctrl+\ back
//            Ctrl+, Ctrl+. previous/next engine  Ctrl+A album  Ctrl+B preset browser  Ctrl+C clock  Ctrl+T tune
//            Ctrl+G signal flow  Ctrl+L lander  (the mouse wheel turns the last used encoder)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "svg.h"
#include "ui_model.h"

namespace op1 {

struct KeyAction {
  bool handled = false;
  bool quit = false, fullscreen = false, legend = false;
};

// scancode/mod are SDL3 values (SDL_Scancode, SDL_Keymod): the physical key; repeat = auto-repeat.
KeyAction map_key(UiModel& ui, uint32_t scancode, uint16_t mod, bool down, bool repeat);
// Turns the last used encoder (mouse wheel).
void turn_last(UiModel& ui, int det);

// One key of the PC keyboard picture the legend draws: position in quarter key widths (row 0 = F-row,
// 5 = the space bar row), the SDL scancode, what it does on the OP-1 and a short label.
enum class KeyKind : uint8_t { None, Note, Gap, Key, Mode, Turn, Push, Volume, Power, Quit, Layer };
struct PanelKey {
  uint8_t row, x, w;       // x, w in quarter key widths
  uint16_t scancode;
  KeyKind kind;
  int8_t arg, dir;         // Note: key 0..23; Key: OpKey; Mode: Mode; Turn/Push: encoder; Turn/Volume: dir
  const char* label;
};
const std::vector<PanelKey>& panel_keys();

// Scripted input: steps separated by commas or new lines; "@FILE" reads them from a file.
//   key=NAME        press and release a computer key by its SDL key name, at its US-layout position (F1, Tab,
//                   c, Space, "shift+F3", "ctrl+1"; comma, period, slash, semicolon, minus, equals, grave,
//                   apostrophe, backslash, leftbracket, rightbracket also work); down=NAME / up=NAME hold or release one
//   op=NAME         press and release an OP-1 key by its panel name, independent of the PC map: synth drum tape
//                   mixer tempo help input com seq t1-t4 lift drop split rec play stop left right s1-s8 vol+
//                   vol- power ("shift+split" holds shift)
//   enc=E:D         turn encoder E (0-3 or blue/green/white/orange) by D detents (with the held shift)
//   press=E         push encoder E (0-3 or blue/green/white/orange)
//   wait=MS         let time pass (the audio keeps running)
//   shot=FILE       save the display (.png or .bmp, 2x) after the screen has been drawn
//   quit            end the run
class Script {
 public:
  bool load(const std::string& spec, std::string& err);
  bool empty() const { return steps_.empty(); }
  bool done() const { return at_ >= steps_.size(); }
  // Runs every step that is due at time t (seconds). Returns screenshots to take after the next render.
  void run(UiModel& ui, double t, std::vector<std::string>& shots, bool& quit);

 private:
  std::vector<std::string> steps_;
  size_t at_ = 0;
  double wait_until_ = 0;
};

// 2x nearest-neighbour PNG (stored, uncompressed deflate) or BMP by extension.
bool save_image(const std::string& path, const svg::Image& im, int scale = 2);

// Runs the UI without a window (no SDL video): renders at 60 Hz, plays the script, saves its shots.
// Returns when the script ends ("quit" or its last step), after duration_s, or never if both are unset.
// (declared in op1/ui.h: int run_ui_headless(Core&, AssetProvider&, const UiOptions&))

}  // namespace op1
