// dsynth (id 12, dsynth.cpp in the firmware): two morphing oscillators, each with its own envelope, cross
// modulation and a low-pass. The only engine that uses all eight parameter slots.
//
// The firmware's DSP routine is hand-written Blackfin code that has not been decoded (og-behaviour-details.md
// §7), so the sound is designed by ear on the OG facts: defaults 20664/0/8528/6225/328/0/12130/11465.
//
// Knobs: blue (0) = envelope crossfader (osc 1 / osc 2 balance, which slides toward osc 2 as osc 1's
//        envelope falls), green (1) = osc 1 waveform (sine > triangle > saw > square > stepped noise,
//        morphing between neighbours), white (2) = osc 1 envelope (lower half: decay 40 ms .. 8 s; upper half:
//        attack 1 ms .. 2 s, then hold), orange (3) = cross modulation (osc 1 -> osc 2 phase modulation, ring
//        modulation on top above 60 %).
// Shift page: blue (4) = osc 2 pitch offset 0 .. +36 semitones (the default 328 is a slight detune),
//        green (5) = osc 2 waveform, white (6) = osc 2 envelope, orange (7) = low-pass cutoff 120 Hz .. 18 kHz.
// The two oscillator envelopes run under the instrument's ADSR (which gates the voice).
//
// display[16..]: [16] osc 1 wave Q15 (x 4 = shape index), [17] osc 2 wave Q15, [18] osc 2 offset
//                (semitones * 256), [19] cutoff in Hz, [20] cross-mod index (Q16 cycles), [21] ring Q15,
//                [22] [23] output level of oscillator 1 / 2 of the newest triggered voice (block peak, Q15, after
//                its envelope; was the envelope level until t2 2026-10-23, changed on t3's request),
//                [24..54] scope: 62 int16 Q15 samples of the newest voice's output, every 32nd sample, oldest
//                first, packed two per slot (low half = even index). 0 when idle (t3, 2026-10-23).
#include <algorithm>

#include "synth_instrument.h"

namespace op1::dsp {
namespace {

struct Osc {
  u32 ph = 0;
  int step = -1;
  s32 held = 0;
  // Morphing shape at phase increment dt with phase offset pm; advances the phase.
  s32 tick(u32 dt, s32 w15, u32 pm, Rng& rng) noexcept {
    const u32 p = ph + pm;
    const s32 x = w15 * 4;  // 0 .. 4 * 32767
    const int i = x >> 15 > 3 ? 4 : x >> 15;
    const s32 f = i >= 4 ? 0 : (x & 32767);
    const s32 a = shape(i, p, dt, rng);
    const s32 b = f > 0 ? shape(i + 1, p, dt, rng) : 0;
    ph += dt;
    return f > 0 ? lerp15(a, b, f) : a;
  }
  s32 shape(int k, u32 p, u32 dt, Rng& rng) noexcept {
    switch (k) {
      case 0: return sin_q31(p);
      case 1: return tri_q31(p);
      case 2: return saw_blep(p, dt);
      case 3: return pulse_blep(p, dt, kHalfCycle);
      default: {  // stepped noise: a new level 16 times per cycle (keeps the pitch audible)
        const int s = int(p >> 28);
        if (s != step) {
          step = s;
          held = rng.next();
        }
        return held;
      }
    }
  }
};

// Per-oscillator envelope: shape 0..0.5 = decay 40 ms .. 8 s, 0.5..1 = attack 1 ms .. 2 s with hold.
struct Env {
  s32 level = 0;  // Q31
  bool attacking = false;
  void trigger() noexcept { attacking = true; }
  s32 advance(int n, s32 shape15) noexcept {
    static constexpr ExpCurve kDecay(0.04, 8.0), kAttack(0.001, 2.0);
    if (shape15 < 16384) {
      if (attacking) {
        level = kOne;
        attacking = false;
      }
      const u64 tau = kDecay.at(shape15 * 2);  // seconds Q16
      const s64 k = (s64(1) << 47) / s64(tau * kSampleRate + 1);  // per sample, Q31 (tau in Q16 seconds)
      const s64 d = (s64(level) * clampv<s64>(k * n, 0, kOne)) >> 31;
      level = s32(s64(level) - d);
    } else if (attacking) {
      const u64 t = kAttack.at((shape15 - 16384) * 2);
      const s64 step = (s64(1) << 47) / s64(t * kSampleRate + 1) * n;
      const s64 v = s64(level) + step;
      if (v >= kOne) {
        level = kOne;
        attacking = false;
      } else {
        level = s32(v);
      }
    }
    return level;
  }
};

struct DsynthEngine {
  static constexpr InstrumentType kType = InstrumentType::DSynth;
  struct Voice {
    Rng rng{9001};
    Osc o1, o2;
    Env e1, e2;
    Svf f;
    u64 age = 0;
  };

