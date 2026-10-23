// Live screens (owner: t6, task t10): drives the UI model with OP-1 keys and encoders against a real Core
// (audio blocks pumped by hand) and checks that
//   1. keys open the right pages/templates in every mode, and encoders reach the engine (no firmware needed);
//   2. with the firmware image (OP1_OG_FIRMWARE, set by build.sh when the image is on this machine), every
//      screen template has a binding, renders without error, and the screens visibly change when encoders
//      turn, notes play and the tape runs (pixel difference between before and after).
// Without the image part 2 is skipped and the test says so; build.sh test sets OP1_OG_FIRMWARE when it finds
// the image, so on the development machine it always runs.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "op1/dsp.h"
#include "../../src/gui/bind.h"
#include "../../src/gui/svg.h"
#include "../../src/gui/ui_model.h"
#include "op1/core.h"
#include "op1/ui.h"

namespace {

using op1::Mode;
using op1::OpKey;

struct Rig {
  op1::Core core;
  std::unique_ptr<op1::AssetProvider> assets = op1::create_asset_provider();
  std::unique_ptr<op1::UiModel> ui;
  op1::svg::Image img{op1::kDisplayW, op1::kDisplayH};
  std::vector<op1::Sample> l = std::vector<op1::Sample>(64), r = std::vector<op1::Sample>(64);
  double t = 0;
  bool fw = false;

