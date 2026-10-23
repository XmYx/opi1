// voltage (id 13, Slump.cpp in the firmware): a drifting two-oscillator "analog" voice (saw + pulse) through
// a phaser and a low-pass, with ground noise.
//
// The firmware's DSP routine is hand-written Blackfin code that has not been decoded (og-behaviour-details.md
// §7), so the sound is designed by ear on the OG facts: defaults 1516/16704/0/15168, byte-for-byte those of
// string (a factory-data quirk kept as is), so the ground-noise curve is quadratic to keep the default's
// hum in the background.
//
// Knobs: blue (0) = modulation (slow drift of pitch and pulse width, up to +-35 cents / +-35 % width),
//        green (1) = ground noise (50 Hz mains buzz and hiss under the note, squared curve), white (2) = phase
//        filter (a 4-stage phaser swept slowly around 150 Hz .. 6 kHz, and a following low-pass at 3x that),
//        orange (3) = detune (oscillator 2 up to +45 cents).
// The slow modulation is computed every 8 samples.
//
// display[16..]: [16] drift value Q15 (signed), [17] phase-filter centre in Hz, [18] detune cents,
//                [19] ground noise level Q15, [20] pulse width Q15,
//                [24..39] the last 128 output samples of the most recently rendered voice, every 4th (32 values,
//                oldest first), int16 Q15 packed two per slot: low half = even index, high half = odd index.
//                The firmware's Slump DSP copies 32 samples for the screen at the end of its render (buffer
//                0x01DF6154, written at 0x01A49ED2..EEC; t3, 2026-10-23). Blocks without a sounding voice give 0.
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

constexpr int kControl = 8;

struct VoltageEngine {
  static constexpr InstrumentType kType = InstrumentType::Voltage;
  // display scope: two 64-frame blocks of the last rendered voice
  int16_t scope[2 * kBlock] = {};
  int scope_base = 0;          // start of the current block in `scope`
  uint32_t block_id = 0;       // advances at every control tick
  bool rendered = false;       // a voice rendered since the last control tick
  struct Voice {
    uint32_t blk = ~0u;        // block_id of this voice's last render, and its frame offset in that block
    int off = 0;
    u32 ph1 = 0, ph2 = u32(0.33 * 4294967296.0);
    u32 drift = u32(0.13 * 4294967296.0), drift2 = u32(0.61 * 4294967296.0), sweep = 0, hum = 0;
    u32 seed = 9090;
    s32 x1[4] = {}, y1[4] = {};  // phaser allpass states
    Svf lp;
    // values updated every kControl samples
    int count = 0;
    u32 dt1 = 0, dt2 = 0, pw = kHalfCycle;
    s32 a = 0;  // allpass coefficient Q31
    s32 d = 0;  // drift Q31
  };

  s32 mod = 0, noise = 0, det_cents = 0;
  u64 fc_q16 = 0, ratio2 = 65536;
  SvfCoef lp_coef;
  u32 drift_inc = 0, drift2_inc = 0, sweep_inc = 0, hum_inc = 0;
  s32 shown_d = 0, shown_pw = 16384;

  void control(const s32 k[kParams]) noexcept {
    // next scope block; a block in which nothing rendered reads as silence
    scope_base = (scope_base + kBlock) & (2 * kBlock - 1);
    if (!rendered)
      for (int i2 = 0; i2 < kBlock; ++i2) scope[(scope_base + kBlock + i2) & (2 * kBlock - 1)] = 0;
    rendered = false;
    ++block_id;
    static constexpr ExpCurve kFc(150.0, 6000.0);
    mod = u15(k[0]);
    noise = sq15(u15(k[1]));
    fc_q16 = kFc.at(k[2]);
    det_cents = s32((s64(u15(k[3])) * 45) >> 15);
    ratio2 = cents_ratio_q16(det_cents);
    lp_coef = svf_coef(hz_inc(fc_q16 * 3), svf_k_q(0.62));
    drift_inc = hz_inc(u64(0.37 * 65536)) * kControl;
    drift2_inc = hz_inc(u64(2.3 * 65536)) * kControl;
    sweep_inc = hz_inc(u64(0.21 * 65536)) * kControl;
    hum_inc = hz_inc(u64(50) << 16);
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (legato) return;
    v.ph1 = 0;
    v.ph2 = u32(0.33 * 4294967296.0);
    v.count = 0;
  }

