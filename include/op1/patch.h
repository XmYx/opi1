// Patch and sample data as the OG stores them (firmware-behaviour.md §4-§6, §8): the enums keep the
// firmware's own ids, the structs mirror the JSON fields of op1_factory.db and user patches.
// t5 (state) fills these from JSON; t4 (instruments) reads them. Owner: t3.
#pragma once

#include <cstdint>

#include "op1/config.h"

namespace op1 {

// Instrument-type enum of the synth factory (name table 0x018DF7BC, jump table 0x018F5A04).
enum class InstrumentType : uint8_t {
  Fm = 0, Digital = 1, DrWave = 2, String = 3, Cluster = 4, Pulse = 5, Sampler = 6, Phase = 7,
  Drum = 8, Dbox = 9, Ls = 10, Dna = 11, DSynth = 12, Voltage = 13, Iter = 14, Count = 15,
};
// Effect ids (table 0x018DD810). Filter (Mmmf) is hidden on R.246: no fx_types row.
enum class FxType : uint8_t { Delay = 0, Grid = 1, Punch = 2, Phone = 3, Spring = 4, Filter = 5, Nitro = 6, Cwo = 7, Count = 8 };
// LFO order of the name table 0x018E13C8 (db ids are these + 1).
enum class LfoType : uint8_t { Tremolo = 0, Value = 1, Random = 2, Element = 3, Midi = 4, Bend = 5, Crank = 6, Count = 7 };
// Sequencers (table 0x018FA634; db ids are these + 1).
enum class SeqType : uint8_t { Pattern = 0, Endless = 1, Tombola = 2, Finger = 3, Sketch = 4, Arpeggio = 5, Count = 6 };

constexpr bool is_drum(InstrumentType t) { return t == InstrumentType::Drum || t == InstrumentType::Dbox; }

struct SynthPatch {
  InstrumentType type = InstrumentType::Fm;
  char name[32] = {};
  int8_t octave = 0;
  int16_t synth_version = 0;
  Param knobs[kParams] = {};     // slots 0-3 follow the encoders, 4-7 are the shift page (fm, dsynth)
  Param adsr[kParams] = {};      // attack, decay, sustain, release, then play mode and portamento
  FxType fx_type = FxType::Delay;
  bool fx_active = false;
  Param fx_params[kParams] = {};
  LfoType lfo_type = LfoType::Tremolo;
  bool lfo_active = false;
  Param lfo_params[kParams] = {};
  float base_freq = 0;           // sampler only
};

struct DrumPatch {
  InstrumentType type = InstrumentType::Drum;  // Drum or Dbox
  int16_t drum_version = 0;
  int8_t octave = 0;
  Param dyna_env[kParams] = {};
  FxType fx_type = FxType::Delay;
  bool fx_active = false;
  Param fx_params[kParams] = {};
  LfoType lfo_type = LfoType::Tremolo;
  bool lfo_active = false;
  Param lfo_params[kParams] = {};
  // one slice of the kit sample per key
  int32_t start[kDrumKeys] = {}, end[kDrumKeys] = {};
  int16_t pitch[kDrumKeys] = {}, volume[kDrumKeys] = {};
  uint8_t playmode[kDrumKeys] = {}, reverse[kDrumKeys] = {};
  // dbox kits carry dbox_data instead of slices: t5 parses it into this opaque block for t4.
  uint8_t dbox_data[256] = {};
  uint16_t dbox_size = 0;
  // ---- appended by t4 (2026-10-23, docs/interface-changes.md). The firmware stores per-key playmode and
  // reverse as 16-bit values around a neutral 8192 (og-behaviour-details.md §4), which uint8_t cannot hold,
  // and dbox kits as 24 keys x 8 signed values (384 bytes, more than dbox_data). t5 fills these with the
  // stored values as they are; t4 reads these instead of playmode/reverse/dbox_data.
  int16_t playmode16[kDrumKeys] = {8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192,
                                   8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192};
  int16_t reverse16[kDrumKeys] = {8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192,
                                  8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192};
  Param dbox_params[kDrumKeys][kParams] = {};
  uint8_t dbox_keys = 0;  // keys filled in dbox_params (0 = the dbox instrument keeps its default kit)
};

// User-provided audio for the sampler and drum engines. Never Teenage Engineering assets in the
// tree: files come from the user at run time (architecture.md, "Assets").
struct SampleData {
  const int16_t* frames = nullptr;  // interleaved if channels == 2
  int32_t length = 0;               // frames (synth sampler 6 s, drum kit 12 s at 44.1 kHz)
  int32_t channels = 1;
  int32_t sample_rate = kSampleRate;
  // Appended by t2 (2026-10-23): the sample's root frequency in Hz (a preset's base_freq), so the sampler
  // plays it in tune when the sample arrives without a patch (engine switch). 0 = keep the engine's root.
  float root_hz = 0;
};

}  // namespace op1
