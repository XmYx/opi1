// dbox (id 9, dbox.cpp): the drum synthesizer. Every one of the 24 keys is its own sound with eight values
// (drum patches carry `dbox_data`, 24 keys x 8 signed values; og-behaviour-details.md §4). The firmware's
// synthesis routine is not decoded, so the voice is designed by ear: two oscillators that morph sine ->
// triangle -> saw -> square -> noise, each with its own decay, a cross modulation (pitch sweep, then FM,
// then ring) and a low-pass / high-pass filter. The db type default 0,0,2917,974,7167,0,2592,4607 is the
// default key sound (a short kick); without a patch the instrument plays a 24-key kit built here from it.
//
// Key values. t5 (2026-10-23) from the firmware (dbox render 0x01970EE8 and the range table run in the bfin
// simulator, t5 notes decode-drums.md; F = seen, I = this port's reading):
//   0 pitch 1: F signed, -24320..24832, 0 = centre, 194 per encoder detent (before t5 the port read it through
//     u15(), so every negative value, 128 of the 240 factory values, collapsed to the lowest pitch). I: 6208 raw =
//     one octave (the factory drum pitch 6208 = 32 detents, and the chromatic bass kits "sub club" / "vox bass"
//     climb ~6100 raw per 12 keys), centre = 261.63 Hz, so -24320..24832 = 17 Hz .. 4.3 kHz.
//   1 wave 1 (F 0..32767; the shapes are not decoded: I a morph sine -> triangle -> saw -> square -> noise),
//   2 envelope 1: F x = value * 136 / 32768, index = 96 - |96 - x|: the decay grows with the knob up to x = 96
//     (value ~23130) and shortens again above; I: above that the knob adds an attack (DECAY ... ATTACK labels).
//   3 MODE: F 8 discrete modes, value >> 12 (0x01971138). I: what each does (the fraction inside the mode sets
//     its amount): 0..2 pitch sweep of osc 1 (0..16, 16..32, 32..48 semitones), 3..5 FM of osc 1 by osc 2
//     (amount 0..1, 1..2, 2..3), 6 ring modulation, 7 ring plus a pitch sweep.
//   4 pitch 2 (F 0..32767 unsigned; I 30 Hz .. 8 kHz exponential), 5 wave 2, 6 envelope 2 (F index =
//     max(94 - |94 - x|, 0)), 7 filter (F value >> 8 indexes the cutoff table; I a high-pass, 0 = open, up to
//     4 kHz: see setup()).
// Hits are one-shot. Keys map as (note - 53) mod 24. Engine knobs 0..7 are kit-wide offsets added to every key's
// value (0 = none), for LFOs and MIDI; the UI edits the last played key (as the firmware does) instead.
// SetDrumKey: slot 8..15 = key value 0..7, index = key 0..23, value = the stored value.
// SetAdsr sets dyna_env (stored and shown on the envelope page; its effect on dbox is not decoded: none here).
//
// display: [0..7] key of voice 0..7 (-1 idle), [16..39] level of key 0..23 (Q15), [40] last key played,
// [41] keys from a patch (1) or the built-in kit (0), t5: [42..49] the 8 values of the key the encoders edit
// (the last played, 0 before any), [50] that key, [51] its MODE 0..7.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>

#include "instrument_base.h"

