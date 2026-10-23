// Synth engine screen "fmpopup" (moved from bind_engines1.cpp by t3; one file per firmware screen template).
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// =====================================================================================================
// fmpopup (fmpopup.svg). Strings next to "fmpopup.svg": blue, green, arrow; neighbours are fm_radio.cpp
// asserts. It is NOT an FM-synth pop-up: the template is the FM RADIO notice (EU radio directive text,
// a knob with a "push" arrow). Group "blue" = the instruction with the blue knob, group "green" = the
// green-knob variant; both are authored on top of each other. This port has no FM radio, so the screen is
// only reachable by browsing (OP1_OG_SCREEN=fmpopup); it shows one coherent state: the blue variant,
// with the push arrow nudging down toward the knob (ASSUMED animation).
// =====================================================================================================

void bind_fmpopup(BindCtx& c) {
  show(c.n("blue"), true);
  show(c.n("green"), false);
  const float ph = std::fmod(float(c.t), 1.f);
  move(c.n("arrow"), 0, ph < 0.5f ? 0.f : 3.f);
}

OP1_SCREEN("fmpopup", bind_fmpopup, nullptr);

}  // namespace
}  // namespace op1
