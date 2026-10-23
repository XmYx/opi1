// Synth engine screen "pd" (phase engine; pd.svg). Decoded from the firmware's Pd screen class by t3
// (SPEC-pd.md): ctor 0x01A0CD50 (singleton 0x01DE4BD8, vtable 0x0191116C), on-load 0x01A0CCA0, per-frame
// update 0x01A0C0A0. Evidence labels: decoded = read from the Blackfin code, inferred, ASSUMED.
//
// The firmware draws NO curve by code here: every frame it only sets a 3x3 transform on five template nodes
// (decoded):
//   blue  (curve A + its dots)  translate x by 30*p0 + 0.001*|L1|
//   green (curve B + its dots)  translate x by 30*p1 - 0.001*|L1|*p1
//   white (dashed level line)   translate y by 30*p2
//   red1  (resonance wave)      scale x by sR about red1's left bbox edge, y by sW about red1's bbox centre
//   red2  (its ghost)           scale x by sR about the same left edge (no vertical scale)
//   with sR = 2.95*(1-p3)^2 + 0.65 + 2e-5*L2 and sW = 1 - 0.8*p2,
// p0..p3 = the engine's normalized parameters 0..1 (blue, green, white, orange). The authored red wave is
// the state p3 ~ 0.66 (sR = 1); at p3 = 0 it is stretched 3.6x to the right (past the plot, into the arrow,
// which the template draws last on purpose).
// L1, L2 are two audio-activity values the Pd voice routine accumulates into 0x01DE4B8C (lo: sum of the
// product of two phase words, hi: sum of a mid-block output sample, both saturating Q15) and that the screen
// smooths (0x01A0BF24: y = 0.96875*y + x/64, read twice per frame). The snapshot does not carry them: the
// port derives stand-ins (ASSUMED, see pd_activity) and REQUESTS display slots for the raw values.
// =====================================================================================================
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

struct PdActivity {
  float lo = 0, hi = 0;  // smoothed values, Q15 units
  float L1 = 0, L2 = 0;  // what the screen reads: |lo| after the first call, hi after the second
  double last_t = -1;
  uint32_t n = 0;
};
PdActivity g_pd;

// One frame of the firmware's smoother fed with stand-in raw values (ASSUMED until the engine publishes
// them): lo = saturated sum, over the blocks of one frame and the sounding voices, of a product of two
// phase words (pseudo-random here); hi = saturated sum of output samples taken from the scope (one per block).
void pd_activity(BindCtx& c) {
  PdActivity& A = g_pd;
  const InstrumentView& iv = c.inst();
  if (c.t == A.last_t) return;
  const double dt = A.last_t < 0 ? 1.0 / 60.0 : std::clamp(c.t - A.last_t, 0.0, 0.1);
  A.last_t = c.t;
  float xlo = 0, xhi = 0;
  if (sounding(iv)) {
    int voices = 0;
    for (int v = 0; v < kSynthVoices; ++v) voices += (iv.voices_active >> v) & 1;
    const int blocks = std::max(1, int(dt * kSampleRate / kBlock));
    for (int b = 0; b < blocks * voices; ++b) {
      const uint32_t h = hash32(++A.n * 0x9E3779B9u);
      const float u1 = float(int16_t(h & 0xFFFF)), u2 = float(int16_t(h >> 16));
      xlo += u1 * u2 / 32768.f;
    }
    for (int b = 0; b < blocks; ++b) {
      const Sample s = c.s.scope[(b * kDisplayW) / blocks % kDisplayW];
      xhi += float(s) / 65536.f;  // scope samples are Q31-ish int32 -> Q15
    }
    xlo = std::clamp(xlo, -32768.f, 32767.f);
    xhi = std::clamp(xhi, -32768.f, 32767.f);
  }
  A.lo = A.lo * 0.96875f + xlo / 64.f;  // first call: update; the lo half is read here
  A.L1 = std::fabs(A.lo);
  A.hi = A.hi * 0.96875f + xhi / 64.f;
  A.lo *= 0.96875f;                     // second call (raw value already cleared): decay; hi half read
  A.hi *= 0.96875f;
  A.L2 = A.hi;
}

void bind_pd(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  pd_activity(c);
  const float L1 = g_pd.L1, L2 = g_pd.L2;
  const float p0 = knob(iv, 0), p1 = knob(iv, 1), p2 = knob(iv, 2), p3 = knob(iv, 3);

  move(c.n("blue"), 30.f * p0 + 0.001f * L1, 0.f);
  move(c.n("green"), 30.f * p1 - 0.001f * L1 * p1, 0.f);
  move(c.n("white"), 0.f, 30.f * p2);

  const float sR = 2.95f * (1.f - p3) * (1.f - p3) + 0.65f + 2e-5f * L2;
  const float sW = 1.f - 0.8f * p2;
  const svg::Box r1 = c.box("red1");  // the on-load stores red1's bbox left x and centre y
  if (r1.empty()) return;
  scale(c.n("red1"), sR, sW, r1.x0, r1.cy());
  scale(c.n("red2"), sR, 1.f, r1.x0, r1.cy());
}

OP1_SCREEN("pd", bind_pd, nullptr);

}  // namespace
}  // namespace op1
