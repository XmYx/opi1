// The sequencer states the OG keeps in op1.db (t5, 2026-10-23), turned into the Domain::Seq commands that load
// them into the port's sequencers at start, as the device restores them at boot:
//   okseq    (is_synth, data {length[14], notes[896], pm, swing})          -> finger
//   drumseq  (is_synth, data {length, notes[16 masks], pm, start, swing})  -> pattern
//   endless  (is_synth, data {euclidean, euclidean_offset, playmode, steps [[key, len, ...] | null], swing, timing})
//   knobs    tombolagravity / tombolarestitution / tombolaangle            -> tombola (both sides)
//   sequencer synth_sequencer / drum_sequencer = the selected type (both "finger" on R.00246)
// is_synth 1 = the synth side (Command.slot 0), 0 = the drum side (slot 1). The sketch has no stored state on a
// fresh image: its table easseq is created empty on first save (firmware string 0x018D73A8). The arpeggio has
// none either (its defaults are a JSON constant in the code, 0x018FAA58, and match the engine's).
// Command meanings: src/seq/seq_engines.h (SetParam / Edit tables).
#pragma once

#include <string>
#include <vector>

#include "op1/messages.h"
#include "op1/ui.h"

namespace op1::state {

// Reads op1.db from the image and appends the commands to `out`. false + err if op1.db is missing or
// unreadable; a missing or malformed table is skipped (and named in err) without failing the rest.
bool sequencer_state_commands(const AssetProvider& assets, std::vector<Command>& out, std::string& err);

// The same from op1.db bytes (tests).
bool sequencer_state_commands(const std::vector<uint8_t>& op1_db, std::vector<Command>& out, std::string& err);

}  // namespace op1::state
