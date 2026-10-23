// Panel keys (owner: t6, keys task; KEYMAP.md): the PC keyboard reaches every OG panel control.
//   1. the map: 24 musical keys F..E, each on its own PC key; every OP-1 key, the four encoder pushes and
//      turns, the volume knob and the power switch have a PC key; no PC key is mapped twice;
//   2. through map_key against a real Core: encoder push resets a parameter, split/join cut and merge a tape
//      take, the arrows shift the octave in synth mode, sound keys work as tape tricks, volume and power act.
// No firmware image is needed.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <memory>
#include <set>
#include <vector>

#include "check.h"
#include "../../src/gui/ui_input.h"
#include "../../src/gui/ui_model.h"
#include "op1/core.h"
#include "op1/ui.h"

namespace {

using op1::KeyKind;
using op1::Mode;
using op1::OpKey;

struct Rig {
  op1::Core core;
  std::unique_ptr<op1::AssetProvider> assets = op1::create_asset_provider();
  op1::UiModel ui{core, *assets};
  op1::svg::Image img{op1::kDisplayW, op1::kDisplayH};
  std::vector<op1::Sample> l = std::vector<op1::Sample>(64), r = std::vector<op1::Sample>(64);
  double t = 0;
  void pump(double sec) {
    const int blocks = int(sec * op1::kSampleRate / 64) + 1;
    for (int i = 0; i < blocks; ++i) core.process(nullptr, nullptr, l.data(), r.data(), 64);
    t += sec;
    ui.render(img, t);
  }
  void tap(SDL_Scancode sc, uint16_t mod = 0) {
    op1::map_key(ui, sc, mod, true, false);
    op1::map_key(ui, sc, mod, false, false);
    pump(0.01);
  }
  const op1::CoreSnapshot& snap() { return core.snapshot(); }
};

void the_map() {
  std::set<int> notes, keys, modes, pushes, turns, scancodes;
  int volume = 0, power = 0;
  for (const op1::PanelKey& k : op1::panel_keys()) {
    CHECK(scancodes.insert(k.scancode).second);  // no PC key twice
    switch (k.kind) {
      case KeyKind::Note: CHECK(notes.insert(k.arg).second); break;
      case KeyKind::Key: keys.insert(k.arg); break;
      case KeyKind::Mode: modes.insert(k.arg); break;
      case KeyKind::Push: pushes.insert(k.arg); break;
      case KeyKind::Turn: turns.insert(k.arg * 2 + (k.dir > 0)); break;
      case KeyKind::Volume: volume += k.dir; break;
      case KeyKind::Power: ++power; break;
      default: break;
    }
  }
  CHECK_EQ(int(notes.size()), 24);
  CHECK_EQ(*notes.begin(), 0);
  CHECK_EQ(*notes.rbegin(), 23);
  for (OpKey k : {OpKey::Shift, OpKey::T1, OpKey::T2, OpKey::T3, OpKey::T4, OpKey::Seq, OpKey::Help, OpKey::Input,
                  OpKey::Record, OpKey::Play, OpKey::Stop, OpKey::Left, OpKey::Right, OpKey::Lift, OpKey::Drop,
                  OpKey::Split})
    CHECK(keys.count(int(k)) == 1);
  for (Mode m : {Mode::Synth, Mode::Drum, Mode::Tape, Mode::Mixer, Mode::Tempo, Mode::Communication})
    CHECK(modes.count(int(m)) == 1);
  CHECK_EQ(int(pushes.size()), 4);
  CHECK_EQ(int(turns.size()), 8);
  CHECK_EQ(volume, 0);  // one up, one down
  CHECK_EQ(power, 1);
}

void notes_and_octave() {
  Rig g;
  g.pump(0.01);
  // all 24 musical keys at once: F3 (53) .. E5 (76) at octave 0
  for (const op1::PanelKey& k : op1::panel_keys())
    if (k.kind == KeyKind::Note) op1::map_key(g.ui, k.scancode, 0, true, false);
  int held = 0;
  for (int n = 0; n < 128; ++n) held += g.ui.state().held[n];
  CHECK_EQ(held, 24);
  CHECK(g.ui.state().held[53] && g.ui.state().held[76]);
  for (const op1::PanelKey& k : op1::panel_keys())
    if (k.kind == KeyKind::Note) op1::map_key(g.ui, k.scancode, 0, false, false);
  held = 0;
  for (int n = 0; n < 128; ++n) held += g.ui.state().held[n];
  CHECK_EQ(held, 0);
  // the arrows are the octave keys in synth mode
  g.tap(SDL_SCANCODE_RIGHT);
  CHECK_EQ(g.ui.state().octave, 1);
  g.tap(SDL_SCANCODE_LEFT);
  g.tap(SDL_SCANCODE_LEFT);
  CHECK_EQ(g.ui.state().octave, -1);
}

void encoder_push() {
  Rig g;
  g.pump(0.05);
  const int k0 = g.snap().synth.knobs[0];
  for (int i = 0; i < 10; ++i) op1::map_key(g.ui, SDL_SCANCODE_F6, 0, true, i > 0);  // blue + held (repeat)
  op1::map_key(g.ui, SDL_SCANCODE_F6, 0, false, false);
  g.pump(0.01);
  CHECK(g.snap().synth.knobs[0] != k0);
  g.tap(SDL_SCANCODE_F1);  // push blue: back to the engine's value
  CHECK_EQ(int(g.snap().synth.knobs[0]), k0);
  CHECK_EQ(g.ui.state().pushes[0], 1);
  // mixer: push green = track 2 back to full volume
  g.tap(SDL_SCANCODE_4);
  op1::map_key(g.ui, SDL_SCANCODE_F7, SDL_KMOD_LCTRL, true, false);  // Ctrl = 8 detents
  g.pump(0.01);
  CHECK(g.snap().mixer.volume[1] < 32767 - 2000);
  g.tap(SDL_SCANCODE_F2);
  CHECK_EQ(int(g.snap().mixer.volume[1]), 32767);
  // tempo: push blue = 120 bpm
  g.tap(SDL_SCANCODE_5);
  g.tap(SDL_SCANCODE_F6);
  CHECK(g.snap().tempo.bpm_tenths != 1200);
  g.tap(SDL_SCANCODE_F1);
  CHECK_EQ(int(g.snap().tempo.bpm_tenths), 1200);
}

void tape_split_and_tricks() {
  Rig g;
  g.tap(SDL_SCANCODE_3);  // tape
  g.tap(SDL_SCANCODE_Z);  // record
  g.tap(SDL_SCANCODE_X);  // play
  op1::map_key(g.ui, SDL_SCANCODE_V, 0, true, false);
  g.pump(1.0);
  op1::map_key(g.ui, SDL_SCANCODE_V, 0, false, false);
  g.tap(SDL_SCANCODE_C);  // stop
  CHECK(!g.snap().tape.playing);
  const int clips = g.snap().tape.display[3];
  CHECK(clips >= 1);
  // into the take, then split: two takes, the one under the head starts at the head
  g.ui.encoder(0, -5);  // blue scrubs back half a second
  g.pump(0.01);
  const int pos = g.snap().tape.position;
  CHECK(g.snap().tape.display[6] >= 0 && g.snap().tape.display[6] < pos);
  g.tap(SDL_SCANCODE_D);  // split
  CHECK_EQ(g.snap().tape.display[3], clips + 1);
  CHECK_EQ(g.snap().tape.display[6], pos);
  g.ui.key(OpKey::Shift, true);  // shift + split = join
  g.tap(SDL_SCANCODE_D);
  g.ui.key(OpKey::Shift, false);
  g.pump(0.01);
  CHECK_EQ(g.snap().tape.display[3], clips);
  CHECK(g.snap().tape.display[6] < pos);
  // sound keys as tape tricks: Ctrl+1 loop in, keypad 5 reverse
  g.tap(SDL_SCANCODE_1, SDL_KMOD_LCTRL);
  CHECK_EQ(g.snap().tape.loop_in, pos);
  CHECK(!g.snap().tape.reverse);
  g.tap(SDL_SCANCODE_KP_5);
  CHECK(g.snap().tape.reverse);
}

void volume_and_power() {
  Rig g;
  g.tap(SDL_SCANCODE_PAGEDOWN);
  CHECK(g.ui.state().volume < 32767);
  g.tap(SDL_SCANCODE_PAGEUP);
  CHECK_EQ(g.ui.state().volume, 32767);
  g.tap(SDL_SCANCODE_END);
  CHECK(g.ui.state().power_off);
  CHECK(g.ui.render(g.img, g.t) == "off");
  op1::map_key(g.ui, SDL_SCANCODE_V, 0, true, false);  // keys do nothing while off
  CHECK(g.ui.state().last_note < 0);
  op1::map_key(g.ui, SDL_SCANCODE_V, 0, false, false);
  g.tap(SDL_SCANCODE_END);
  CHECK(!g.ui.state().power_off);
}

// t6 review (Sir): hold combos. Record + a note starts the tape recording; shift + record arms overdub; a note
// released while Ctrl is held lets go (before, the release was eaten by the Ctrl layer and the note hung).
void record_combos_and_ctrl_release() {
  Rig g;
  g.tap(SDL_SCANCODE_1);  // synth
  op1::map_key(g.ui, SDL_SCANCODE_Z, 0, true, false);  // hold record
  g.pump(0.01);
  CHECK(g.snap().tape.armed && !g.snap().tape.playing);
  CHECK(g.ui.state().rec_held);
  op1::map_key(g.ui, SDL_SCANCODE_V, 0, true, false);  // + a note: the tape starts, recording
  g.pump(0.2);
  CHECK(g.snap().tape.playing);
  CHECK(g.snap().tape.recording);
  op1::map_key(g.ui, SDL_SCANCODE_Z, 0, false, false);
  op1::map_key(g.ui, SDL_SCANCODE_V, 0, false, false);
  g.tap(SDL_SCANCODE_C);  // stop: disarms
  g.pump(0.05);
  CHECK(!g.snap().tape.playing && !g.snap().tape.armed);
  CHECK(!g.ui.state().rec_held);
  // armed (record tapped, then released) + a note also starts it
  g.tap(SDL_SCANCODE_Z);
  op1::map_key(g.ui, SDL_SCANCODE_B, 0, true, false);
  g.pump(0.1);
  CHECK(g.snap().tape.playing && g.snap().tape.recording);
  op1::map_key(g.ui, SDL_SCANCODE_B, 0, false, false);
  g.tap(SDL_SCANCODE_C);
  g.pump(0.05);
  CHECK_EQ(g.snap().tape.display[4], 0);  // replace by default
  // shift + record: overdub armed; record again disarms; plain record arms replace again
  g.tap(SDL_SCANCODE_Z, SDL_KMOD_LSHIFT);
  op1::map_key(g.ui, SDL_SCANCODE_LSHIFT, 0, true, false);
  op1::map_key(g.ui, SDL_SCANCODE_Z, SDL_KMOD_LSHIFT, true, false);
  op1::map_key(g.ui, SDL_SCANCODE_Z, SDL_KMOD_LSHIFT, false, false);
  op1::map_key(g.ui, SDL_SCANCODE_LSHIFT, 0, false, false);
  g.pump(0.05);
  CHECK(g.snap().tape.armed);
  CHECK_EQ(g.snap().tape.display[4], 1);
  g.tap(SDL_SCANCODE_Z);
  g.pump(0.05);
  CHECK(!g.snap().tape.armed);
  g.tap(SDL_SCANCODE_Z);
  g.pump(0.05);
  CHECK(g.snap().tape.armed);
  CHECK_EQ(g.snap().tape.display[4], 0);
  g.tap(SDL_SCANCODE_Z);
  // Ctrl pressed while a note is held: the release still ends the note
  op1::map_key(g.ui, SDL_SCANCODE_N, 0, true, false);
  g.pump(0.01);
  const int n = g.ui.state().last_note;
  CHECK(n >= 0 && g.ui.state().held[n]);
  op1::map_key(g.ui, SDL_SCANCODE_N, SDL_KMOD_LCTRL, false, false);
  g.pump(0.01);
  CHECK(!g.ui.state().held[n]);
}

// t6 review (Sir): shift + sound key opens the preset browser for that slot; blue picks the folder (engine),
// green the preset; an encoder push puts it on the slot, which keeps it. Needs the firmware image (the lists
// come from its op1_factory.db): skipped without one.
void preset_browser_push() {
  // the UI reads the lists when it is built, so the image is opened first (as op1-og does)
  const char* fw = std::getenv("OP1_OG_FIRMWARE");
  std::string err;
  auto opened = op1::create_asset_provider();
  if (!fw || !*fw || !opened->open(fw, err)) {
    std::printf("gui/test_keys: preset browser SKIPPED (OP1_OG_FIRMWARE not set or unreadable)\n");
    return;
  }
  struct G {
    op1::Core core;
    std::unique_ptr<op1::AssetProvider> assets;
    op1::UiModel ui;
    op1::svg::Image img{op1::kDisplayW, op1::kDisplayH};
    double t = 0;
    explicit G(std::unique_ptr<op1::AssetProvider> a) : assets(std::move(a)), ui(core, *assets) {}
    void tap(SDL_Scancode sc) { op1::map_key(ui, sc, 0, true, false); op1::map_key(ui, sc, 0, false, false); }
    void pump(double) { ui.render(img, t += 0.01); }
  } g(std::move(opened));
  g.tap(SDL_SCANCODE_1);
  op1::map_key(g.ui, SDL_SCANCODE_LSHIFT, 0, true, false);
  g.ui.key(OpKey::S3, true);
  g.ui.key(OpKey::S3, false);
  op1::map_key(g.ui, SDL_SCANCODE_LSHIFT, 0, false, false);
  g.pump(0.01);
  CHECK(g.ui.state().mode == Mode::Browser);
  CHECK(g.ui.render(g.img, g.t) == "presetbrowser");
  CHECK(g.ui.state().pb_folders.size() >= 10);  // one folder per engine (11 on R.00246)
  CHECK_EQ(g.ui.state().pb_slot, 2);
  g.ui.encoder(0, 3);
  g.ui.encoder(1, 2);
  const int f = g.ui.state().pb_folder, p = g.ui.state().pb_preset;
  CHECK_EQ(p, 2);
  g.ui.push(0);
  g.pump(0.01);
  CHECK(g.ui.state().mode == Mode::Synth);
  CHECK_EQ(g.ui.state().sound, 3);
  // reopening the browser on that slot finds the stored choice; another slot still has its default
  g.ui.key(OpKey::Shift, true);
  g.ui.key(OpKey::S3, true);
  g.ui.key(OpKey::Shift, false);
  CHECK_EQ(g.ui.state().pb_folder, f);
  CHECK_EQ(g.ui.state().pb_preset, p);
  g.ui.push(1);
  g.ui.key(OpKey::Shift, true);
  g.ui.key(OpKey::S4, true);
  g.ui.key(OpKey::Shift, false);
  CHECK(g.ui.state().pb_folder != f || g.ui.state().pb_preset != p);
}

}  // namespace

int main() {
  the_map();
  notes_and_octave();
  encoder_push();
  tape_split_and_tricks();
  volume_and_power();
  record_combos_and_ctrl_release();
  preset_browser_push();
  return check_result("gui/test_keys");
}