  s32 xf = 0, w1 = 0, env1 = 0, w2 = 0, env2 = 0;
  s64 fm_q16 = 0;  // phase-modulation index, Q16 cycles
  s32 ring = 0;    // Q15
  u64 ratio2 = 65536;
  s32 semis2 = 0;
  SvfCoef coef;
  u32 cutoff_hz = 0;
  u64 clock = 0;
  s32 shown_e1 = 0, shown_e2 = 0;  // oscillator output levels of the newest voice (block peak, Q15)
  u64 shown_age = 0;
  static constexpr int kScope = 62;
  int16_t scope[kScope] = {};
  int scope_pos = 0, scope_div = 0;  // ring position; samples until the next kept one (every 32nd)
  bool newest_rendered = false;

  void control(const s32 k[kParams]) noexcept {
    if (!newest_rendered) {  // idle: the scope runs on with silence, the levels drop
      for (int i = 0; i < kBlock / 32; ++i) {
        scope[scope_pos] = 0;
        scope_pos = (scope_pos + 1) % kScope;
      }
      shown_e1 = shown_e2 = 0;
    }
    newest_rendered = false;
    static constexpr ExpCurve kCut(120.0, 18000.0);
    xf = u15(k[0]);
    w1 = u15(k[1]);
    env1 = u15(k[2]);
    const s32 xm = u15(k[3]);
    fm_q16 = (s64(sq15(xm)) * 3 * 65536 / 2) >> 15;  // xm^2 * 1.5 cycles
    ring = xm > 19661 ? clampv<s32>(s32((s64(xm - 19661) * 32767) / 13106), 0, 32767) : 0;
    semis2 = s32((s64(u15(k[4])) * 36 * 256) >> 15);
    ratio2 = semis_ratio_q16(semis2);
    w2 = u15(k[5]);
    env2 = u15(k[6]);
    const u64 hz = kCut.at(k[7]);
    cutoff_hz = u32(hz >> 16);
    coef = svf_coef(hz_inc(hz), svf_k_q(0.7));
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (legato) return;
    v.e1.trigger();
    v.e2.trigger();
    v.o2.ph = 1u << 30;
    v.o1.ph = 0;
    v.age = ++clock;
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    const bool shown = v.age >= shown_age;
    s32 pk1 = 0, pk2 = 0;
    const s32 a1 = v.e1.level, a2 = v.e2.level;
    const s32 b1 = v.e1.advance(n, env1), b2 = v.e2.advance(n, env2);
    const u32 inc2 = inc_mul(inc, ratio2);
    for (int i = 0; i < n; ++i) {
      const s32 t15 = s32((s64(i) * 32767) / n);
      const s32 e1 = lerp15(a1, b1, t15), e2 = lerp15(a2, b2, t15);
      // crossfader slides toward osc 2 as env 1 falls: pos = xf + (1 - env1) * xf / 2
      const s32 pos = clampv<s32>(xf + s32((((s64(kOne) - e1) >> 16) * xf) >> 16), 0, 32767);
      const s32 s1 = v.o1.tick(inc, w1, 0, v.rng);
      s32 s2 = v.o2.tick(inc2, w2, pm_offset(s1, fm_q16), v.rng);
      if (ring) s2 = lerp15(s2, sat((s64(mulq(s2, s1)) * 52429) >> 15), ring);  // ring x 1.6
      const s32 v1 = mul15(mulq(s1, e1), 32767 - pos), v2 = mul15(mulq(s2, e2), pos);
      const s32 y = v.f.tick(coef, sat(s64(v1) + v2));
      out[i] = soft_clip((s64(y) * 9) >> 3);
      if (shown) {
        pk1 = std::max(pk1, v1 < 0 ? -(v1 >> 16) : v1 >> 16);
        pk2 = std::max(pk2, v2 < 0 ? -(v2 >> 16) : v2 >> 16);
        if (--scope_div <= 0) {
          scope_div = 32;
          scope[scope_pos] = int16_t(out[i] >> 16);
          scope_pos = (scope_pos + 1) % kScope;
        }
      }
    }
    if (shown) {
      shown_age = v.age;
      shown_e1 = pk1;
      shown_e2 = pk2;
      newest_rendered = true;
    }
  }

  void view(int32_t* d) const noexcept {
    d[0] = w1;
    d[1] = w2;
    d[2] = semis2;
    d[3] = int32_t(cutoff_hz);
    d[4] = int32_t(fm_q16);
    d[5] = ring;
    d[6] = shown_e1;
    d[7] = shown_e2;
    for (int j = 0; j < kScope / 2; ++j) {
      const int a = (scope_pos + 2 * j) % kScope, b = (a + 1) % kScope;  // oldest first
      d[8 + j] = int32_t(uint32_t(uint16_t(scope[a])) | (uint32_t(uint16_t(scope[b])) << 16));
    }
  }
};

}  // namespace

Instrument* make_dsynth(void* mem, size_t size) noexcept { return make_synth<DsynthEngine>(mem, size); }

}  // namespace op1::dsp
