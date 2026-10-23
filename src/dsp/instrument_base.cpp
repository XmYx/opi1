// InstrumentBase: see instrument_base.h.
#include "instrument_base.h"

#include <new>

namespace op1::dsp {

AdsrRates adsr_rates(s32 a, s32 d, s32 s, s32 r) noexcept {
  auto idx = [](s32 v, s32& f) {
    v = clampv<s32>(v, 0, 16384);
    f = v & 63;
    return v >> 6;
  };
  AdsrRates out;
  s32 f;
  int i = idx(a, f);
  out.attack = u32(s64(g_env_attack[i]) + ((s64(g_env_attack[i + 1]) - s64(g_env_attack[i])) * f >> 6));
  if (out.attack < 1) out.attack = 1;
  i = idx(d, f);
  out.decay = g_env_coef[i] + s32((s64(g_env_coef[i + 1]) - g_env_coef[i]) * f >> 6);
  i = idx(r, f);
  out.release = g_env_coef[i] + s32((s64(g_env_coef[i + 1]) - g_env_coef[i]) * f >> 6);
  out.sustain = q15to31(u15(s));
  return out;
}

InstrumentBase::InstrumentBase(InstrumentType t) noexcept : type_(t) {
  std::memcpy(adsr_, kDefaultAdsr, sizeof adsr_);
  set_fx_type(FxType::Delay, true);
  set_lfo_type(LfoType::Tremolo, true);
}

InstrumentBase::~InstrumentBase() {
  if (fx_) fx_->~Effect();
  if (lfo_) lfo_->~Lfo();
}

void InstrumentBase::set_fx_type(FxType t, bool load_defaults) noexcept {
  if (t >= FxType::Count) return;
  if (fx_) fx_->~Effect();
  fx_type_ = t;
  fx_ = create_effect(t, false, fx_mem_, sizeof fx_mem_);
  if (load_defaults) std::memcpy(fxp_, default_fx_params(t), sizeof fxp_);
  if (fx_)
    for (int i = 0; i < kParams; ++i) fx_->set_param(i, fxp_[i]);
}

void InstrumentBase::set_lfo_type(LfoType t, bool load_defaults) noexcept {
  if (t >= LfoType::Count) return;
  if (lfo_) lfo_->~Lfo();
  lfo_type_ = t;
  lfo_ = create_lfo(t, lfo_mem_, sizeof lfo_mem_);
  if (load_defaults) std::memcpy(lfop_, default_lfo_params(t), sizeof lfop_);
}

s32 InstrumentBase::bend_q8(const ProcessContext& ctx) noexcept {
  return ctx.controls ? s32(ctx.controls->pitch_bend) / 16 : 0;  // 8192 -> 512 = 2 semitones
}

void InstrumentBase::push(int note, bool on, int offset) noexcept {
  if (offset <= 0) {
    apply(Pending{0, uint8_t(note & 127), on});
    return;
  }
  if (npending_ >= int(sizeof pending_ / sizeof pending_[0])) {  // full: play it now rather than drop it
    apply(Pending{0, uint8_t(note & 127), on});
    return;
  }
  // keep the queue ordered by offset (stable: same offset keeps arrival order)
  int i = npending_++;
  while (i > 0 && pending_[i - 1].offset > offset) {
    pending_[i] = pending_[i - 1];
    --i;
  }
  pending_[i] = Pending{int16_t(offset), uint8_t(note & 127), on};
}

void InstrumentBase::apply(const Pending& e) noexcept {
  if (e.on) ++note_ons_;
  on_note(e.note, e.on);
}

void InstrumentBase::note_on(int note, int, int offset) noexcept {
  // The OG keyboard has no velocity (controller mode sends a fixed 100, og-behaviour-details.md §2.1):
  // notes play at one level whatever the MIDI velocity.
  push(note, true, offset);
}
void InstrumentBase::note_off(int note, int offset) noexcept { push(note, false, offset); }
void InstrumentBase::all_notes_off() noexcept {
  npending_ = 0;
  on_all_off();
}

void InstrumentBase::command(const Command& c) noexcept {
  if (own_command(c)) return;
  const int i = c.index & 7;
  const Param v = Param(clampv<s32>(c.value, -32768, 32767));
  switch (InstrumentOp(c.op)) {
    case InstrumentOp::SetKnob: knobs_[i] = v; break;
    case InstrumentOp::SetAdsr: adsr_[i] = v; break;
    case InstrumentOp::SetFxParam:
      fxp_[i] = v;
      break;  // pushed to the effect at the next control tick
    case InstrumentOp::SetLfoParam: lfop_[i] = v; break;
    case InstrumentOp::SetFxType: set_fx_type(FxType(c.value), true); break;
    case InstrumentOp::SetLfoType: set_lfo_type(LfoType(c.value), true); break;
    case InstrumentOp::SetFxOn:
      if (c.value && !fx_on_ && fx_) fx_->reset();
      fx_on_ = c.value != 0;
      break;
    case InstrumentOp::SetLfoOn:
      if (c.value && !lfo_on_ && lfo_) lfo_->reset();
      lfo_on_ = c.value != 0;
      break;
    case InstrumentOp::SetOctave: octave_ = int8_t(clampv<s32>(c.value, -4, 4)); break;
    case InstrumentOp::LoadSynthPatch:
      if (c.ptr && !is_drum(type_)) {
        const auto& p = *static_cast<const SynthPatch*>(c.ptr);
        std::memcpy(knobs_, p.knobs, sizeof knobs_);
        std::memcpy(adsr_, p.adsr, sizeof adsr_);
        octave_ = int8_t(clampv<int>(p.octave, -4, 4));
        set_fx_type(p.fx_type, false);
        std::memcpy(fxp_, p.fx_params, sizeof fxp_);
        fx_on_ = p.fx_active;
        if (fx_) fx_->reset();
        set_lfo_type(p.lfo_type, false);
        std::memcpy(lfop_, p.lfo_params, sizeof lfop_);
        lfo_on_ = p.lfo_active;
        load_synth_patch(p);
      }
      break;
    case InstrumentOp::LoadDrumPatch:
      if (c.ptr && is_drum(type_)) {
        const auto& p = *static_cast<const DrumPatch*>(c.ptr);
        octave_ = int8_t(clampv<int>(p.octave, -4, 4));
        set_fx_type(p.fx_type, false);
        std::memcpy(fxp_, p.fx_params, sizeof fxp_);
        fx_on_ = p.fx_active;
        if (fx_) fx_->reset();
        set_lfo_type(p.lfo_type, false);
        std::memcpy(lfop_, p.lfo_params, sizeof lfop_);
        lfo_on_ = p.lfo_active;
        load_drum_patch(p);
      }
      break;
    default: break;  // SetType is the core's; SetSample/SetDrumKey are handled by own_command
  }
}

void InstrumentBase::process(const ProcessContext& ctx, Sample* l, Sample* r) noexcept {
  const int n = ctx.frames < 0 ? 0 : (ctx.frames > kBlock ? kBlock : ctx.frames);
  if (ctx.block_start) {
    // ---- control tick (every 64 frames): LFO, effective parameters, effect parameters
    mod_ = ModOut{};
    if (lfo_on_ && lfo_) {
      LfoInputs in;
      in.bpm_tenths = ctx.bpm_tenths;
      in.note_ons = note_ons_;
      in.envelope = newest_envelope();
      in.out_level = last_peak_;
      in.controls = ctx.controls;
      lfo_->process(lfop_, in, mod_);
    }
    note_ons_ = 0;
    for (int k = 0; k < kParams; ++k) eff_knob_[k] = clampv<s32>(s32(knobs_[k]) + mod_.knob[k], -32768, 32767);
    for (int k = 0; k < 4; ++k) eff_adsr_[k] = clampv<s32>(s32(adsr_[k]) + mod_.adsr[k], 0, 32767);
    if (fx_)
      for (int k = 0; k < kParams; ++k)
        fx_->set_param(k, Param(clampv<s32>(s32(fxp_[k]) + (k < 4 ? mod_.fx[k] : 0), -32768, 32767)));
    gain_prev_ = gain_cur_;
    gain_cur_ = clampv<s32>(mod_.volume15, 0, 65536);
    block_control(ctx);
  }

  // ---- render, split at the note offsets inside this chunk
  int pos = 0, k = 0;
  while (k < npending_ && pending_[k].offset < n) {
    const int at = pending_[k].offset;
    if (at > pos) {
      render(ctx, mono_ + pos, at - pos);
      pos = at;
    }
    apply(pending_[k]);
    ++k;
  }
  if (pos < n) render(ctx, mono_ + pos, n - pos);
  // offsets beyond this chunk (should not happen: the core clamps them) move to the next chunk's start
  for (int j = k; j < npending_; ++j) {
    pending_[j - k] = pending_[j];
    pending_[j - k].offset = 0;
  }
  npending_ -= k;

  // ---- LFO volume, ramped over the 64-frame block
  if (gain_prev_ != 32768 || gain_cur_ != 32768) {
    const int base = int(ctx.frame % kBlock);
    for (int i = 0; i < n; ++i) {
      const s32 g = gain_prev_ + s32(s64(gain_cur_ - gain_prev_) * (base + i) / kBlock);
      mono_[i] = mul15(mono_[i], g);
    }
  }
  // ---- the instrument's own (mono) effect
  if (fx_on_ && fx_) fx_->process(ctx, mono_, nullptr);

  s32 peak = out_peak_;
  for (int i = 0; i < n; ++i) {
    const s32 a = mono_[i] == INT32_MIN ? kOne : (mono_[i] < 0 ? -mono_[i] : mono_[i]);
    if (a > peak) peak = a;
    l[i] = mono_[i];
    r[i] = mono_[i];
  }
  out_peak_ = peak;
  if ((ctx.frame + n) % kBlock == 0) {
    last_peak_ = out_peak_;
    out_peak_ = 0;
  }
}

void InstrumentBase::view(InstrumentView& v) const noexcept {
  v.type = type_;
  std::memcpy(v.knobs, knobs_, sizeof knobs_);
  std::memcpy(v.adsr, adsr_, sizeof adsr_);
  std::memcpy(v.fx, fxp_, sizeof fxp_);
  std::memcpy(v.lfo, lfop_, sizeof lfop_);
  v.fx_type = fx_type_;
  v.lfo_type = lfo_type_;
  v.fx_on = fx_on_;
  v.lfo_on = lfo_on_;
  v.octave = octave_;
  for (auto& d : v.display) d = 0;
  for (auto& d : v.fx_display) d = 0;
  for (auto& d : v.lfo_display) d = 0;
  v.display[14] = last_peak_ >> 16;
  v.display[15] = mod_.pitch_q8;
  if (fx_) fx_->view(v.fx_display);
  if (lfo_) lfo_->view(v.lfo_display);
  own_view(v);
}

}  // namespace op1::dsp
