// The OP-1 user interface without a window (owner: t6): which screen is shown, what the keys and the four
// encoders do on it, and drawing it with live data. ui_sdl.cpp maps the computer keyboard onto this and
// shows the result; the headless runner (ui_script.cpp) and tests/gui drive it directly.
//
// Pages follow the OG (firmware-behaviour.md §10, ModeSwitcher): in synth and drum mode T1..T4 open the
// instrument, envelope, effect and LFO pages; the sequencer key opens the side's sequencer. Pressing T3/T4
// on their own page steps the effect/LFO type, shift+T3/T4 switches it on/off. Tape: T1..T4 select the
// track. Mixer: T1 mixer, T2 EQ, T3 master effect, T4 master. Encoders always act on what the screen shows;
// shift selects the second page (parameters 4..7, or pan in the mixer).
//
// Panel keys (keys task, KEYMAP.md): every OG panel control has an entry point here. The four encoder pushes
// (push), split (tape split / shift: join), the eight sound keys S1..S8 (synth/drum: sound slot, shift: preset
// browser; tape/mixer: tape tricks), the volume knob (volume) and the power switch (power). Left/right are
// the octave keys in synth and drum mode, as on the device (guide 3.2), and move the tape elsewhere.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "op1/messages.h"
#include "op1/patch.h"
#include "op1/snapshot.h"
#include "svg.h"

namespace op1 {

class Core;
class AssetProvider;
class Worker;

// OP-1 function keys (not the musical keyboard, not the mode keys).
enum class OpKey : uint8_t {
  Shift, T1, T2, T3, T4, Seq, Help, Input, Record, Play, Stop, Left, Right, Lift, Drop, Rewind,
  Split, S1, S2, S3, S4, S5, S6, S7, S8,  // keys task: split and the sound keys 1-8 (tape tricks in tape/mixer)
};

enum class Page : uint8_t {
  Main,       // the mode's own screen (engine screen in synth/drum)
  Envelope,   // adsr (synth) / dynaenv (drum)
  PlayMode,   // playmode (synth, shift+T2)
  Fx,         // instrument effect / master effect in mixer mode
  Lfo,
  Seq,        // the side's sequencer
  Eq,         // mixer T2
  Master,     // mixer T4
  TapeConfig, // shift + tape key
  Help, Input, MicLine,
};

// What the screens know beyond the audio snapshot.
struct UiState {
  Mode mode = Mode::Synth;
  Page page = Page::Main;
  Mode inst_mode = Mode::Synth;   // last instrument mode (synth or drum): the side the sequencer key opens
  bool shift = false;
  int octave = 0;                 // -3..3 (keyboard transpose)
  int last_enc = 0;               // last encoder turned 0..3 (blue, green, white, orange)
  double enc_t[4] = {-1e9, -1e9, -1e9, -1e9};  // when each encoder last turned (seconds, ui clock)
  int enc_acc[4] = {};            // detents turned so far, signed (screens whose encoders drive no engine value)
  double key_t = -1e9;            // last musical key
  double octave_t = -1e9;         // last octave change (the octave pop-up)
  bool held[128] = {};            // notes held on the computer/on-screen keyboard
  int last_note = -1;
  double t = 0;                   // seconds since the UI started
  // values the snapshot does not report, kept by the UI as it sends them
  int seq_param[2][16] = {};      // per side
  int metronome_level = 32 * 511; // 0..32704: the level knob 0..64 x 511 (t5; the firmware starts at 0, the port at half)
  int sync = 0;                   // TempoOp::SetSync value
  bool sync_out = false;
  int com_page = 0;
  int master_fx[8] = {4000, 4000, 4000, 4000, 4000, 4000, 4000, 4000};  // master effect params as sent (mixer.cpp default)
  // keys task
  double push_t[4] = {-1e9, -1e9, -1e9, -1e9};  // when each encoder was last pushed
  int pushes[4] = {};             // encoder pushes so far
  int sound = 1;                  // sound key last pressed in synth/drum mode (1..8)
  int volume = 32767;             // volume knob, Q1.15 output gain
  double volume_t = -1e9;         // last volume change
  bool power_off = false;         // power switch: off = black screen, keys ignored, notes off
  // preset browser (shift + sound key; t6 review, Sir's request): the factory preset folders of the side
  // (synth: one per engine; drum: drum, dbox) on blue, that folder's presets on green; an encoder push loads
  // the preset onto the sound slot the browser was opened for, and that slot keeps it
  std::vector<std::string> pb_folders;
  std::vector<std::vector<std::string>> pb_names;  // preset names per folder
  int pb_folder = 0, pb_preset = 0, pb_slot = 0;   // cursors; slot 0..7
  bool rec_held = false;          // the record key is held (record + play / record + note start the tape)
};

// The template name of an effect / LFO / sequencer screen (firmware-behaviour.md §5, §6, §8).
const char* fx_screen(FxType t);
const char* lfo_screen(LfoType t);
const char* seq_screen(SeqType t);

class UiModel {
 public:
  UiModel(Core& core, AssetProvider& assets);
  ~UiModel();

  // ---- input (UI thread)
  void key(OpKey k, bool down);
  void set_mode(Mode m);                  // a mode key (synth, drum, tape, mixer, tempo, ...)
  void encoder(int index, int detents);   // 0..3 = blue, green, white, orange; shift applies
  void push(int index);                   // push (tap) encoder 0..3; what it does per page: KEYMAP.md
  void volume(int dir);                   // the volume knob, one step down (-1) or up (+1)
  void power();                           // the power switch: toggles off/on
  void note(int key, bool down);          // musical key 0..23 from the lowest F (F3 at octave 0), transpose applies
  void octave(int dir);
  void engine_step(int dir);              // previous/next synth engine (drum: drum <-> dbox)
  void browse(int dir);                   // step through all 61 firmware screens (0 = back to the mode's)
  bool browse_to(const std::string& name);  // show one template by name (tests, --screen); false if unknown

  // ---- output
  // Draws the current screen into dst (kDisplayW x kDisplayH) for time t (seconds). Returns the screen name.
  const std::string& render(svg::Image& dst, double t);
  const std::string& screen() const;
  const UiState& state() const;
  bool firmware() const;
  std::vector<std::string> screen_names() const;
  void release_all();                     // note-offs for everything held (window closing)
  void set_worker(Worker* w);             // optional: sound keys 1-8 load factory presets through it

  struct Impl;

 private:
  void sound_key(int n);                  // sound key 0..7 (OpKey::S1..S8)
  std::unique_ptr<Impl> impl_;
};

}  // namespace op1