namespace op1::dsp {
namespace {

constexpr int kDboxVoices = 8;

// The built-in kit (this port's own sounds, derived from the db type default): kicks, snares, hats, toms,
// zaps and blips, four of each.
constexpr Param kKit[kDrumKeys][kParams] = {
    {0, 0, 2917, 974, 7167, 0, 2592, 4607},       {1500, 0, 3317, 1574, 7167, 0, 2592, 4607},
    {3000, 0, 3717, 2174, 7167, 0, 2592, 4607},    {4500, 0, 4117, 2774, 7167, 0, 2592, 4607},
    {9000, 8192, 3500, 15000, 18000, 32767, 6500, 20000},  {9800, 8192, 3500, 15000, 18000, 32767, 7000, 21000},
    {10600, 8192, 3200, 15500, 19000, 32767, 7500, 22000}, {11400, 8192, 3000, 16000, 20000, 32767, 8000, 23000},
    {20000, 32767, 1500, 0, 26000, 32767, 1800, 29000},    {20000, 32767, 2400, 0, 26000, 32767, 3000, 29900},
    {20000, 32767, 3300, 0, 26000, 32767, 4200, 30800},    {20000, 32767, 6000, 0, 26000, 32767, 9000, 31700},
    {8000, 0, 7000, 4000, 12000, 8192, 3000, 9000},        {9600, 0, 7000, 4000, 13600, 8192, 3000, 9000},
    {11200, 0, 7000, 4000, 15200, 8192, 3000, 9000},       {12800, 0, 7000, 4000, 16800, 8192, 3000, 9000},
    {14000, 16384, 4000, 13000, 10000, 0, 2000, 16384},    {16000, 16384, 4000, 13000, 12000, 0, 2000, 16384},
    {18000, 16384, 4000, 18000, 14000, 0, 2000, 16384},    {20000, 16384, 4000, 20000, 16000, 0, 2000, 16384},
    {18000, 0, 1500, 26000, 22000, 24576, 1500, 16384},    {19500, 0, 1500, 27000, 23000, 24576, 1500, 16384},
    {21000, 0, 1500, 28000, 24000, 24576, 1500, 16384},    {22500, 0, 1500, 30000, 25000, 24576, 1500, 16384},
};

// Per-sample decay coefficient (to -60 dB) for a u15 envelope value: 5 ms .. 2.5 s.
s32 decay_coef(s32 v) noexcept {
  static constexpr ExpCurve kDecay(0.005, 2.5);
  constexpr double kK = 6.907755 * 140737488355328.0 / kSampleRate;  // 6.9 * 2^47 / fs
  const u64 t = kDecay.at(v);
  const s64 x = s64(u64(kK) / (t ? t : 1));      // Q31
  return s32(x - ((x * x) >> 32));                // 1 - e^-x ~ x - x^2/2
}

class Dbox final : public InstrumentBase {
 public:
  Dbox() noexcept : InstrumentBase(InstrumentType::Dbox) {
    for (int k = 0; k < kDrumKeys; ++k)
      for (int p = 0; p < kParams; ++p) keys_[k][p] = kKit[k][p];
    // the built-in kit was authored for word 0 on a 30 Hz .. 8 kHz curve: the same pitches on the signed scale
    for (int k = 0; k < kDrumKeys; ++k) {
      const double hz = 30.0 * std::pow(8000.0 / 30.0, double(kKit[k][0]) / 32767.0);
      keys_[k][0] = Param(clampv<s32>(s32(std::lround(6208.0 * std::log2(hz / 261.63))), -24320, 24832));
    }
    dyna_[1] = 8192;
  }

 protected:
  void on_note(int note, bool on) noexcept override {
    if (!on) return;  // one-shot: the hit plays out
    const int key = ((note - 53) % kDrumKeys + kDrumKeys) % kDrumKeys;
    V* pick = nullptr;
    for (auto& v : v_)
      if (v.on && v.key == key) pick = &v;
    if (!pick)
      for (auto& v : v_)
        if (!v.on && (!pick || v.age < pick->age)) pick = &v;
    if (!pick)
      for (auto& v : v_)
        if (!pick || v.age < pick->age) pick = &v;
    V& v = *pick;
    v = V{};
    v.on = true;
    v.key = key;
    v.age = ++clock_;
    v.sw = kOne;
    v.rng = Rng(u32(4242 + key * 977));
    setup(v);
    last_key_ = key;
  }
  void on_all_off() noexcept override {}  // hits always play out

  void block_control(const ProcessContext& ctx) noexcept override {
    shift_q8_ = bend_q8(ctx) + mod_.pitch_q8;
    for (auto& v : v_)
      if (v.on) setup(v);
  }

  void render(const ProcessContext&, s32* out, int n) noexcept override {
    for (int i = 0; i < n; ++i) out[i] = 0;
    for (auto& v : v_) {
      if (!v.on) continue;
      for (int i = 0; i < n; ++i) {
        const s32 raw2 = osc(v, v.ph2, v.inc2, v.w2, 0);
        v.ph2 += v.inc2;
        u32 inc1 = v.inc1;
        if (v.sweep_q8) {
          const s64 oct_q16 = (((s64(v.sweep_q8) * v.sw) >> 31) << 16) / 3072;
          inc1 = inc_mul(v.inc1, pow2_q16(oct_q16));
        }
        const u32 pm = v.fm_q16 ? pm_offset((s64(raw2) * v.e2) >> 31, v.fm_q16 / 2) : 0;
        s32 s1 = osc(v, v.ph1 + pm, inc1, v.w1, 1);
        v.ph1 += inc1;
        if (v.ring15) s1 = lerp15(s1, mulq(s1, raw2), v.ring15);
        s64 x = (s64(s1) * v.e1 >> 31) + ((s64(mulq(raw2, v.e2)) * (32768 - (v.ring15 * 7 / 10))) >> 15);
        // attack (envelope words above the decay peak): rise first, then decay
        if (v.att1) {
          v.e1 = v.e1 > kOne - v.a1 ? kOne : v.e1 + v.a1;
          if (v.e1 == kOne) v.att1 = false;
        }
        if (v.att2) {
          v.e2 = v.e2 > kOne - v.a2 ? kOne : v.e2 + v.a2;
          if (v.e2 == kOne) v.att2 = false;
        }
        if (v.fmode) {
          v.flt.tick(v.fc, s32(clampv<s64>(x, INT32_MIN, INT32_MAX)));
          x = v.fmode == 1 ? v.flt.lp : v.flt.hp;
        }
        s32 y = soft_clip(x * 4 / 5);
        y = mulq(y, s32(0.75 * 2147483648.0));
        if (v.age_s < 35) y = s32(s64(y) * v.age_s / 35);  // 0.8 ms de-click ramp
        ++v.age_s;
        out[i] = add(out[i], y);
        if (!v.att1) v.e1 -= s32((s64(v.e1) * v.k1) >> 31);
        if (!v.att2) v.e2 -= s32((s64(v.e2) * v.k2) >> 31);
        v.sw -= s32((s64(v.sw) * v.ksw) >> 31);
      }
      if (!v.att1 && !v.att2 && v.e1 < 2147483 && v.e2 < 2147483) v.on = false;  // both below -60 dB
    }
  }

