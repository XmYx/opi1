// What the audio thread publishes for the screens, once per 64-frame block, through a TripleBuffer.
// Each module fills its own view in its view() method; the UI (t6) only reads.
// The display[] arrays are free-form: the owning module documents what each slot means for its
// screen animation (t4 in src/dsp/<engine>.h, t5 in src/tape/, src/seq/), and t6 reads them.
// Owner: t3.
#pragma once

#include <cstdint>

#include "op1/config.h"
#include "op1/messages.h"
#include "op1/patch.h"

namespace op1 {

struct InstrumentView {
  InstrumentType type = InstrumentType::Fm;
  Param knobs[kParams] = {}, adsr[kParams] = {}, fx[kParams] = {}, lfo[kParams] = {};
  FxType fx_type = FxType::Delay;
  LfoType lfo_type = LfoType::Tremolo;
  bool fx_on = false, lfo_on = false;
  int8_t octave = 0;
  uint8_t voices_active = 0;     // bit per voice
  int32_t display[64] = {};      // engine-defined
  int32_t fx_display[16] = {};   // effect-defined
  int32_t lfo_display[16] = {};  // lfo-defined
  // Added by t3 for screens that read more than display[] holds (filled by the engine's view()):
  // sampler: the loaded sample, immutable while published. The worker must free a retired SampleData
  // only after a grace period (>= 1 s), so a snapshot the UI is still reading never points at freed memory.
  const SampleData* sample = nullptr;
  // drwave: the oscillator's current single cycle, 256 points of the last rendered voice, Q16
  // (+-65536 = full scale); wave_valid = false until the engine fills it.
  bool wave_valid = false;
  int32_t wave[256] = {};
};

struct TapeView {
  int32_t position = 0;          // frames, 0..kTapeLength
  bool playing = false, recording = false, loop = false, reverse = false;
  uint8_t track = 0;             // selected track 0..3
  int32_t loop_in = 0, loop_out = 0;
  int16_t speed = 0;             // tape speed control as the firmware stores it
  int16_t level[kTapeTracks] = {};
  int32_t display[32] = {};      // tape-defined (clip edges near the play head, lift/drop state, ...)
  // t4 (2026-10-23): what the tape screen draws. armed = record is on (also while stopped). The region being
  // recorded right now is [rec_start, rec_stop) on rec_track (rec_stop > rec_start only while recording); the
  // clips are every finished clip within kTapeViewSpan frames of the head, nearest first.
  static constexpr int kMaxViewClips = 48;
  static constexpr int32_t kTapeViewSpan = 800000;
  bool armed = false;
  uint8_t rec_track = 0;
  int32_t rec_start = 0, rec_stop = 0;
  int32_t nclips = 0;
  struct Clip { int32_t start = 0, stop = 0; uint8_t ch = 0; };
  Clip clips[kMaxViewClips] = {};
};

struct MixerView {
  Param volume[kTapeTracks] = {}, pan[kTapeTracks] = {};
  Param eq[3] = {};
  Param master[4] = {};
  FxType master_fx = FxType::Delay;
  bool master_fx_on = false;
  int16_t meter_l = 0, meter_r = 0;
  // t5 (2026-10-23): the master effect's parameters and its effect-defined display, as InstrumentView::fx and
  // fx_display, so the master effect screens show the engine's state rather than a copy kept by the UI.
  Param master_fx_params[kParams] = {};
  int32_t master_fx_display[16] = {};
};

struct SeqView {
  SeqType synth_seq = SeqType::Pattern, drum_seq = SeqType::Pattern;
  bool running = false;
  int16_t step = 0;
  int32_t display[64] = {};      // sequencer-defined (tombola balls, sketch points, ...)
  // t5 (2026-10-23): per side (0 synth, 1 drum), the selected sequencer's current SetParam values (index =
  // SeqOp::SetParam index, src/seq/seq_engines.h), so the UI and screens read the engine's own values.
  int32_t params[2][16] = {};
};

struct TempoView {
  int16_t bpm_tenths = 1200;
  bool metronome = false;
  bool external_clock = false;
};

struct EngineStats {
  uint64_t blocks = 0;
  uint32_t xruns = 0;
  float dsp_load_pct = 0;        // last block's processing time as % of its duration
  float dsp_load_peak_pct = 0;   // peak since start
};

struct CoreSnapshot {
  uint64_t frame = 0;
  Mode mode = Mode::Synth;
  InstrumentView synth, drum;
  TapeView tape;
  MixerView mixer;
  SeqView seq;
  TempoView tempo;
  EngineStats stats;
  Sample scope[kDisplayW] = {};  // last kDisplayW output samples (left), for the screens that draw a wave
};

}  // namespace op1
