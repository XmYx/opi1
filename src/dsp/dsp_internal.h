// Internal contracts of the engine track (t4): the LFO interface, the ADSR, the per-type factory functions
// that factory.cpp dispatches to, and storage sizes. Not part of include/op1 (other tracks use dsp.h only).
#pragma once

#include <cstddef>
#include <cstdint>

#include "dsp_util.h"
#include "op1/dsp.h"

namespace op1::dsp {

// ---------------------------------------------------------------- storage
inline constexpr size_t kMonoFxStorage = size_t(512) << 10;  // largest mono effect (delay, 2.1 s line)
inline constexpr size_t kLfoStorage = 1024;

// ---------------------------------------------------------------- LFO
// What an LFO adds to an instrument for the next 64-frame block. Offsets are in stored-parameter units
// (the same scale as the patch values, +-32767), so an LFO moves a knob exactly as turning it would.
struct ModOut {
  s32 knob[kParams] = {};  // engine parameters 0..7
  s32 adsr[4] = {};        // attack, decay, sustain, release
  s32 fx[4] = {};          // effect parameters 0..3
  s32 pitch_q8 = 0;        // pitch offset, semitones * 256
  s32 volume15 = 32768;    // output gain, Q15 (32768 = unity, 0 = silent)
};

// What an LFO may read. Filled by the instrument at each block start.
struct LfoInputs {
  int bpm_tenths = 1200;
  int note_ons = 0;               // notes started since the previous block
  s32 envelope = 0;               // amplitude envelope of the newest voice, Q31
  s32 out_level = 0;              // peak of the instrument's own output over the previous block, Q31
  const ControlInputs* controls = nullptr;  // MIDI CCs, bend, accelerometer, input level (may be null)
};

// Destination encodings shared by the LFOs (firmware-behaviour.md §6: single or dual target).
enum class ModGroup : uint8_t { Engine, Envelope, Fx, Main };
struct ModTarget {
  ModGroup group = ModGroup::Engine;
  uint8_t index = 0;  // engine 0..7, envelope 0..3, fx 0..3, main 0 = pitch, 1 = volume
  bool off = false;
};
// Group + parameter (value, element, bend): group from one u15 encoder, parameter from another.
ModTarget target_of(s32 group_v, s32 param_v) noexcept;
// Flat list of the 18 targets (random, midi, crank): engine 1-8, envelope 1-4, fx 1-4, pitch, volume.
inline constexpr int kFlatTargets = 18;
ModTarget flat_target(s32 v) noexcept;
// Adds `amount` (Q15, +-32767 = full scale) to a target: full scale moves a parameter by +-32767, the
// pitch by +-12 semitones and the volume by +-100 %.
void add_mod(ModOut& m, const ModTarget& t, s32 amount15) noexcept;
// LFO rate: 0.05 .. 20 Hz from a u15 value, as a phase increment per 64-frame block.
u32 lfo_rate_inc(s32 v15) noexcept;
// Tempo-synced rate: divisions 4 bars .. 1/32 from a u15 value.
u32 lfo_sync_inc(s32 v15, int bpm_tenths) noexcept;

class Lfo {
 public:
  virtual ~Lfo() = default;
  // Once per 64-frame block. p = the eight stored LFO parameters.
  virtual void process(const Param p[kParams], const LfoInputs& in, ModOut& out) noexcept = 0;
  virtual void reset() noexcept = 0;
  // Screen data (lfo_display in InstrumentView); meaning documented in lfo.h per type.
  virtual void view(int32_t display[16]) const noexcept = 0;
};
Lfo* create_lfo(LfoType type, void* storage, size_t size) noexcept;

// ---------------------------------------------------------------- envelope
// Amplitude ADSR. Attack is a linear ramp, decay and release are exponential (time to -60 dB).
struct AdsrRates {
  u32 attack = 1u << 20;
  s32 decay = 1 << 20, sustain = kOne, release = 1 << 20;
};
AdsrRates adsr_rates(s32 a, s32 d, s32 s, s32 r) noexcept;  // stored values (A/D/R 0..16384, S 0..32767)

struct Adsr {
  enum Stage : uint8_t { Idle, Attack, Decay, Sustain, Release };
  Stage stage = Idle;
  s32 level = 0;
  void gate_on() noexcept { stage = Attack; }  // from the current level: no click on retrigger
  void gate_off() noexcept { if (stage != Idle) stage = Release; }
  void kill() noexcept { stage = Idle; level = 0; }
  bool active() const noexcept { return stage != Idle; }
  s32 tick(const AdsrRates& r) noexcept {
    switch (stage) {
      case Idle: return 0;
      case Attack: {
        const s64 v = s64(level) + r.attack;
        if (v >= kOne) { level = kOne; stage = Decay; } else { level = s32(v); }
        break;
      }
      case Decay: {
        const s64 d = ((s64(level) - r.sustain) * r.decay) >> 31;
        level = s32(s64(level) - (d > 0 ? d : (level > r.sustain ? 1 : d)));
        if (level <= r.sustain + 4096) { level = r.sustain; stage = Sustain; }
        break;
      }
      case Sustain: level = r.sustain; break;  // follows sustain changes
      case Release: {
        const s64 d = (s64(level) * r.release) >> 31;
        level = s32(s64(level) - (d > 0 ? d : 1));
        if (level < 200000) { level = 0; stage = Idle; }  // ~-80 dB
        break;
      }
    }
    return level;
  }
};

// ---------------------------------------------------------------- per-type factories
// Effects (one file each, fx_<name>.cpp). Mono = instrument position, stereo = master (firmware §5).
Effect* make_fx_delay(bool stereo, void* mem, size_t size) noexcept;
Effect* make_fx_grid(bool stereo, void* mem, size_t size) noexcept;
Effect* make_fx_punch(bool stereo, void* mem, size_t size) noexcept;
Effect* make_fx_phone(bool stereo, void* mem, size_t size) noexcept;
Effect* make_fx_spring(bool stereo, void* mem, size_t size) noexcept;
Effect* make_fx_filter(bool stereo, void* mem, size_t size) noexcept;
Effect* make_fx_nitro(bool stereo, void* mem, size_t size) noexcept;
Effect* make_fx_cwo(bool stereo, void* mem, size_t size) noexcept;

// LFOs (lfo_<name>.cpp).
Lfo* make_lfo_tremolo(void* mem, size_t size) noexcept;
Lfo* make_lfo_value(void* mem, size_t size) noexcept;
Lfo* make_lfo_random(void* mem, size_t size) noexcept;
Lfo* make_lfo_element(void* mem, size_t size) noexcept;
Lfo* make_lfo_midi(void* mem, size_t size) noexcept;
Lfo* make_lfo_bend(void* mem, size_t size) noexcept;
Lfo* make_lfo_crank(void* mem, size_t size) noexcept;

// Instruments (eng_<name>.cpp, drum.cpp, dbox.cpp).
Instrument* make_fm(void* mem, size_t size) noexcept;
Instrument* make_digital(void* mem, size_t size) noexcept;
Instrument* make_drwave(void* mem, size_t size) noexcept;
Instrument* make_string(void* mem, size_t size) noexcept;
Instrument* make_cluster(void* mem, size_t size) noexcept;
Instrument* make_pulse(void* mem, size_t size) noexcept;
Instrument* make_sampler(void* mem, size_t size) noexcept;
Instrument* make_phase(void* mem, size_t size) noexcept;
Instrument* make_drum(void* mem, size_t size) noexcept;
Instrument* make_dbox(void* mem, size_t size) noexcept;
Instrument* make_dna(void* mem, size_t size) noexcept;
Instrument* make_dsynth(void* mem, size_t size) noexcept;
Instrument* make_voltage(void* mem, size_t size) noexcept;
Instrument* make_iter(void* mem, size_t size) noexcept;
void init_dna_tables() noexcept;  // eng_dna.cpp, called by dsp_init()

// Type defaults from op1_factory.db default_params (og-behaviour-details.md §4). Engines without a db
// row (sampler, iter, filter) use values chosen here and documented in their files.
const Param* default_knobs(InstrumentType t) noexcept;
const Param* default_fx_params(FxType t) noexcept;
const Param* default_lfo_params(LfoType t) noexcept;
// Default ADSR of a fresh synth: attack 64, decay 6000, sustain 24000, release 5000, poly, no glide.
inline constexpr Param kDefaultAdsr[kParams] = {64, 6000, 24000, 5000, 2048, 64, 4000, 4000};

}  // namespace op1::dsp