  s32 newest_envelope() const noexcept override {
    const V* best = nullptr;
    for (const auto& v : v_)
      if (v.on && (!best || v.age > best->age)) best = &v;
    return best ? (best->e1 > best->e2 ? best->e1 : best->e2) : 0;
  }

  bool own_command(const Command& c) noexcept override {
    if (InstrumentOp(c.op) == InstrumentOp::SetAdsr) {
      dyna_[c.index & 7] = Param(clampv<s32>(c.value, -32768, 32767));
      return true;
    }
    if (InstrumentOp(c.op) != InstrumentOp::SetDrumKey) return false;
    if (c.index >= 0 && c.index < kDrumKeys && c.slot >= 8 && c.slot < 16)
      keys_[c.index][c.slot - 8] = Param(clampv<s32>(c.value, -32768, 32767));
    from_patch_ = true;
    return true;
  }

  void load_drum_patch(const DrumPatch& p) noexcept override {
    for (int i = 0; i < kParams; ++i) dyna_[i] = p.dyna_env[i];
    if (p.dbox_keys == 0) return;  // no dbox data: keep the current kit
    for (int k = 0; k < kDrumKeys && k < p.dbox_keys; ++k)
      for (int i = 0; i < kParams; ++i) keys_[k][i] = p.dbox_params[k][i];
    from_patch_ = true;
  }

  void own_view(InstrumentView& out) const noexcept override {
    uint8_t mask = 0;
    for (int i = 0; i < kDboxVoices; ++i) {
      out.display[i] = v_[i].on ? v_[i].key : -1;
      if (v_[i].on) {
        mask |= uint8_t(1u << i);
        const s32 lvl = (v_[i].e1 > v_[i].e2 ? v_[i].e1 : v_[i].e2) >> 16;
        if (lvl > out.display[16 + v_[i].key]) out.display[16 + v_[i].key] = lvl;
      }
    }
    out.voices_active = mask;
    out.display[40] = last_key_;
    out.display[41] = from_patch_ ? 1 : 0;
    const int e = last_key_ < 0 ? 0 : last_key_;
    for (int i = 0; i < kParams; ++i) out.display[42 + i] = keys_[e][i];
    out.display[50] = e;
    out.display[51] = (keys_[e][3] < 0 ? 0 : keys_[e][3]) >> 12;
    for (int i = 0; i < kParams; ++i) out.adsr[i] = dyna_[i];
  }

 private:
  struct V {
    bool on = false;
    int key = 0;
    u64 age = 0;
    u32 age_s = 0;
    u32 ph1 = 0, ph2 = 0, inc1 = 0, inc2 = 0;
    s32 e1 = 0, e2 = 0, sw = 0, k1 = 0, k2 = 0, ksw = 0, a1 = 0, a2 = 0;
    bool att1 = false, att2 = false, started = false;
    s32 w1 = 0, w2 = 0, sweep_q8 = 0, ring15 = 0;
    s64 fm_q16 = 0;
    uint8_t fmode = 0;  // 0 open, 1 low-pass, 2 high-pass
    SvfCoef fc;
    Svf flt;
    Rng rng;
    s32 noise1 = 0, noise2 = 0;
  };

