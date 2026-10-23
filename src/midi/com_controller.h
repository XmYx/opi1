// COM mode: the OP-1 as a MIDI controller (owner: t5; used by the UI, t6, on the UI thread).
// Turns encoder turns, key presses and program changes into the MIDI messages the firmware's
// src/main/ctls/com_controller.cpp sends (og-behaviour-details.md §2.1). The UI posts the returned events
// with core.post_ui(); the core hands controller messages to the sequencer, which sends them on the MIDI
// channel (Source::Keyboard: never confused with MIDI input).
//
//   encoders   CC = encoder + 1 (blue/green/white/orange = CC 1..4); with a page p > 0, CC = 53 + enc + 4(p-1).
//              Relative mode (midi_knobs_relative): the delta as 7-bit two's complement (+1 -> 1, -1 -> 127).
//              Absolute mode: a per-encoder accumulator, clamped 0..127, is sent.
//   keys       function key code k (0..12, 54..63) sends CC k + 5 = 127 on press, 0 on release; codes 0..9
//              are help, metronome, synth, drum, tape, mixer, T1..T4 -> CC 5..14. Track keys T1..T4
//              (codes 6..9) send CC 110 = 118 first, on press.
//   notes      note keys (codes 13..53) send note = (code - 13) + 53 + 12 x octave at velocity 100.
//   program    CC 110 = 118, then Program Change.
// Open in the firmware and so not reproduced: the page variable's source, the OP-Z layout tables, the CC 53 /
// 55 / 56 / 57 computed values, names of codes above 9, and the delta scaling (>> 8 / >> 10) of raw encoder
// words: this takes detent steps.
#pragma once

#include <cstdint>

#include "op1/messages.h"

namespace op1::midi {

class ComController {
 public:
  static constexpr int kMaxOut = 2;  // events written per call at most

  void set_relative(bool on) noexcept { relative_ = on; }
  void set_octave(int o) noexcept { octave_ = o < -4 ? -4 : (o > 4 ? 4 : o); }
  void set_page(int p) noexcept { page_ = p < 0 ? 0 : p; }

  // One encoder turn of `delta` detents. Returns the number of events written to out.
  int encoder(int knob, int delta, Event* out) noexcept {
    if (knob < 0 || knob > 3 || delta == 0) return 0;
    const int cc = page_ > 0 ? 53 + knob + 4 * (page_ - 1) : knob + 1;
    int value;
    if (relative_) {
      const int d = delta < -64 ? -64 : (delta > 63 ? 63 : delta);
      value = d < 0 ? d + 128 : d;  // 7-bit two's complement
    } else {
      acc_[knob] += delta;
      if (acc_[knob] < 0) acc_[knob] = 0;
      if (acc_[knob] > 127) acc_[knob] = 127;
      value = acc_[knob];
    }
    out[0] = cc_event(cc, value);
    return 1;
  }

  // A key press (down) or release. Note keys become notes; other keys become CCs.
  int key(int code, bool down, Event* out) noexcept {
    if (code < 0 || code >= 64) return 0;
    if (code >= 13 && code <= 53) {
      const int note = code - 13 + 53 + 12 * octave_;
      if (note < 0 || note > 127) return 0;
      out[0] = down ? note_on(uint8_t(note), 100, Source::Keyboard, 0) : note_off(uint8_t(note), Source::Keyboard, 0);
      return 1;
    }
    int n = 0;
    if (down && code >= 6 && code <= 9) out[n++] = cc_event(110, 118);  // track keys
    out[n++] = cc_event(code + 5, down ? 127 : 0);
    return n;
  }

  int program(int prog, Event* out) noexcept {
    out[0] = cc_event(110, 118);
    out[1] = Event{};
    out[1].type = EventType::ProgramChange;
    out[1].src = Source::Keyboard;
    out[1].a = uint8_t(prog & 127);
    return 2;
  }

  int accumulator(int knob) const noexcept { return knob >= 0 && knob < 4 ? acc_[knob] : 0; }

 private:
  static Event cc_event(int cc, int value) noexcept {
    Event e;
    e.type = EventType::ControlChange;
    e.src = Source::Keyboard;
    e.a = uint8_t(cc & 127);
    e.b = uint8_t(value & 127);
    return e;
  }
  bool relative_ = false;  // op1.db midi_knobs_relative default 0
  int octave_ = 0, page_ = 0;
  int acc_[4] = {};
};

}  // namespace op1::midi