  Rig() {
    const char* f = std::getenv("OP1_OG_FIRMWARE");
    std::string err;
    fw = f && *f && assets->open(f, err);
    ui = std::make_unique<op1::UiModel>(core, *assets);
  }
  // runs the audio for `sec` seconds and advances the UI clock with it
  void pump(double sec) {
    const int blocks = int(sec * op1::kSampleRate / 64) + 1;
    for (int i = 0; i < blocks; ++i) core.process(nullptr, nullptr, l.data(), r.data(), 64);
    t += sec;
  }
  std::string screen() {
    pump(0.01);
    return ui->render(img, t);
  }
  op1::svg::Image shot() {
    pump(0.01);
    ui->render(img, t);
    return img;
  }
  void key(OpKey k, bool shift = false) {
    if (shift) ui->key(OpKey::Shift, true);
    ui->key(k, true);
    ui->key(k, false);
    if (shift) ui->key(OpKey::Shift, false);
    pump(0.01);
  }
  const op1::CoreSnapshot& snap() { return core.snapshot(); }
};

int diff(const op1::svg::Image& a, const op1::svg::Image& b) {
  int n = 0;
  for (size_t i = 0; i < a.px.size(); ++i) n += a.px[i] != b.px[i];
  return n;
}

#define EXPECT_SCREEN(rig, name)                                                                     \
  do {                                                                                               \
    const std::string s_ = (rig).screen();                                                           \
    if (s_ != (name)) std::fprintf(stderr, "screen is '%s', expected '%s'\n", s_.c_str(), (name)); \
    CHECK(s_ == (name));                                                                             \
  } while (0)

void pages() {
  Rig g;
  op1::UiModel& ui = *g.ui;
  ui.set_mode(Mode::Synth);
  EXPECT_SCREEN(g, "fm");
  g.key(OpKey::T2);
  EXPECT_SCREEN(g, "adsr");
  g.key(OpKey::T2, true);
  EXPECT_SCREEN(g, "playmode");
  g.key(OpKey::T3);
  const std::string fx0 = g.screen();
  CHECK(fx0 == op1::fx_screen(g.snap().synth.fx_type));
  g.key(OpKey::T3);  // again: the next effect type
  CHECK(g.screen() == op1::fx_screen(g.snap().synth.fx_type));
  CHECK(g.screen() != fx0);
  const bool fx_on = g.snap().synth.fx_on;
  g.key(OpKey::T3, true);  // shift: on/off
  CHECK(g.snap().synth.fx_on != fx_on);
  g.key(OpKey::T4);
  CHECK(g.screen() == op1::lfo_screen(g.snap().synth.lfo_type));
  g.key(OpKey::T1);
  EXPECT_SCREEN(g, "fm");
  ui.engine_step(1);
  EXPECT_SCREEN(g, "t10");
  ui.engine_step(-1);
  EXPECT_SCREEN(g, "fm");
  g.key(OpKey::Seq);
  CHECK(g.screen() == op1::seq_screen(g.snap().seq.synth_seq));
  CHECK(g.snap().seq.display[0] == 1);  // the sequencer key enables the side's sequencer
  const op1::SeqType st = g.snap().seq.synth_seq;
  g.key(OpKey::Seq, true);  // shift: next type
  CHECK(g.snap().seq.synth_seq != st);
  ui.set_mode(Mode::Synth);
  EXPECT_SCREEN(g, "fm");  // the mode key returns to the main page

  ui.set_mode(Mode::Drum);
  EXPECT_SCREEN(g, "drum2");
  g.key(OpKey::T2);
  EXPECT_SCREEN(g, "dynaenv");
  ui.set_mode(Mode::Drum);
  ui.engine_step(1);
  EXPECT_SCREEN(g, "dbox");

  ui.set_mode(Mode::Tape);
  EXPECT_SCREEN(g, "tape");
  g.key(OpKey::T3);
  CHECK(g.snap().tape.track == 2);
  ui.key(OpKey::Shift, true);
  ui.set_mode(Mode::Tape);
  ui.key(OpKey::Shift, false);
  EXPECT_SCREEN(g, "tapeconfig");

  ui.set_mode(Mode::Mixer);
  EXPECT_SCREEN(g, "mixer");
  g.key(OpKey::T2);
  EXPECT_SCREEN(g, "eq");
  g.key(OpKey::T3);
  CHECK(g.screen() == op1::fx_screen(g.snap().mixer.master_fx));
  g.key(OpKey::T4);
  EXPECT_SCREEN(g, "master");

  ui.set_mode(Mode::Tempo);
  EXPECT_SCREEN(g, "tempo");
  ui.set_mode(Mode::Clock);
  EXPECT_SCREEN(g, "clock");
  ui.set_mode(Mode::Album);
  EXPECT_SCREEN(g, "album");
  ui.set_mode(Mode::Communication);
  EXPECT_SCREEN(g, "com");
  g.key(OpKey::Help);
  EXPECT_SCREEN(g, "help");
  g.key(OpKey::Input);
  EXPECT_SCREEN(g, "in");
  ui.octave(1);
  EXPECT_SCREEN(g, "octave");  // the octave pop-up ...
  g.pump(1.0);
  EXPECT_SCREEN(g, "in");      // ... goes away
}

void encoders() {
  Rig g;
  op1::UiModel& ui = *g.ui;
  ui.set_mode(Mode::Synth);
  g.pump(0.01);
  for (int e = 0; e < 4; ++e) {
    const int before = g.snap().synth.knobs[e];
    ui.encoder(e, before > 16000 ? -10 : 10);
    g.pump(0.01);
    CHECK(g.snap().synth.knobs[e] != before);
  }
  const int k4 = g.snap().synth.knobs[4];  // shift: the second page
  ui.key(OpKey::Shift, true);
  ui.encoder(0, k4 > 16000 ? -10 : 10);
  ui.key(OpKey::Shift, false);
  g.pump(0.01);
  CHECK(g.snap().synth.knobs[4] != k4);

  g.key(OpKey::T2);
  const int a = g.snap().synth.adsr[0];
  ui.encoder(0, a > 16000 ? -10 : 10);
  g.pump(0.01);
  CHECK(g.snap().synth.adsr[0] != a);
  g.key(OpKey::T3);
  const int fx1 = g.snap().synth.fx[1];
  ui.encoder(1, fx1 > 16000 ? -10 : 10);
  g.pump(0.01);
  CHECK(g.snap().synth.fx[1] != fx1);
  g.key(OpKey::T4);
  const int lf2 = g.snap().synth.lfo[2];
  ui.encoder(2, lf2 > 16000 ? -10 : 10);
  g.pump(0.01);
  CHECK(g.snap().synth.lfo[2] != lf2);

  ui.set_mode(Mode::Mixer);
  const int v3 = g.snap().mixer.volume[3];
  ui.encoder(3, -10);
  g.pump(0.01);
  CHECK(g.snap().mixer.volume[3] < v3);
  ui.key(OpKey::Shift, true);
  ui.encoder(1, 10);
  ui.key(OpKey::Shift, false);
  g.pump(0.01);
  CHECK(g.snap().mixer.pan[1] > 0);

  ui.set_mode(Mode::Tempo);
  const int bpm = g.snap().tempo.bpm_tenths;
  ui.encoder(0, 5);
  g.pump(0.01);
  CHECK_EQ(int(g.snap().tempo.bpm_tenths), bpm + 50);

  ui.set_mode(Mode::Tape);
  g.key(OpKey::Play);
  g.pump(0.5);
  CHECK(g.snap().tape.playing);
  CHECK(g.snap().tape.position > op1::kSampleRate / 4);
  const int spd = g.snap().tape.display[5];
  ui.encoder(2, -5);
  g.pump(0.01);
  CHECK(g.snap().tape.display[5] != spd);
  g.key(OpKey::Stop);
  CHECK(!g.snap().tape.playing);

  // t4: the recorded line grows behind the head while recording, becomes a listed clip on stop, and stop disarms
  g.key(OpKey::Rewind);
  g.pump(0.01);
  g.key(OpKey::Record);
  g.pump(0.01);
  CHECK(g.snap().tape.armed && !g.snap().tape.recording);
  g.key(OpKey::Play);
  g.pump(0.5);
  const op1::TapeView ta = g.snap().tape;
  g.pump(0.5);
  const op1::TapeView tb = g.snap().tape;
  CHECK(ta.recording && tb.recording);
  CHECK(ta.rec_stop > ta.rec_start);
  CHECK(tb.rec_start == ta.rec_start);
  CHECK(tb.rec_stop - ta.rec_stop > op1::kSampleRate / 4);  // grew by about the 0.5 s that passed
  CHECK(tb.rec_stop >= tb.position - 1 && tb.rec_stop <= tb.position + 1);
  g.key(OpKey::Stop);
  g.pump(0.01);
  const op1::TapeView tc = g.snap().tape;
  CHECK(!tc.recording && !tc.armed);
  CHECK(tc.nclips == 1 && tc.clips[0].ch == 0);
  CHECK(tc.clips[0].start == tb.rec_start && tc.clips[0].stop >= tb.rec_stop);
}

// Before/after pixels for one action: the number of changed pixels.
template <class F>
int changes(Rig& g, F act) {
  const op1::svg::Image a = g.shot();
  act();
  const op1::svg::Image b = g.shot();
  return diff(a, b);
}

void live_screens() {
  Rig g;
  if (!g.fw) {
    std::printf("gui/test_bindings: firmware checks SKIPPED (OP1_OG_FIRMWARE not set): only pages and encoders checked\n");
    return;
  }
  op1::UiModel& ui = *g.ui;

  // every template has a binding (opfont is the font, not a screen)
  std::vector<std::string> unbound;
  for (const std::string& n : ui.screen_names())
    if (n != "opfont" && !op1::find_binding(n)) unbound.push_back(n);
  for (const std::string& n : unbound) std::fprintf(stderr, "no binding: %s\n", n.c_str());
  CHECK(unbound.empty());

  // every template renders through its binding, in two states, without error (ASan/UBSan in sanitize)
  for (const std::string& n : ui.screen_names()) {
    CHECK(ui.browse_to(n));
    g.shot();
    ui.note(0, true);
    g.pump(0.2);
    g.shot();
    ui.note(0, false);
  }
  ui.browse(0);

  // per mode: the screen changes with the controls (pixel counts printed for the record)
  auto add = [&](const char* what, int got, int min = 20) {
    std::printf("  %-34s %6d px changed\n", what, got);
    if (got < min) std::fprintf(stderr, "too few pixels changed: %s (%d < %d)\n", what, got, min);
    CHECK(got >= min);
  };
  // down then up: moves the value from anywhere in its range (many factory defaults sit at an end)
  auto wiggle = [&](int e) { ui.encoder(e, -20); ui.encoder(e, 7); g.pump(0.05); };
  ui.set_mode(Mode::Synth);
  for (int e = 0; e < 4; ++e) {
    char w[48];
    std::snprintf(w, sizeof w, "synth fm encoder %d", e);
    add(w, changes(g, [&] { wiggle(e); }));
  }
  add("synth fm note", changes(g, [&] { ui.note(4, true); g.pump(0.15); }));
  ui.note(4, false);
  g.key(OpKey::T2);
  add("adsr attack", changes(g, [&] { wiggle(0); }));
  g.key(OpKey::T3);
  add("fx encoders", changes(g, [&] { for (int e = 0; e < 4; ++e) wiggle(e); }));
  g.key(OpKey::T4);
  add("lfo encoders", changes(g, [&] { for (int e = 0; e < 4; ++e) wiggle(e); }));
  ui.set_mode(Mode::Drum);
  {  // t5: the drum encoders edit the last played key of the kit, so give the drum a kit (a synthetic 1 s sample,
     // without the worker) and play a key first; turning green then moves that key's start
    static std::vector<int16_t> kit(44100);
    for (size_t i = 0; i < kit.size(); ++i) kit[i] = int16_t(12000.0 * std::sin(double(i) * 0.05) * (1.0 - double(i % 1837) / 1837.0));
    static op1::SampleData sd;
    sd.frames = kit.data();
    sd.length = int32_t(kit.size());
    op1::Command c;
    c.domain = op1::Domain::Drum;
    c.op = uint16_t(op1::InstrumentOp::SetSample);
    c.ptr = &sd;
    g.core.post_ui(c);
    ui.note(3, true);
    g.pump(0.05);
    ui.note(3, false);
    g.pump(1.6);  // past the encoder read-outs of the synth pages
  }
  add("drum encoder", changes(g, [&] { ui.encoder(1, 12); g.pump(0.05); }));
  ui.set_mode(Mode::Tape);
  add("tape play 1 s", changes(g, [&] { g.key(OpKey::Play); g.pump(1.0); }));
  add("tape track T4", changes(g, [&] { g.key(OpKey::T4); }));
  g.key(OpKey::Stop);
  ui.set_mode(Mode::Mixer);
  add("mixer volume", changes(g, [&] { ui.encoder(0, -20); g.pump(0.05); }));
  ui.set_mode(Mode::Tempo);
  add("tempo bpm", changes(g, [&] { ui.encoder(0, 7); g.pump(0.05); }));
  ui.set_mode(Mode::Synth);
  g.key(OpKey::Seq);
  add("sequencer runs", changes(g, [&] {
        ui.set_mode(Mode::Tape);
        g.key(OpKey::Play);
        ui.set_mode(Mode::Synth);
        g.key(OpKey::Seq);
        ui.note(0, true);  // the finger, endless, sketch and arpeggio sequencers play while a key is held
        g.pump(1.0);
      }));
  ui.note(0, false);
}

}  // namespace

int main() {
  pages();
  encoders();
  live_screens();
  return check_result("gui/test_bindings");
}
