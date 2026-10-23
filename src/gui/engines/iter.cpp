// Synth engine screen "iter" (iter.svg; firmware Iter.cpp screen class, vtable 0x19103fc).
// Decoded from the firmware (t3, see SPEC-iter.md): init 0x019b9de4, per-frame update 0x019b9ea4.
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// ============================================================================================ iter
// The hidden engine ("..\src\dsp\Iter.cpp:82"). Strings next to "iter.svg": "bar%i", "base%i".
// Decoded: init keeps bar0..3 and the centre of base0..3's bounds. Every frame, for i = 0..3, bar<i> is
// rotated clockwise by param[i] * 1.5 pi (270 degrees; param * 1536.0f * pi >> 10 in Q10 radians, sine table
// of 1024 entries) about base<i>'s centre; then the document is rendered. Nothing else moves or is drawn
// (the dots, ticks and colours stay as authored). param[i] = the engine's four parameters 0..1 (blue, green,
// white, orange); the authored needles point at 7:30 = parameter 0.
void bind_iter(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  for (int i = 0; i < 4; ++i) {
    svg::Node* bar = c.n("bar" + std::to_string(i));
    const svg::Box b = c.box("base" + std::to_string(i));
    if (bar && !b.empty()) rotate(c, bar, knob(iv, i) * 270.f, b.cx(), b.cy());
  }
}

OP1_SCREEN("iter", bind_iter, nullptr);

}  // namespace
}  // namespace op1
