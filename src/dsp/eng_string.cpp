// string (id 3, St.cpp): two Karplus-Strong waveguide strings, the second detuned, summed (mono). Designed by
// ear (the firmware routine is not decoded) on the OG facts: defaults 1516/16704/0/15168.
//
// Knobs: blue (0) = tension (loop damping: slack and dark low, taut and bright high, plus a slight pitch
//        rise), green (1) = decay (loop feedback, T60 0.15 .. 25 s), white (2) = detune (second string up to
//        +40 cents), orange (3) = impulse (excitation: white noise burst > soft filtered noise > triangle
//        pluck > continuous bowing above ~60 %).
// Each note (not a legato pitch change) restarts the excitation from the same noise seed.
//
// display[16..]: [16] tension Q15, [17] T60 in ms, [18] detune cents, [19] impulse type 0 noise, 1 soft,
//                2 pluck, 3 bow, [20] impulse Q15,
//                [24..39] the first 16 delay-line cells of the most recently triggered voice (string 1), newest
//                cell first, Q15 signed, at voice amplitude (before the voice envelope and gain); per block, 0 when
//                that voice is idle. The st screen redraws line0..15 from the firmware's delay line (t3, 2026-10-23).
#include "synth_instrument.h"

namespace op1::dsp {
namespace {

constexpr int kLine = 4096;  // > 44100 / 15 samples: the lowest string ~11 Hz

struct Waveguide {
  DelayLine<kLine> line;
  s32 prev = 0;
  // One sample: period (Q16 samples), damping s (Q16, 0..0.5), feedback fb (Q31).
  s32 tick(s32 excite, s64 period_q16, s32 s_q16, s32 fb) noexcept {
    const s64 d = period_q16 - s_q16 < (s64(1) << 16) ? (s64(1) << 16) : period_q16 - s_q16;
    const s32 out = line.readf(d);
    const s32 damped = s32((s64(out) * (65536 - s_q16) + s64(prev) * s_q16) >> 16);
    prev = out;
    line.write(soft_clip(((s64(damped) * fb) >> 31) + excite));
    return out;
  }
};

struct StringEngine {
  static constexpr InstrumentType kType = InstrumentType::String;
  struct Voice {
    Waveguide a, b;
    Rng rng{4242};
    int burst = 0, burst_len = 1;
    OnePole color;
    u32 bow1 = 0, bow2 = kHalfCycle;
  };

  const Voice* newest = nullptr;  // most recently triggered voice (voices live in the instrument: stable)
  bool newest_rendered = false, newest_active = false;
  s32 tension15 = 0, imp15 = 0, s_q16 = 32768, pluck15 = 0, bow15 = 0, color_coef = kOne, cents = 0;
  u64 tens_ratio = 65536, ratio2 = 65536, t60_q16 = 65536;

  void control(const s32 k[kParams]) noexcept {
    static constexpr ExpCurve kT60(0.15, 25.0);
    static constexpr ExpCurve kColor(600.0, 16000.0);
    newest_active = newest_rendered;  // latched per block: the screen goes quiet with the voice
    newest_rendered = false;
    tension15 = u15(k[0]);
    t60_q16 = kT60.at(k[1]);
    cents = s32(s64(u15(k[2])) * 40 / 32767);
    imp15 = u15(k[3]);
    tens_ratio = 65536 + u64(tension15) * 262 / 32767;          // 1 + 0.004 t
    ratio2 = cents_ratio_q16(cents);
    s_q16 = 32768 - s32(s64(tension15) * 30147 / 32767);        // 0.5 .. 0.04
    const s32 bright = s32(clampv<s64>(s64(imp15) * 8 / 5, 0, 32767));
    color_coef = onepole_coef(kColor.inc(32767 - bright));
    pluck15 = s32(clampv<s64>((s64(imp15) - 11469) * 32767 / 9830, 0, 32767));
    bow15 = s32(clampv<s64>((s64(imp15) - 19661) * 32767 / 13107, 0, 32767) * 1147 >> 15);  // up to 0.035
  }

  void note_on(Voice& v, bool legato) noexcept {
    if (legato) return;
    newest = &v;
    v.rng = Rng(4242);
    v.burst = -1;  // length set from the pitch at the next render
  }

  // Loop feedback for a T60 at the loop frequency: 10^(-3 / (t60 f)) = 2^(-9.966 / (t60 f)).
  s32 feedback(u32 inc) const noexcept {
    constexpr u64 kK = u64(9.965784284662087 * 18446744073709551616.0 / kSampleRate);
    const u64 den = t60_q16 * u64(inc ? inc : 1);
    const s64 x_q16 = s64(kK / (den ? den : 1));
    const u64 fb_q16 = pow2_q16(-x_q16);
    constexpr s64 kTop = s64(0.99995 * 2147483648.0);
    return s32(clampv<s64>(s64(fb_q16) << 15, 0, kTop));
  }

  void render(Voice& v, u32 inc, s32* out, int n) noexcept {
    const u32 inc1 = inc_mul(inc, tens_ratio), inc2 = inc_mul(inc1, ratio2);
    constexpr s64 kMinP = s64(2) << 16, kMaxP = s64(kSampleRate / 15) << 16;
    const s64 p1 = clampv<s64>(s64((u64(1) << 48) / (inc1 ? inc1 : 1)), kMinP, kMaxP);
    const s64 p2 = clampv<s64>(s64((u64(1) << 48) / (inc2 ? inc2 : 1)), kMinP, kMaxP);
    const s32 fb1 = feedback(inc1), fb2 = feedback(inc2);
    if (v.burst < 0) {
      v.burst_len = int(p1 >> 16) < 8 ? 8 : int(p1 >> 16);
      v.burst = v.burst_len;
    }
    for (int i = 0; i < n; ++i) {
      s32 ex = 0;
      if (v.burst > 0) {
        const s32 pos = s32(32767 - s64(v.burst) * 32767 / v.burst_len);  // 0..1 across the burst
        const s32 d = 2 * pos - 32767;
        const s32 tri = 32767 - 2 * (d < 0 ? -d : d);
        const s32 nz = v.color.tick(color_coef, v.rng.next());
        ex = lerp15(nz, tri * 65536, pluck15);
        ex = s32((s64(ex) * 29491) >> 15);  // 0.9
        --v.burst;
      }
      s32 e1 = ex, e2 = ex;
      if (bow15 > 0) {
        e1 = add(e1, mul15(saw_blep(v.bow1, inc1), bow15));
        e2 = add(e2, mul15(saw_blep(v.bow2, inc2), bow15));
        v.bow1 += inc1;
        v.bow2 += inc2;
      }
      const s32 y1 = v.a.tick(e1, p1, s_q16, fb1);
      const s32 y2 = v.b.tick(e2, p2, s_q16, fb2);
      out[i] = soft_clip(((s64(y1) + y2) * 31130) >> 15);  // 0.95
    }
    if (&v == newest) newest_rendered = true;
  }

  void view(int32_t* d) const noexcept {
    d[0] = tension15;
    d[1] = int32_t((t60_q16 * 1000) >> 16);
    d[2] = cents;
    d[3] = choose(imp15, 4);
    d[4] = imp15;
    for (int c = 0; c < 16; ++c) d[8 + c] = newest && newest_active ? newest->a.line.read(c + 1) >> 16 : 0;
  }
};

}  // namespace

Instrument* make_string(void* mem, size_t size) noexcept { return make_synth<StringEngine>(mem, size); }

}  // namespace op1::dsp