  void update(Voice& v, u32 inc) noexcept {
    const s32 sa = sin_q31(v.drift), sb = sin_q31(v.drift2);
    v.drift += drift_inc;
    v.drift2 += drift2_inc;
    v.d = sat((s64(sa) * 7 + s64(sb) * 3) / 10);
    // pitch drift: d * mod * 35 cents (oscillator 2 drifts the other way)
    const s32 dm15 = s32((s64(v.d >> 16) * mod) >> 15);
    const u64 pm = cents_ratio_q16(s32((s64(dm15) * 35) >> 15));
    v.dt1 = inc_mul(inc, pm);
    v.dt2 = inc_mul(inc_mul(inc, ratio2), (u64(1) << 32) / (pm ? pm : 1));
    const s64 pw = s64(kHalfCycle) + ((s64(dm15) * s64(0.35 * 4294967296.0)) >> 15);
    v.pw = u32(clampv<s64>(pw, s64(0.1 * 4294967296.0), s64(0.9 * 4294967296.0)));
    // phaser centre sweeps +-50 % around the phase-filter frequency
    const s32 sw = sin_q31(v.sweep);
    v.sweep += sweep_inc;
    const u64 hz = u64(s64(fc_q16) + ((s64(fc_q16) * (sw >> 16)) >> 16));
    const s64 t = tan_q16(hz_inc(hz));
    v.a = s32(((t - 65536) << 31) / (t + 65536));
    shown_d = v.d >> 16;
    shown_pw = s32(v.pw >> 17);
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    const s32 gnd_gain = s32((s64(noise) * 13107) >> 15);  // 0.4
    for (int i = 0; i < n; ++i) {
      if (v.count == 0) update(v, inc);
      v.count = (v.count + 1) & (kControl - 1);
      const s32 s1 = saw_blep(v.ph1, v.dt1);
      const s32 s2 = pulse_blep(v.ph2, v.dt2, v.pw);
      v.ph1 += v.dt1;
      v.ph2 += v.dt2;
      // ground noise: clipped 50 Hz buzz + hiss
      const s32 hc = soft_clip(s64(sin_q31(v.hum)) * 4);
      v.hum += hum_inc;
      v.seed = v.seed * 1664525u + 1013904223u;
      const s32 gnd = mul15(sat((s64(hc) * 3 + s64(s32(v.seed)) * 2) / 5), gnd_gain);
      const s32 in = sat(((s64(s1) + s2) >> 2) + gnd);
      // phaser: four first-order allpasses with coefficient a
      s32 y = in;
      for (int k = 0; k < 4; ++k) {
        const s32 z = sat(((-s64(v.a) * y) >> 31) + v.x1[k] + ((s64(v.a) * v.y1[k]) >> 31));
        v.x1[k] = y;
        v.y1[k] = z;
        y = z;
      }
      const s32 l = v.lp.tick(lp_coef, sat(((s64(in) + y) * 22938) >> 15));  // x 0.7
      out[i] = soft_clip(s64(l) * 2);
    }
    if (v.blk != block_id) {
      v.blk = block_id;
      v.off = 0;
    }
    for (int i = 0; i < n && v.off + i < kBlock; ++i) scope[(scope_base + v.off + i) & (2 * kBlock - 1)] = int16_t(out[i] >> 16);
    v.off += n;
    rendered = true;
  }

  void view(int32_t* d) const noexcept {
    d[0] = shown_d;
    d[1] = int32_t(fc_q16 >> 16);
    d[2] = det_cents;
    d[3] = noise;
    d[4] = shown_pw;
    // oldest first: the previous block starts at scope_base + 64
    for (int j = 0; j < 16; ++j) {
      const int a = (scope_base + kBlock + 8 * j) & (2 * kBlock - 1), b = (a + 4) & (2 * kBlock - 1);
      d[8 + j] = int32_t(uint32_t(uint16_t(scope[a])) | (uint32_t(uint16_t(scope[b])) << 16));
    }
  }
};

}  // namespace

Instrument* make_voltage(void* mem, size_t size) noexcept { return make_synth<VoltageEngine>(mem, size); }

}  // namespace op1::dsp
