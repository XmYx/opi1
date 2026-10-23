// Lookup tables of the engine track, built once by dsp_init() on the main thread (floating point is fine
// here: nothing in this file runs on the audio thread). See dsp_util.h for each table's format.
#include <algorithm>
#include <cmath>

#include "dsp_util.h"

namespace op1::dsp {

s32 g_sin[kSinSize + 1];
u32 g_pow2[1025];
u64 g_note_inc[3072];
s32 g_tan[4097];
s32 g_onepole[4097];
s32 g_tanh[4097];
u32 g_env_attack[258];
s32 g_env_coef[258];
s32 g_glide[258];

namespace {
s32 q31(double x) {
  const double v = x * 2147483648.0;
  return v >= 2147483647.0 ? INT32_MAX : (v <= -2147483648.0 ? INT32_MIN : s32(std::lround(v)));
}
}  // namespace

void init_tables() noexcept {
  const double pi = 3.14159265358979323846;
  const double fs = kSampleRate;
  // symmetric (+-(2^31 - 1)): a sine value can always be negated without overflow
  for (int i = 0; i <= kSinSize; ++i) g_sin[i] = std::max(-INT32_MAX, q31(std::sin(2 * pi * i / kSinSize)));
  g_sin[kSinSize] = g_sin[0];
  for (int i = 0; i <= 1024; ++i) g_pow2[i] = u32(std::llround(std::pow(2.0, i / 1024.0) * 1073741824.0));
  const double f0 = 440.0 * std::pow(2.0, -69.0 / 12.0);  // MIDI note 0
  for (int i = 0; i < 3072; ++i)
    g_note_inc[i] = u64(std::llround(f0 * std::pow(2.0, i / 3072.0) / fs * 4294967296.0 * 65536.0));
  for (int i = 0; i <= 4096; ++i) {
    const double w = pi * i / 8192.0;  // pi * f/fs
    const double t = i >= 4096 ? 1000.0 : std::min(1000.0, std::tan(w));
    g_tan[i] = s32(std::lround(t * 65536.0));
    g_onepole[i] = q31(1.0 - std::exp(-2.0 * w));
    g_tanh[i] = q31(std::tanh(i / 512.0));
  }
  // Envelope curves over a stored value 0..16384 (the firmware's A/D/R values live in 64..16320):
  // attack 0.5 ms .. 8 s (linear ramp), decay/release time to -60 dB 5 ms .. 12 s (exponential).
  for (int i = 0; i <= 257; ++i) {
    const double v = std::min(1.0, i / 256.0);
    const double ta = 0.0005 * std::pow(8.0 / 0.0005, v);
    g_env_attack[i] = u32(std::max(1.0, std::min(2147483647.0, 2147483648.0 / (ta * fs))));
    const double td = 0.005 * std::pow(12.0 / 0.005, v);
    g_env_coef[i] = q31(1.0 - std::exp(-6.907755 / (td * fs)));
    // Portamento: stored 64..12864 -> glide time constant 10 ms .. 2 s, as a per-block coefficient.
    const double tg = 0.010 * std::pow(2.0 / 0.010, std::min(1.0, i / 201.0));
    g_glide[i] = q31(1.0 - std::exp(-double(kBlock) / (tg * fs)));
  }
}

}  // namespace op1::dsp
