// The sequencer states stored in op1.db load into the port's sequencers as on the device at boot (t5,
// 2026-10-23; src/state/seq_state.h). Needs the user's firmware image (OP1_OG_FIRMWARE, as the other state tests;
// SKIPPED without one). Checks, against the factory op1.db of R.00246:
//   - every stored table turns into commands (finger 2 x (2 + 14 + 896), pattern, endless, tombola, selection);
//   - the synth side's stored pattern plays its first steps when a key is held (step 0 = keys 7 and 14);
//   - the drum side's finger slot 0 (the lowest white key) plays note 53 on every fourth sixteenth.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "op1/sequencer.h"
#include "op1/ui.h"
#include "../../src/state/seq_state.h"

using namespace op1;

namespace {

std::vector<int> play(const std::vector<Command>& cmds, int side, SeqType t, int note, int frames) {
  auto s = create_sequencer();
  for (const Command& c : cmds) s->command(c);
  Command sel;
  sel.domain = Domain::Seq;
  sel.slot = uint8_t(side);
  sel.op = uint16_t(SeqOp::Select);
  sel.value = int32_t(t);
  s->command(sel);
  sel.op = uint16_t(SeqOp::SetEnabled);
  sel.value = 1;
  s->command(sel);
  s->event(note_on(uint8_t(note), 100, Source::Keyboard, 1, side ? Target::Drum : Target::Synth));
  EventSink notes, midi;
  std::vector<int> on;
  for (int64_t f = 0; f < frames; f += kBlock) {
    ProcessContext ctx;
    ctx.frame = f;
    ctx.frames = kBlock;
    ctx.block_start = true;
    ctx.bpm_tenths = s->bpm_tenths();
    notes.clear();
    midi.clear();
    s->process(ctx, notes, midi);
    for (int k = 0; k < notes.n; ++k)
      if (notes.ev[k].type == EventType::NoteOn && notes.ev[k].b > 0) on.push_back(notes.ev[k].a);
  }
  return on;
}

}  // namespace

int main() {
  std::string fw = std::getenv("OP1_OG_FIRMWARE") ? std::getenv("OP1_OG_FIRMWARE") : "";
  if (fw.empty()) {
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const std::string def = home + "/rigs-work/op1-og-fw/fw/op1_246.op1";
    if (FILE* f = std::fopen(def.c_str(), "rb")) { std::fclose(f); fw = def; }
  }
  auto assets = create_asset_provider();
  std::string err;
  if (fw.empty() || !assets->open(fw, err)) {
    std::printf("seq_state: SKIPPED (no firmware image: %s)\n", err.empty() ? "set OP1_OG_FIRMWARE" : err.c_str());
    return 0;
  }
  std::vector<Command> cmds;
  CHECK(state::sequencer_state_commands(*assets, cmds, err));
  std::printf("seq_state: %zu commands from op1.db%s%s\n", cmds.size(), err.empty() ? "" : "; ", err.c_str());
  CHECK(cmds.size() > 2 * (2 + 14 + 896));
  // synth pattern (drumseq is_synth 1): step 0 = keys 7 and 14 = notes 60, 67 when C4 is held, 1/16 steps at 125 BPM
  const auto pat = play(cmds, 0, SeqType::Pattern, 60, 44100);
  std::printf("synth pattern, C4 held, 1 s:");
  for (int n : pat) std::printf(" %d", n);
  std::printf("\n");
  CHECK(pat.size() >= 2);
  if (pat.size() >= 2) { CHECK_EQ(pat[0], 60); CHECK_EQ(pat[1], 67); }
  // drum finger (okseq is_synth 0): slot 0 = note 53 on steps 0, 4, 8, 12
  const auto fin = play(cmds, 1, SeqType::Finger, 53, 44100);
  std::printf("drum finger, F3 held, 1 s:");
  for (int n : fin) std::printf(" %d", n);
  std::printf("\n");
  CHECK(fin.size() >= 2);
  for (int n : fin) CHECK_EQ(n, 53);
  return check_result("seq_state");
}
