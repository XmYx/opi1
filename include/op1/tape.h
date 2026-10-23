// Contract of the tape and mixer part of t5: the 4-track tape (6:00, clips, lift/drop, loop, speed,
// reverse; firmware-behaviour.md §7) and the mixer/master section (§5 "Master section").
// Owner: t3 (interface); t5 implements in src/tape/.
#pragma once

#include <cstdint>
#include <memory>

#include "op1/config.h"
#include "op1/dsp.h"
#include "op1/messages.h"
#include "op1/snapshot.h"

namespace op1 {

// Domain::Tape operations. t5 may append, never renumber.
enum class TapeOp : uint16_t {
  Play = 1, Stop = 2,
  Record = 3,         // value 1 = arm/start recording on the selected track, 0 = stop
  SelectTrack = 4,    // value 0..3
  Seek = 5,           // value = frame
  SetLoop = 6,        // value 0/1
  SetLoopIn = 7, SetLoopOut = 8,
  SetSpeed = 9,       // value as the firmware stores tapespeed
  SetReverse = 10,    // value 0/1
  Lift = 11, Drop = 12,
  SetInputRouting = 13,  // what gets recorded (tapeinput / tapein%i), t5 defines values
  // t5 additions (2026-10-23, docs/interface-changes.md). Value meanings: src/tape/tape_engine.h.
  SetRecordMode = 14,    // 0 = replace (punch-in, default), 1 = overdub (sum with what is there)
  ClearTrack = 15,       // slot = track: erase its audio and clips
  // t6 addition (keys task): the split key. index 0 = split the take under the head at the head (selected
  // track), index 1 = join (shift + split): merge that take with the nearest take on the same track.
  Split = 16,
};

class Tape {
 public:
  virtual ~Tape() = default;
  virtual void command(const Command& c) noexcept = 0;
  // rec: the mono signal offered for recording this chunk (the core passes the live instrument + input
  // mix). Writes ctx.frames samples of each track's playback into tracks[0..3].
  virtual void process(const ProcessContext& ctx, const Sample* rec, Sample* const tracks[kTapeTracks]) noexcept = 0;
  virtual void view(TapeView& v) const noexcept = 0;
  // t5 addition: transport from the sequencer/MIDI side (Start, Continue, Stop, SongPosition with
  // value = MIDI song position in sixteenths). The core forwards these from the sequencer's note
  // sink; bpm_tenths converts song position to frames (og-behaviour-details.md §2.2).
  virtual void transport(const Event& e, int16_t bpm_tenths) noexcept {}
};

// Domain::Mixer operations (slot = tape track where it applies).
enum class MixerOp : uint16_t {
  SetVolume = 1, SetPan = 2,
  SetEq = 3,            // index 0..2
  SetMaster = 4,        // index 0..3 (master_params)
  SetMasterFx = 5,      // value = FxType
  SetMasterFxOn = 6,
  SetMasterFxParam = 7, // index 0..7
  // t5 (2026-10-23): the metronome click (metronome.cpp), value = level 0..32768 (the firmware's knob 0..64 x 511;
  // 0 = silent). The mixer adds it to the output after the master effect, so it is heard but not recorded.
  SetMetronome = 8,
};

class Mixer {
 public:
  virtual ~Mixer() = default;
  virtual void command(const Command& c) noexcept = 0;
  // tracks: tape playback; live_l/r: instruments and input as heard live. Writes the master output.
  virtual void process(const ProcessContext& ctx, const Sample* const tracks[kTapeTracks],
                       const Sample* live_l, const Sample* live_r, Sample* out_l, Sample* out_r) noexcept = 0;
  virtual void view(MixerView& v) const noexcept = 0;
};

// Main thread, at startup: may allocate (the tape owns 4 x kTapeLength samples) and must prefault it.
std::unique_ptr<Tape> create_tape();
std::unique_ptr<Mixer> create_mixer();

}  // namespace op1
