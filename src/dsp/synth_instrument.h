// The synth instrument (t4): 6 voices of one engine E, the amplitude ADSR, the four play modes and
// portamento (firmware-behaviour.md §3-§4, og-behaviour-details.md §4), hosted by InstrumentBase.
//
// An engine E provides (all noexcept, audio thread, no allocation):
//   static constexpr InstrumentType kType;
//   struct Voice { ... };                                   per-voice state
//   void control(const s32 k[8]);                           every 64 frames: effective knob values
//   void note_on(Voice& v, bool legato);                    legato = pitch change only, no re-attack
//   void render(Voice& v, u32 inc, s32* out, int n);        write n samples (Q31, about +-0.5..1 full scale)
//   void view(int32_t* d) const;                            display[16..63] (48 slots), documented per engine
// and optionally:
//   bool command(const Command& c);                         engine-specific ops (sampler: SetSample)
//   void view_full(InstrumentView& v) const;                fields beyond display[] (sampler, drwave)
//   void load_patch(const SynthPatch& p);                   engine-specific patch fields (sampler: base_freq)
//
// Play mode (adsr slot 4) in four 4096-wide bands: poly, mono, legato, unison. Portamento (adsr slot 5,
// 64..12864): 64 or less = off, otherwise a glide time constant of 10 ms .. 2 s. G-BEND (slot 6): pitch-bend
// range, value * 24 / 32767 semitones. DETUNE (slot 7): unison spread, x value / 4000 (4000 = 0/+-7/+-14/+21 ct).
#pragma once

#include <cstring>
#include <new>

#include "instrument_base.h"

namespace op1::dsp {

enum class PlayMode : uint8_t { Poly = 0, Mono = 1, Legato = 2, Unison = 3 };
constexpr PlayMode play_mode_of(s32 v) noexcept { return PlayMode(clampv<s32>(v, 0, 16383) >> 12); }

template <class E>
class SynthInstrument final : public InstrumentBase {
 public:
  SynthInstrument() noexcept : InstrumentBase(E::kType) {
    std::memcpy(knobs_, default_knobs(E::kType), sizeof knobs_);
    for (int k = 0; k < kParams; ++k) eff_knob_[k] = knobs_[k];
    engine_.control(eff_knob_);
    rates_ = adsr_rates(adsr_[0], adsr_[1], adsr_[2], adsr_[3]);
  }

 protected:
  void on_note(int note, bool on) noexcept override {
    if (mode_ == PlayMode::Poly) {
      on ? poly_on(note) : poly_off(note);
    } else {
      on ? mono_on(note) : mono_off(note);
    }
  }

  void on_all_off() noexcept override {
    nstack_ = 0;
    for (auto& v : v_) {
      v.gate = false;
      v.env.gate_off();
    }
  }

  void block_control(const ProcessContext& ctx) noexcept override {
    const PlayMode m = play_mode_of(adsr_[4]);
    if (m != mode_) {  // changing mode releases everything (stack and voice roles change meaning)
      on_all_off();
      mode_ = m;
    }
    glide_ = adsr_[5] > 64 ? g_glide[clampv<s32>(adsr_[5], 0, 16384) >> 6] : 0;
    rates_ = adsr_rates(eff_adsr_[0], eff_adsr_[1], eff_adsr_[2], eff_adsr_[3]);
    engine_.control(eff_knob_);
    // G-BEND (adsr slot 6) = pitch-bend range: value * 24 / 32767 semitones, truncated (the factory value 4000
    // gives 2, the range this port had fixed). DETUNE (slot 7) scales the unison spread: 4000, the factory value
    // of almost every preset, gives the 21 cents this port had fixed. Both ASSUMED (t2, 2026-10-23): the slots
    // are named on the playmode screen (g/gval, dt/dtval) and vary in 6 factory presets, their units are open.
    const s32 bend_st = clampv<s32>(s32(adsr_[6]) * 24 / 32767, 0, 24);
    const s32 bend = ctx.controls ? s32(ctx.controls->pitch_bend) * bend_st / 32 : 0;  // 8192 -> 256 * bend_st
    const s32 det_scale = clampv<s32>(adsr_[7], 0, 32767);
    const s32 shift = s32(octave_) * 3072 + bend + mod_.pitch_q8;
    for (int i = 0; i < kSynthVoices; ++i) {
      Voice& v = v_[i];
      if (!v.env.active()) continue;
      if (glide_ && v.pitch != v.target)
        v.pitch = s32(s64(v.pitch) + ((s64(v.target) - v.pitch) * glide_ >> 31) +
                      (v.target > v.pitch ? 1 : (v.target < v.pitch ? -1 : 0)));
      else
        v.pitch = v.target;
      const s32 det = mode_ == PlayMode::Unison ? kUnisonDetune[i] * det_scale / 4000 : 0;
      v.inc = note_inc(v.pitch + shift + det);
    }
  }

