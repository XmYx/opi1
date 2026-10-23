// What the other threads send into the audio thread. Two plain, trivially copyable message types
// travel through SpscQueue: Events (timed musical input) and Commands (state changes).
// Owner: t3. Op-code enums live with their domain owner (dsp.h, tape.h, sequencer.h, io.h), so a
// track adds operations without touching anyone else's file.
#pragma once

#include <cstdint>

#include "op1/config.h"

namespace op1 {

// ---------------------------------------------------------------- events

enum class EventType : uint8_t {
  NoteOn, NoteOff, ControlChange, PitchBend, ChannelPressure, ProgramChange,
  Clock, Start, Continue, Stop, SongPosition,
};
enum class Source : uint8_t { Keyboard, Midi, Sequencer, Internal };
// Where a note goes. Auto = the instrument of the current mode (synth or drum), as on the device.
enum class Target : uint8_t { Auto, Synth, Drum };

struct Event {
  int64_t time_ns = 0;      // CLOCK_MONOTONIC when it entered the process (MIDI receive, key press)
  EventType type = EventType::NoteOn;
  Source src = Source::Internal;
  Target target = Target::Auto;
  uint8_t channel = 0;
  uint8_t a = 0;            // note / CC number / program
  uint8_t b = 0;            // velocity / CC value
  int16_t value = 0;        // pitch bend (-8192..8191), song position
  int32_t offset = -1;      // frame offset inside the current chunk if known (sequencer), else -1 = chunk start
};

inline Event note_on(uint8_t note, uint8_t vel, Source src, int64_t t_ns, Target target = Target::Auto) {
  Event e;
  e.time_ns = t_ns; e.type = EventType::NoteOn; e.src = src; e.target = target; e.a = note; e.b = vel;
  return e;
}
inline Event note_off(uint8_t note, Source src, int64_t t_ns, Target target = Target::Auto) {
  Event e;
  e.time_ns = t_ns; e.type = EventType::NoteOff; e.src = src; e.target = target; e.a = note;
  return e;
}

// ---------------------------------------------------------------- commands

// Which module handles a command. The core routes by domain and never interprets `op` for other domains.
enum class Domain : uint8_t { Core, Synth, Drum, Tape, Mixer, Seq, Tempo, Midi, Count };

struct Command {
  Domain domain = Domain::Core;
  uint8_t slot = 0;         // free for the domain (e.g. tape track, mixer channel)
  uint16_t op = 0;          // domain-specific op code (enum in the owner's header)
  int32_t index = 0;
  int32_t value = 0;
  // Ownership hand-over for objects built off the audio thread (patch, sample, ...). The audio thread
  // copies what it needs and hands the pointer back through Core::retire(); the worker frees it there.
  void* ptr = nullptr;
};

// Core-domain operations (owner t3).
enum class CoreOp : uint16_t {
  SetMode = 1,       // value = Mode
  AllNotesOff = 2,
  SetInputGain = 3,  // value = Q1.15 gain
  SetOutputVolume = 4,  // t6 addition (keys task): the OG volume knob, value = Q1.15 gain on the output
};

// The edit modes of the OG (firmware-behaviour.md §10, ModeSwitcher). The UI owns mode logic; the core
// only needs to know which instrument a Target::Auto note plays.
enum class Mode : uint8_t {
  Synth, Drum, Tape, Mixer, Tempo, Album, Browser, Communication, Clock, Lander, Tune,
  Battery, Debug, ClassD, Codec, Gain, SignalFlow,
};

}  // namespace op1
