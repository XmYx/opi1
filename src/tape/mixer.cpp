// Mixer and master section (owner: t5): 4 tape tracks (mono) with volume and pan, the live signal, and
// the master effect (stereo factory, firmware-behaviour.md §5). Defaults are op1.db `master`: volumes
// 4 x 32767, pans 4 x 0, EQ 3 x 16383, master_params 32767/32767/8192/10240, fx punch with 8 x 4000, off.
// Gain law and EQ/master_params use: tape_engine.h (assumed; the firmware's are not located).
#include <cstring>
#include <new>

#include "op1/dsp.h"
#include "op1/fract.h"
#include "op1/tape.h"
#include "tape_engine.h"

namespace op1 {
namespace {

class MixerImpl final : public Mixer {
 public:
  MixerImpl() {
    fx_mem_ = static_cast<std::byte*>(::operator new(kEffectStorage, std::align_val_t(64)));
    std::memset(fx_mem_, 0, kEffectStorage);  // prefault
    fx_ = create_effect(fx_type_, true, fx_mem_, kEffectStorage);
    for (int i = 0; i < kParams; ++i) {
      fx_params_[i] = 4000;
      if (fx_) fx_->set_param(i, fx_params_[i]);
    }
  }
  ~MixerImpl() override {
    if (fx_) fx_->~Effect();
    ::operator delete(fx_mem_, std::align_val_t(64));
  }
  MixerImpl(const MixerImpl&) = delete;
  MixerImpl& operator=(const MixerImpl&) = delete;

  void command(const Command& c) noexcept override {
    const int s = c.slot & 3;
    const Param v = Param(c.value < -32768 ? -32768 : (c.value > 32767 ? 32767 : c.value));
    switch (MixerOp(c.op)) {
      case MixerOp::SetVolume: volume_[s] = v < 0 ? Param(0) : v; break;
      case MixerOp::SetPan: pan_[s] = v; break;
      case MixerOp::SetEq: eq_[c.index < 0 ? 0 : (c.index > 2 ? 2 : c.index)] = v; break;
      case MixerOp::SetMaster: master_[c.index & 3] = v; break;
      case MixerOp::SetMasterFx: switch_fx(c.value); break;
      case MixerOp::SetMasterFxOn:
        if (c.value && !fx_on_ && fx_) fx_->reset();
        fx_on_ = c.value != 0;
        break;
      case MixerOp::SetMasterFxParam:
        fx_params_[c.index & 7] = v;
        if (fx_) fx_->set_param(c.index & 7, v);
        break;
      case MixerOp::SetMetronome: set_metronome(c.value); break;
    }
  }

  void process(const ProcessContext& ctx, const Sample* const tracks[kTapeTracks], const Sample* live_l,
               const Sample* live_r, Sample* out_l, Sample* out_r) noexcept override {
    int32_t gl[kTapeTracks], gr[kTapeTracks];  // Q15 gains per track and side
    for (int t = 0; t < kTapeTracks; ++t) {
      const int32_t p = pan_[t];  // -32768 (left) .. 0 (centre) .. 32767 (right)
      int32_t bl = 32767, br = 32767;
      if (p > 0) bl = 32767 - p;
      if (p < 0) br = 32767 + p < 0 ? 0 : 32767 + p;
      gl[t] = (int32_t(volume_[t]) * bl) >> 15;
      gr[t] = (int32_t(volume_[t]) * br) >> 15;
    }
    for (int i = 0; i < ctx.frames; ++i) {
      int64_t l = live_l[i], r = live_r[i];
      for (int t = 0; t < kTapeTracks; ++t) {
        l += (int64_t(tracks[t][i]) * gl[t]) >> 15;
        r += (int64_t(tracks[t][i]) * gr[t]) >> 15;
      }
      out_l[i] = bf::sat32(l);
      out_r[i] = bf::sat32(r);
    }
    if (fx_on_ && fx_) fx_->process(ctx, out_l, out_r);
    if (met_level_ > 0) metronome(ctx, out_l, out_r);
    int32_t pl = 0, pr = 0;
    for (int i = 0; i < ctx.frames; ++i) {
      const int32_t a = bf::abs_fr1x32(out_l[i]), b = bf::abs_fr1x32(out_r[i]);
      if (a > pl) pl = a;
      if (b > pr) pr = b;
    }
    if (ctx.block_start) meter_l_ = meter_r_ = 0;
    if (int16_t(pl >> 16) > meter_l_) meter_l_ = int16_t(pl >> 16);
    if (int16_t(pr >> 16) > meter_r_) meter_r_ = int16_t(pr >> 16);
  }