  void render(const ProcessContext&, s32* out, int n) noexcept override {
    for (int i = 0; i < n; ++i) acc_[i] = 0;
    const s32 vg = mode_ == PlayMode::Unison ? s32(s64(kVoiceGain) * 21 / 50) : kVoiceGain;  // 1/sqrt(6) in unison
    for (auto& v : v_) {
      if (!v.env.active()) continue;
      engine_.render(v.ev, v.inc, tmp_, n);
      for (int i = 0; i < n; ++i) {
        const s32 e = v.env.tick(rates_);
        acc_[i] += mulq(mulq(tmp_[i], e), vg);
      }
      if (!v.env.active()) v.note = -1;
    }
    // The voice sum through a soft knee instead of a hard saturating add (t2, 2026-10-23): unchanged up to
    // half scale, so one or a few notes sound as before, then tanh-shaped toward full scale, so a 6-note chord
    // no longer hard-clips (it did on digital, pulse, phase, voltage and iter: PARITY-AUDIT.md §4.1).
    for (int i = 0; i < n; ++i) out[i] = soft_knee(acc_[i]);
  }

  // Linear below 0.5 FS, then 0.5 + 0.5 tanh((|x| - 0.5) / 0.5): continuous with slope 1 at the knee, < 1 FS.
  static s32 soft_knee(s64 x) noexcept {
    constexpr s64 kKnee = s64(1) << 30;
    const s64 a = x < 0 ? -x : x;
    if (a <= kKnee) return s32(x);
    const s64 y = kKnee + (s64(soft_clip((a - kKnee) * 2)) >> 1);
    return s32(x < 0 ? -y : y);
  }

  s32 newest_envelope() const noexcept override {
    const Voice* best = nullptr;
    for (const auto& v : v_)
      if (v.env.active() && (!best || v.age > best->age)) best = &v;
    return best ? best->env.level : 0;
  }

  bool own_command(const Command& c) noexcept override {
    if constexpr (requires(E e, const Command& cc) { e.command(cc); }) return engine_.command(c);
    return false;
  }
  void load_synth_patch(const SynthPatch& p) noexcept override {
    on_all_off();
    if constexpr (requires(E e, const SynthPatch& pp) { e.load_patch(pp); }) engine_.load_patch(p);
  }

  void own_view(InstrumentView& out) const noexcept override {
    uint8_t mask = 0;
    for (int i = 0; i < kSynthVoices; ++i) {
      const bool on = v_[i].env.active();
      if (on) mask |= uint8_t(1u << i);
      out.display[i] = on ? v_[i].note : -1;
      out.display[6 + i] = v_[i].env.level >> 16;
    }
    out.voices_active = mask;
    out.display[12] = int32_t(mode_);
    out.display[13] = glide_ ? 1 : 0;
    engine_.view(out.display + kEngineDisplay);
    // engines that publish more than display[] (sampler: the sample; drwave: its cycle) implement view_full
    if constexpr (requires(const E e, InstrumentView& v) { e.view_full(v); }) engine_.view_full(out);
  }

 private:
  static constexpr s32 kVoiceGain = s32(0.32 * 2147483648.0);
  // Unison spread in Q8 semitones: 0, +-7, +-14, +21 cents.
  static constexpr s32 kUnisonDetune[kSynthVoices] = {0, 18, -18, 36, -36, 54};

