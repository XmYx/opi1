// Contract of the sequencer/tempo part of t5 (firmware-behaviour.md §8): pattern, endless, tombola,
// finger, sketch, arpeggio; tempo 40-220 BPM in tenths; MIDI clock in and out.
// Every note the core receives goes through Sequencer::event() first and comes back out of process()
// (unchanged when no sequencer is active), so sequencers that transform played notes (arpeggio,
// endless, finger) behave as on the device. Owner: t3 (interface); t5 implements in src/seq/.
#pragma once

#include <cstdint>
#include <memory>

#include "op1/config.h"
#include "op1/dsp.h"
#include "op1/messages.h"
#include "op1/snapshot.h"

namespace op1 {

// Fixed-size event list filled on the audio thread (no allocation).
struct EventSink {
  static constexpr int kMax = 64;
  Event ev[kMax];
  int n = 0;
  bool push(const Event& e) noexcept {
    if (n == kMax) return false;
    ev[n++] = e;
    return true;
  }
  void clear() noexcept { n = 0; }
};

// Domain::Seq operations (slot 0 = synth side, 1 = drum side). t5 may append, never renumber.
enum class SeqOp : uint16_t {
  Select = 1,      // value = SeqType
  Start = 2, Stop = 3,
  SetParam = 4,    // index, value: sequencer-specific (t5 defines per type)
  Edit = 5,        // index, value: step/point edits (t5 defines per type)
  // t5 addition (docs/interface-changes.md): turn the side's sequencer on/off. Off = notes pass straight
  // through. SetParam/Edit meanings per type are in src/seq/seq_engines.h.
  SetEnabled = 6,  // value 0/1
  // t5 (2026-10-23): SetParam / Edit for one sequencer type whether or not it is selected (loading the stored
  // op1.db states at start). index = (SeqType << 16) | the SetParam/Edit index (0..65535).
  SetParamOf = 7,
  EditOf = 8,
  // t5 (2026-10-23): the sequencer page's controls as the firmware's controllers read them
  // (src/seq/seq_engines.h has each sequencer's table):
  Encoder = 9,   // index 0..3 = blue, green, white, orange, 4..7 = the same with shift; value = detents (signed)
  KeyEdit = 10,  // shift + a keyboard key (step entry): index = key 0..23, value 1 down / 0 up; index -1 = shift up
  Nav = 11,      // index 0 = <<, 1 = >>, 2 = lift, 3 = drop; value = shift (0/1)
};
// Domain::Tempo operations.
enum class TempoOp : uint16_t {
  SetBpmTenths = 1,  // 400..2200
  SetMetronome = 2,  // value 0/1
  SetSync = 3,       // value: 0 internal, 1 MIDI clock in (t5 may add more). The firmware's own sync-type
                     // values 2 and 4 (og-behaviour-details.md §2.2) are also accepted as "MIDI clock in".
  SetSyncOut = 4,    // t5 addition: value 0/1, send MIDI clock and Start/Stop while the transport runs
};

class Sequencer {
 public:
  virtual ~Sequencer() = default;
  virtual void command(const Command& c) noexcept = 0;  // Domain::Seq and Domain::Tempo
  // Incoming notes (target already resolved to Synth or Drum) and MIDI clock/transport.
  virtual void event(const Event& e) noexcept = 0;
  // Emits the notes to play in this chunk (offset = frame in chunk) and the MIDI messages to send.
  virtual void process(const ProcessContext& ctx, EventSink& notes, EventSink& midi_out) noexcept = 0;
  virtual int16_t bpm_tenths() const noexcept = 0;
  virtual void view(SeqView& seq, TempoView& tempo) const noexcept = 0;
};

std::unique_ptr<Sequencer> create_sequencer();  // main thread, at startup

}  // namespace op1
