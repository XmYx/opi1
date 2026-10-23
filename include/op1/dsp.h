// Contract of the engine track (t4): instruments (engine + voices + envelopes + play mode + their own
// effect and LFO) and effects, created by factories that construct into core-owned storage.
// Owner: t3 (interface); t4 implements in src/dsp/.
#pragma once

#include <cstddef>
#include <cstdint>

#include "op1/config.h"
#include "op1/messages.h"
#include "op1/patch.h"
#include "op1/snapshot.h"

namespace op1 {

// Inputs that modulate sound besides notes: MIDI controllers, the accelerometer and the input level the
// element LFO reads (firmware-behaviour.md §6). Written by the core before each chunk.
struct ControlInputs {
  int16_t cc[128] = {};          // last value per MIDI CC (0..127)
  int16_t pitch_bend = 0;        // -8192..8191
  int16_t accel[3] = {};         // accelerometer x/y/z (no sensor on a desktop: 0 unless the UI maps one)
  int32_t input_level = 0;       // live input envelope, Q1.31
};

// Passed to every module's process(). frames is 1..kBlock and a chunk never crosses a multiple of
// kBlock, so block_start (frame % kBlock == 0) marks the OG's 64-frame control-rate tick.
struct ProcessContext {
  int64_t frame = 0;             // absolute frame index of the first sample of this chunk
  int frames = 0;
  bool block_start = false;
  int16_t bpm_tenths = 1200;
  const Sample* in_l = nullptr;  // live input (mic/line) for this chunk
  const Sample* in_r = nullptr;
  const ControlInputs* controls = nullptr;
};

// Operations for Domain::Synth / Domain::Drum commands. t4 may append, never renumber.
enum class InstrumentOp : uint16_t {
  SetType = 1,          // value = InstrumentType; handled by the core (engine switch, see core.h)
  SetKnob = 2,          // index 0..7, value = Param
  SetAdsr = 3,
  SetFxParam = 4,
  SetLfoParam = 5,
  SetFxType = 6,        // value = FxType
  SetLfoType = 7,
  SetFxOn = 8,          // value 0/1
  SetLfoOn = 9,
  SetOctave = 10,
  LoadSynthPatch = 11,  // ptr = SynthPatch*, retired after the copy
  LoadDrumPatch = 12,   // ptr = DrumPatch*
  SetSample = 13,       // ptr = SampleData*, kept until replaced, then retired
  SetDrumKey = 14,      // index = key 0..23, value = packed per-key edit (t4 defines)
};

// All methods run on the audio thread and must follow the realtime rules (architecture.md).
class Instrument {
 public:
  virtual ~Instrument() = default;  // must not free memory: instances live in core-owned storage
  virtual InstrumentType type() const noexcept = 0;
  virtual void note_on(int note, int velocity, int offset) noexcept = 0;  // offset: frame in chunk, or -1
  virtual void note_off(int note, int offset) noexcept = 0;
  virtual void all_notes_off() noexcept = 0;
  virtual void command(const Command& c) noexcept = 0;                   // InstrumentOp
  // Writes (does not add) ctx.frames samples to l and r.
  virtual void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept = 0;
  virtual void view(InstrumentView& v) const noexcept = 0;
};

// One effect. The firmware has separate stereo (master) and mono (instrument) implementations.
class Effect {
 public:
  virtual ~Effect() = default;
  virtual void set_param(int index, Param value) noexcept = 0;
  virtual void reset() noexcept = 0;
  virtual void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept = 0;  // in place; r null = mono
  virtual void view(int32_t display[16]) const noexcept = 0;
};

// ---- factories, implemented by t4 in src/dsp/
// Each constructs into `storage` with placement new and never allocates, so engines can be switched on
// the audio thread. nullptr if the type is unknown or does not fit in `size`.
inline constexpr size_t kInstrumentStorage = size_t(4) << 20;  // per instrument slot; t4 may raise it
inline constexpr size_t kEffectStorage = size_t(2) << 20;
Instrument* create_instrument(InstrumentType type, void* storage, size_t size) noexcept;
Effect* create_effect(FxType type, bool stereo, void* storage, size_t size) noexcept;
void dsp_init() noexcept;  // once, on the main thread, before any audio (tables, etc.)
// t5 (2026-10-23): an effect type's default parameters (op1_factory.db fx_types.default_params), so the
// master effect loads them on a type switch as the instrument effect does. Never null.
const Param* fx_type_defaults(FxType type) noexcept;

}  // namespace op1