  struct Voice {
    typename E::Voice ev{};
    Adsr env;
    int note = -1;
    s32 pitch = 60 * 256, target = 60 * 256;  // Q8 semitones (MIDI note * 256)
    u32 inc = 0;
    bool gate = false;
    u64 age = 0;
  };

  void start(Voice& v, int note, bool legato) noexcept {
    const bool was_active = v.env.active();
    v.note = note;
    v.target = note * 256;
    if (!(glide_ && (have_last_ || was_active))) v.pitch = v.target;
    else if (!was_active) v.pitch = last_pitch_;
    v.inc = note_inc(v.pitch + s32(octave_) * 3072 + mod_.pitch_q8);
    engine_.note_on(v.ev, legato && was_active);
    if (!(legato && was_active)) v.env.gate_on();
    v.gate = true;
    v.age = ++clock_;
    last_pitch_ = v.target;
    have_last_ = true;
  }

  void poly_on(int note) noexcept {
    Voice* pick = nullptr;
    for (auto& v : v_)  // the same note again: retrigger its voice
      if (v.gate && v.note == note) pick = &v;
    if (!pick)
      for (auto& v : v_)  // a silent voice
        if (!v.env.active() && (!pick || v.age < pick->age)) pick = &v;
    if (!pick)
      for (auto& v : v_)  // the oldest released voice
        if (!v.gate && (!pick || v.age < pick->age)) pick = &v;
    if (!pick)
      for (auto& v : v_)  // the oldest voice
        if (!pick || v.age < pick->age) pick = &v;
    start(*pick, note, false);
  }
  void poly_off(int note) noexcept {
    for (auto& v : v_)
      if (v.gate && v.note == note) {
        v.gate = false;
        v.env.gate_off();
      }
  }

  // ---- mono, legato, unison: last-note priority over a stack of held notes
  void stack_remove(int note) noexcept {
    int j = 0;
    for (int i = 0; i < nstack_; ++i)
      if (stack_[i] != note) stack_[j++] = stack_[i];
    nstack_ = j;
  }
  int voices_used() const noexcept { return mode_ == PlayMode::Unison ? kSynthVoices : 1; }
  void mono_on(int note) noexcept {
    const bool held = nstack_ > 0;
    stack_remove(note);
    if (nstack_ == int(sizeof stack_ / sizeof stack_[0])) {  // full: drop the oldest
      for (int i = 1; i < nstack_; ++i) stack_[i - 1] = stack_[i];
      --nstack_;
    }
    stack_[nstack_++] = uint8_t(note);
    const bool legato = mode_ == PlayMode::Legato && held;
    for (int i = 0; i < voices_used(); ++i) start(v_[i], note, legato);
  }
  void mono_off(int note) noexcept {
    const bool was_top = nstack_ > 0 && stack_[nstack_ - 1] == note;
    stack_remove(note);
    if (!was_top) return;
    if (nstack_ == 0) {
      for (int i = 0; i < voices_used(); ++i) {
        v_[i].gate = false;
        v_[i].env.gate_off();
      }
      return;
    }
    // back to the previous held note: legato glides there, mono and unison re-attack
    const int prev = stack_[nstack_ - 1];
    for (int i = 0; i < voices_used(); ++i) start(v_[i], prev, mode_ == PlayMode::Legato);
  }

  E engine_{};
  Voice v_[kSynthVoices];
  AdsrRates rates_;
  PlayMode mode_ = PlayMode::Poly;
  s32 glide_ = 0;
  s32 last_pitch_ = 60 * 256;
  bool have_last_ = false;
  u64 clock_ = 0;
  uint8_t stack_[16] = {};
  int nstack_ = 0;
  s32 tmp_[kBlock] = {};
  s64 acc_[kBlock] = {};
};

// Construct SynthInstrument<E> into `mem` (factory helper for eng_<name>.cpp).
template <class E>
Instrument* make_synth(void* mem, size_t size) noexcept {
  if (sizeof(SynthInstrument<E>) > size) return nullptr;
  return new (mem) SynthInstrument<E>();
}

}  // namespace op1::dsp
