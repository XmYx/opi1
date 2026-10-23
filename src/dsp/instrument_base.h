// Shared host of every instrument (t4): the patch values, the instrument's own mono effect and LFO
// (firmware-behaviour.md §5-§6: mono effects only in synth/drum), sample-accurate note offsets, the
// 64-frame control tick, the LFO volume ramp and the common part of the screen view.
//
// OG instruments are mono (the tape tracks are mono; stereo starts at the mixer/master), so engines render
// one channel and the host copies it to both outputs.
#pragma once

#include <cstddef>
#include <cstring>

#include "dsp_internal.h"

namespace op1::dsp {

class InstrumentBase : public Instrument {
 public:
  explicit InstrumentBase(InstrumentType t) noexcept;
  ~InstrumentBase() override;

  InstrumentType type() const noexcept final { return type_; }
  void note_on(int note, int velocity, int offset) noexcept final;
  void note_off(int note, int offset) noexcept final;
  void all_notes_off() noexcept final;
  void command(const Command& c) noexcept final;
  void process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept final;
  void view(InstrumentView& v) const noexcept final;

 protected:
  // ---- hooks for the concrete instrument (audio thread)
  virtual void on_note(int note, bool on) noexcept = 0;
  virtual void on_all_off() noexcept = 0;
  virtual void block_control(const ProcessContext& ctx) noexcept = 0;  // every 64 frames, after the LFO
  virtual void render(const ProcessContext& ctx, s32* out, int n) noexcept = 0;  // write n mono samples
  virtual s32 newest_envelope() const noexcept = 0;
  virtual bool own_command(const Command& c) noexcept { (void)c; return false; }  // true = handled
  virtual void load_synth_patch(const SynthPatch& p) noexcept { (void)p; }
  virtual void load_drum_patch(const DrumPatch& p) noexcept { (void)p; }
  virtual void own_view(InstrumentView& v) const noexcept = 0;

  void set_fx_type(FxType t, bool load_defaults) noexcept;
  void set_lfo_type(LfoType t, bool load_defaults) noexcept;
  // Pitch-bend offset from the MIDI controls: +-2 semitones, in Q8.
  static s32 bend_q8(const ProcessContext& ctx) noexcept;

  InstrumentType type_;
  Param knobs_[kParams] = {}, adsr_[kParams] = {}, fxp_[kParams] = {}, lfop_[kParams] = {};
  FxType fx_type_ = FxType::Delay;
  LfoType lfo_type_ = LfoType::Tremolo;
  bool fx_on_ = false, lfo_on_ = false;
  int8_t octave_ = 0;
  // Effective values for this block: stored value + LFO offset, clamped to the int16 range.
  s32 eff_knob_[kParams] = {}, eff_adsr_[4] = {};
  ModOut mod_;

 private:
  struct Pending {
    int16_t offset;
    uint8_t note;
    bool on;
  };
  void apply(const Pending& e) noexcept;
  void push(int note, bool on, int offset) noexcept;

  Effect* fx_ = nullptr;
  Lfo* lfo_ = nullptr;
  Pending pending_[32] = {};
  int npending_ = 0;
  int note_ons_ = 0;
  s32 out_peak_ = 0, last_peak_ = 0;
  s32 gain_prev_ = 32768, gain_cur_ = 32768;
  s32 mono_[kBlock] = {};
  alignas(64) std::byte lfo_mem_[kLfoStorage];
  alignas(64) std::byte fx_mem_[kMonoFxStorage];
};

// Display slots every instrument fills (InstrumentView::display[0..15]); engines use 16..63.
//   [0..5]   note of synth voice 0..5 (-1 = idle)            (drums: see drum.cpp / dbox.cpp)
//   [6..11]  amplitude envelope of voice 0..5, Q15
//   [12]     play mode 0 poly, 1 mono, 2 legato, 3 unison
//   [13]     portamento active (0/1)
//   [14]     output peak of the last block, Q15
//   [15]     LFO pitch offset, semitones * 256
inline constexpr int kEngineDisplay = 16;

}  // namespace op1::dsp