  // The key's eight values plus the kit-wide offsets, then the voice's derived controls.
  void setup(V& v) noexcept {
    static constexpr ExpCurve kPitch(30.0, 8000.0);
    s32 p[kParams];
    for (int i = 0; i < kParams; ++i) p[i] = u15(s32(keys_[v.key][i]) + eff_knob_[i]);
    // word 0: signed pitch around 261.63 Hz, 6208 raw per octave (see the header)
    const s32 p0 = clampv<s32>(s32(keys_[v.key][0]) + eff_knob_[0], -24320, 24832);
    const u64 r = semis_ratio_q16(shift_q8_);
    v.inc1 = inc_mul(inc_mul(hz_inc(u64(261.63 * 65536.0)), semis_ratio_q16(s32(s64(p0) * 3072 / 6208))), r);
    v.inc2 = inc_mul(kPitch.inc(p[4]), r);
    v.w1 = p[1];
    v.w2 = p[5];
    // envelope words: the decay index rises to a peak (x = 96 / 94 of 136) and falls again; above the peak the
    // knob adds an attack, up to 250 ms at the top (see the header)
    auto env = [](s32 w, int peak, s32& k, s32& a) {
      const s32 x = s32(s64(w) * 136 / 32768);
      const s32 idx = std::max<s32>(peak - std::abs(peak - x), 0);
      k = decay_coef(s32(s64(idx) * 32767 / peak));
      const s32 over = x - peak;  // 0 .. 40
      a = over > 0 ? s32(s64(kOne) / std::max<s64>(1, s64(over) * 11025 / 40)) : 0;  // per-sample rise
    };
    env(p[2], 96, v.k1, v.a1);
    env(p[6], 94, v.k2, v.a2);
    if (!v.started) {
      v.started = true;
      v.att1 = v.a1 > 0;
      v.att2 = v.a2 > 0;
      v.e1 = v.att1 ? 0 : kOne;
      v.e2 = v.att2 ? 0 : kOne;
    }
    // sweep envelope: 12 .. 150 ms, following envelope 1
    const s32 kq = decay_coef(p[2] / 6);
    v.ksw = kq > (1 << 22) ? kq : (1 << 22);
    // MODE: 8 discrete modes (word 3 >> 12), the fraction inside the mode is its amount (see the header)
    const int mode = p[3] >> 12;
    const s64 mf = p[3] & 4095;  // 0..4095
    v.sweep_q8 = 0;
    v.fm_q16 = 0;
    v.ring15 = 0;
    if (mode <= 2) v.sweep_q8 = s32((s64(mode) * 16 * 256) + mf * 16 * 256 / 4096);
    else if (mode <= 5) v.fm_q16 = (s64(mode - 3) << 16) + (mf << 16) / 4096;
    else if (mode == 6) v.ring15 = s32(mf * 32767 / 4095);
    else {
      v.ring15 = 32767;
      v.sweep_q8 = s32(mf * 24 * 256 / 4096);
    }
    // word 7: the filter, index value >> 8 (0..127) into the cutoff table (F). I: a high-pass, 0 = open: the
    // factory kits give their low keys (kicks) low values and their high keys (hats) high values, and the bass
    // kits "sub club" / "vox bass" store 0 on every key (a low-pass there would mute them). Cutoff 20 Hz .. 4 kHz.
    const int fi = int(clampv<s32>(keys_[v.key][7] + eff_knob_[7], 0, 32767) >> 8);
    if (fi == 0) {
      v.fmode = 0;
    } else {
      const double hz = 20.0 * std::pow(200.0, double(fi) / 127.0);
      v.fmode = 2;
      v.fc = svf_coef(hz_inc(u64(hz * 65536.0)), svf_k_q(0.7));
    }
  }

  // Morphing oscillator: sine -> triangle -> saw -> square -> noise (w = u15).
  static s32 shape(V& v, int k, u32 ph, u32 inc, int which) noexcept {
    switch (k) {
      case 0: return sin_q31(ph);
      case 1: return tri_q31(ph);
      case 2: return saw_blep(ph, inc);
      case 3: return pulse_blep(ph, inc, kHalfCycle);
      default: {
        s32& n = which ? v.noise1 : v.noise2;
        n = v.rng.next();
        return n;
      }
    }
  }
  static s32 osc(V& v, u32 ph, u32 inc, s32 w, int which) noexcept {
    const s32 x = u15(w) * 4;
    const int i = x >> 15 > 3 ? 3 : x >> 15;
    const s32 f = x - (i << 15);
    const s32 a = shape(v, i, ph, inc, which);
    if (f <= 0) return a;
    return lerp15(a, shape(v, i + 1, ph, inc, which), f);
  }

  Param keys_[kDrumKeys][kParams] = {};
  Param dyna_[kParams] = {};
  V v_[kDboxVoices];
  s32 shift_q8_ = 0;
  u64 clock_ = 0;
  int last_key_ = -1;
  bool from_patch_ = false;
};

}  // namespace

Instrument* make_dbox(void* mem, size_t size) noexcept {
  return sizeof(Dbox) <= size ? new (mem) Dbox() : nullptr;
}

}  // namespace op1::dsp