  void view(MixerView& v) const noexcept override {
    std::memcpy(v.volume, volume_, sizeof volume_);
    std::memcpy(v.pan, pan_, sizeof pan_);
    std::memcpy(v.eq, eq_, sizeof eq_);
    std::memcpy(v.master, master_, sizeof master_);
    v.master_fx = fx_type_;
    v.master_fx_on = fx_on_;
    v.meter_l = meter_l_;
    v.meter_r = meter_r_;
    std::memcpy(v.master_fx_params, fx_params_, sizeof fx_params_);
    for (auto& d : v.master_fx_display) d = 0;
    if (fx_) fx_->view(v.master_fx_display);
  }

 private:
  // The metronome (metronome.cpp, t5 notes decode-arp-endless-tombola.md §6; F = seen, I = this port's reading).
  // F: from the level L (0..32768): start amplitude A0 = 8192 + L / 2 (Q15), a one-pole low-pass with coefficient
  // (163 + L x 655 / 32768) / 32768, a +-full-scale square wave with half-period 100 - 80 L / 32768 samples, an
  // envelope that decays x 0x7FF0 / 0x8000 per sample; the same click on every beat (no bar accent); added equally
  // to left and right. I: one click per quarter note at the tempo, the beat grid running freely (the firmware also
  // restarts it when the transport starts), not recorded to tape.
  void set_metronome(int32_t level) noexcept {
    met_level_ = level < 0 ? 0 : (level > 32768 ? 32768 : level);
    met_a0_ = 8192 + met_level_ / 2;
    met_a_ = 163 + int32_t((int64_t(met_level_) * 655) >> 15);
    met_half_ = 100 - int32_t((int64_t(met_level_) * 80) >> 15);
    if (met_half_ < 1) met_half_ = 1;
  }
  void metronome(const ProcessContext& ctx, Sample* l, Sample* r) noexcept {
    const int64_t beat = int64_t(kSampleRate) * 600 * 65536 / (ctx.bpm_tenths > 0 ? ctx.bpm_tenths : 1200);  // Q16 samples
    for (int i = 0; i < ctx.frames; ++i) {
      met_phase_ -= 65536;
      if (met_phase_ < 0) {  // a beat: retrigger
        met_phase_ += beat;
        met_env_ = met_a0_;
        met_count_ = 0;
        met_sq_ = 0x7FFF;
      }
      if (met_env_ <= 0) continue;
      if (++met_count_ >= met_half_) { met_count_ = 0; met_sq_ = -met_sq_; }
      met_y_ += ((met_sq_ - met_y_) * met_a_) >> 15;  // y = y (1 - a) + x a, Q15
      met_env_ = (met_env_ * 0x7FF0) >> 15;
      const int64_t click = (int64_t(met_y_) * met_env_) << 1;  // Q15 x Q15 -> Q31
      l[i] = bf::sat32(int64_t(l[i]) + click);
      r[i] = bf::sat32(int64_t(r[i]) + click);
    }
  }

  // Audio thread: rebuild the effect in the same storage (placement, no allocation). The new type starts from
  // its fx_types defaults, as the instrument effect does (instrument_base.cpp set_fx_type): before t5 the master
  // kept the previous effect's values (PARITY-AUDIT.md §4.2 gap 7, e.g. cwo got punch's turned values).
  void switch_fx(int32_t value) noexcept {
    if (value < 0 || value >= int32_t(FxType::Count) || FxType(value) == fx_type_) return;
    const FxType old = fx_type_;
    if (fx_) fx_->~Effect();
    fx_ = create_effect(FxType(value), true, fx_mem_, kEffectStorage);
    fx_type_ = FxType(value);
    if (!fx_) {
      fx_ = create_effect(old, true, fx_mem_, kEffectStorage);
      fx_type_ = old;
    } else {
      std::memcpy(fx_params_, fx_type_defaults(fx_type_), sizeof fx_params_);
    }
    if (fx_)
      for (int i = 0; i < kParams; ++i) fx_->set_param(i, fx_params_[i]);
  }

  Param volume_[kTapeTracks] = {32767, 32767, 32767, 32767};
  Param pan_[kTapeTracks] = {};
  Param eq_[3] = {16383, 16383, 16383};
  Param master_[4] = {32767, 32767, 8192, 10240};
  Param fx_params_[kParams] = {};
  FxType fx_type_ = FxType::Punch;
  bool fx_on_ = false;
  std::byte* fx_mem_ = nullptr;
  Effect* fx_ = nullptr;
  int16_t meter_l_ = 0, meter_r_ = 0;
  int32_t met_level_ = 0, met_a0_ = 8192, met_a_ = 163, met_half_ = 100, met_count_ = 0, met_sq_ = 0x7FFF;
  int32_t met_env_ = 0, met_y_ = 0;
  int64_t met_phase_ = 0;
};

}  // namespace

std::unique_ptr<Mixer> create_mixer() { return std::make_unique<MixerImpl>(); }

}  // namespace op1
