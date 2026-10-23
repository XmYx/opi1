// Sequencers, tempo, transport and MIDI out (owner: t5). Replaces the t3 placeholder.
//
// What runs here, on the audio thread, once per chunk (Core::render_chunk):
//   - every note the core receives (target already Synth or Drum) goes to the side's sequencer when that
//     side's sequencer is on, else straight through (same chunk, unchanged), as on the device;
//   - the 96-PPQN clock (tick_clock.h): internal at the set tempo, or following MIDI clock in;
//   - the transport: tape Play/Stop, SeqOp Start/Stop, or MIDI Start/Continue/Stop/Song Position in;
//   - MIDI out (the sequencer owns it): played and sequenced notes on the MIDI channel, clock (24 PPQN)
//     and Start/Continue/Stop while sync out is on, and the UI's controller messages (COM mode).
//
// Firmware behaviour reproduced (og-behaviour-details.md §2, §3):
//   - MIDI in Start: seek the tape to 0 and start the players; Continue: start without seeking; Stop:
//     stop. Each is re-emitted on MIDI out (here: when sync out is on).
//   - Song Position Pointer: acted on only with external sync and the transport stopped; the tape seeks
//     to SPP * 661,500 / BPM samples (the tape does the arithmetic, Tape::transport) and the sequencer
//     grid moves to SPP sixteenths.
//   - MIDI clock in: x4 into the 96-PPQN clock.
//   - All notes off: Note Off velocity 64 for every note held on MIDI out; no CC 123.
//   - Sync types 2 and 4 of the firmware mean "follow external clock"; 1 does too here (TempoOp::SetSync).
// Assumed where the firmware is not known: MIDI input is omni (every channel plays); sync out defaults
// to on; tape Play sends Start (not Song Position + Continue); played MIDI-in notes are not echoed.
#include <cstring>

#include "op1/io.h"
#include "op1/sequencer.h"
#include "op1/tape.h"
#include "seq_engines.h"
#include "tick_clock.h"

namespace op1 {
namespace {

using namespace seq;

class SequencerImpl;

class SideOut final : public Emitter {
 public:
  SequencerImpl* s = nullptr;
  int side = 0;
  void on(int note, int vel, int gate) noexcept override;
  void off(int note) noexcept override;
  void pass(const Event& e) noexcept override;
};

class SequencerImpl final : public Sequencer {
 public:
  SequencerImpl() noexcept {
    for (int i = 0; i < 2; ++i) {
      out_[i].s = this;
      out_[i].side = i;
      out_[i].drum = i == 1;
    }
    clock_.set_bpm_tenths(1250);  // op1.db: tempobpmdecimal 1250 = 125.0 BPM
  }

  // ------------------------------------------------------------ commands (audio thread, before process)
  void command(const Command& c) noexcept override {
    switch (c.domain) {
      case Domain::Tempo:
        switch (TempoOp(c.op)) {
          case TempoOp::SetBpmTenths: clock_.set_bpm_tenths(c.value); break;
          case TempoOp::SetMetronome: metronome_ = c.value != 0; break;
          case TempoOp::SetSync: set_sync(c.value); break;
          case TempoOp::SetSyncOut: sync_out_ = c.value != 0; break;
        }
        break;
      case Domain::Midi:
        switch (MidiOp(c.op)) {
          case MidiOp::SetChannel:
            if ((c.value & 15) != channel_) {
              all_notes_off();  // on the old channel, so nothing hangs
              channel_ = uint8_t(c.value & 15);
            }
            break;
          case MidiOp::SetKnobsRelative: knobs_relative_ = c.value != 0; break;
          case MidiOp::SetLrIsTransport: lr_transport_ = c.value != 0; break;
          case MidiOp::SetSyncIn: set_sync(c.value); break;
          case MidiOp::SetSyncOut: sync_out_ = c.value != 0; break;
          case MidiOp::AllNotesOff: all_notes_off(); break;
        }
        break;
      case Domain::Tape:
        if (TapeOp(c.op) == TapeOp::Play) request(Req::TapePlay);
        else if (TapeOp(c.op) == TapeOp::Stop) request(Req::TapeStop);
        break;
      case Domain::Seq: {
        Side& s = sides_[c.slot & 1];
        switch (SeqOp(c.op)) {
          case SeqOp::Select:
            if (c.value >= 0 && c.value < int(SeqType::Count) && SeqType(c.value) != s.type) {
              s.release_pending = true;
              s.release_type = s.type;
              s.type = SeqType(c.value);
            }
            break;
          case SeqOp::SetEnabled:
            if (s.enabled && c.value == 0) {
              s.release_pending = true;
              s.release_type = s.type;
            }
            s.enabled = c.value != 0;
            break;
          case SeqOp::Start: request(Req::SeqStart); break;  // sequencer transport only, not the tape
          case SeqOp::Stop: request(Req::SeqStop); break;
          case SeqOp::SetParam: s.engine(s.type).param(c.index, c.value); break;
          case SeqOp::Edit: s.engine(s.type).edit(c.index, c.value); break;
          case SeqOp::Encoder: s.engine(s.type).encoder(c.index & 7, c.value, out_[c.slot & 1]); break;
          case SeqOp::KeyEdit: s.engine(s.type).key_edit(c.index, c.value != 0, out_[c.slot & 1]); break;
          case SeqOp::Nav: s.engine(s.type).nav(c.index, c.value != 0, out_[c.slot & 1]); break;
          case SeqOp::SetParamOf:
          case SeqOp::EditOf: {
            const int t = (c.index >> 16) & 0xFF;
            if (t >= int(SeqType::Count)) break;
            Engine& en = s.engine(SeqType(t));
            if (SeqOp(c.op) == SeqOp::SetParamOf) en.param(c.index & 0xFFFF, c.value);
            else en.edit(c.index & 0xFFFF, c.value);
            break;
          }
        }
        break;
      }
      default: break;
    }
  }

  void event(const Event& e) noexcept override {
    if (npend_ < kPending) pending_[npend_++] = e;
    else ++dropped_;
  }

  // ------------------------------------------------------------ audio
  void process(const ProcessContext& ctx, EventSink& notes, EventSink& midi) noexcept override {
    notes_ = &notes;
    midi_ = &midi;
    offset_ = 0;
    for (auto& o : out_) o.tick = clock_.ticks();

    for (int s = 0; s < 2; ++s)
      if (sides_[s].release_pending) {
        sides_[s].release_pending = false;
        sides_[s].engine(sides_[s].release_type).release(out_[s]);
        release_gates(s);
      }
    for (int i = 0; i < nreq_; ++i) {
      switch (req_[i]) {
        case Req::TapePlay: start(true, false); break;
        case Req::TapeStop: stop(false); break;
        case Req::SeqStart: start_seq_only(); break;
        case Req::SeqStop: stop_seq_only(); break;
      }
    }
    nreq_ = 0;
    for (int i = 0; i < npend_; ++i) handle(pending_[i]);
    npend_ = 0;
    flush_step();

    if (ctx.block_start)
      for (int s = 0; s < 2; ++s)
        if (sides_[s].enabled) sides_[s].engine(sides_[s].type).control(out_[s]);

    for (int i = 0; i < ctx.frames; ++i)
      if (clock_.step()) {
        offset_ = i;
        on_tick(clock_.ticks() - 1);
      }
    for (auto& o : out_) o.tick = clock_.ticks();
    notes_ = midi_ = nullptr;
  }

  int16_t bpm_tenths() const noexcept override { return clock_.bpm_tenths(); }

  void view(SeqView& v, TempoView& t) const noexcept override {
    v.synth_seq = sides_[0].type;
    v.drum_seq = sides_[1].type;
    v.running = running_;
    v.step = int16_t(sides_[0].engine_c(sides_[0].type).step());
    for (int s = 0; s < 2; ++s) {
      int32_t* d = v.display + 32 * s;
      const Engine& e = sides_[s].engine_c(sides_[s].type);
      for (int k = 0; k < 32; ++k) d[k] = 0;
      d[0] = sides_[s].enabled;
      d[1] = int32_t(sides_[s].type);
      d[2] = e.step();
      d[3] = e.playing();
      e.view(d + 4);
      for (auto& p : v.params[s]) p = 0;
      e.get_params(v.params[s]);
    }
    t.bpm_tenths = clock_.bpm_tenths();
    t.metronome = metronome_;
    t.external_clock = clock_.external();
  }

  // ------------------------------------------------------------ used by SideOut
  void emit_on(int side, int note, int vel, int gate) noexcept {
    note = note < 0 ? 0 : (note > 127 ? 127 : note);
    Side& s = sides_[side];
    for (int g = 0; g < kGates; ++g)
      if (s.gates[g].note == note) emit_off(side, note);  // retrigger: end the sounding one first
    int slot = -1;
    for (int g = 0; g < kGates && slot < 0; ++g)
      if (s.gates[g].note < 0) slot = g;
    if (slot < 0) {  // more than kGates sounding: end the first one
      emit_off(side, s.gates[0].note);
      slot = 0;
    }
    s.gates[slot].note = int8_t(note);
    s.gates[slot].left = int16_t(gate > 0 ? gate : 0);
    Event e = note_on(uint8_t(note), uint8_t(vel < 1 ? 1 : (vel > 127 ? 127 : vel)), Source::Sequencer, 0,
                      side ? Target::Drum : Target::Synth);
    e.offset = offset_;
    push_note(e);
    send_note(e);
  }
  void emit_off(int side, int note) noexcept {
    for (auto& g : sides_[side].gates)
      if (g.note == note) g.note = -1;
    Event e = note_off(uint8_t(note & 127), Source::Sequencer, 0, side ? Target::Drum : Target::Synth);
    e.offset = offset_;
    push_note(e);
    send_note(e);
  }
  void emit_pass(int side, const Event& in) noexcept {
    Event e = in;
    e.target = side ? Target::Drum : Target::Synth;
    push_note(e);
    if (e.src != Source::Midi) send_note(e);  // played keys go out; MIDI input is never echoed
  }

 private:
  static constexpr int kGates = 32, kPending = 256, kReq = 16;
  enum class Req : uint8_t { TapePlay, TapeStop, SeqStart, SeqStop };
  struct Gate {
    int8_t note = -1;
    int16_t left = 0;  // ticks until note off; 0 = held until the engine turns it off
  };
  struct Side {
    SeqType type = SeqType::Finger;  // op1.db `sequencer`: both sides default to finger
    bool enabled = false;
    bool release_pending = false;
    SeqType release_type = SeqType::Finger;
    PatternSeq pattern;
    EndlessSeq endless;
    TombolaSeq tombola;
    FingerSeq finger;
    SketchSeq sketch;
    ArpSeq arp;
    Gate gates[kGates];
    Engine& engine(SeqType t) noexcept { return const_cast<Engine&>(engine_c(t)); }
    const Engine& engine_c(SeqType t) const noexcept {
      switch (t) {
        case SeqType::Pattern: return pattern;
        case SeqType::Endless: return endless;
        case SeqType::Tombola: return tombola;
        case SeqType::Sketch: return sketch;
        case SeqType::Arpeggio: return arp;
        default: return finger;
      }
    }
  };

  bool playing(int s) const noexcept { return sides_[s].enabled && sides_[s].engine_c(sides_[s].type).playing(); }
  void set_sync(int v) noexcept { clock_.set_external(v == 1 || v == 2 || v == 4); }
  void request(Req r) noexcept {
    if (nreq_ < kReq) req_[nreq_++] = r;
  }

  void handle(const Event& e) noexcept {
    switch (e.type) {
      case EventType::NoteOn:
      case EventType::NoteOff: {
        const int side = e.target == Target::Drum ? 1 : 0;
        if (sides_[side].enabled && e.type == EventType::NoteOn && e.b > 0 && !running_ && !clock_.external() &&
            !playing(0) && !playing(1)) {
          clock_.rephase();  // nothing uses the clock: start its grid at this key, so step 1 is on time
          for (auto& o : out_) o.tick = o.grid = clock_.ticks();
        }
        if (sides_[side].enabled) sides_[side].engine(sides_[side].type).key(e, out_[side]);
        else emit_pass(side, e);
        break;
      }
      case EventType::Clock:
        if (e.src == Source::Midi) clock_.midi_clock(e.time_ns);
        break;
      case EventType::Start:
        if (e.src == Source::Midi && clock_.external()) clock_.midi_start();
        start(true, e.src == Source::Midi);
        break;
      case EventType::Continue:
        start(false, e.src == Source::Midi);
        break;
      case EventType::Stop:
        stop(e.src == Source::Midi);
        break;
      case EventType::SongPosition:
        if (e.src == Source::Midi && clock_.external() && !running_) {
          const uint64_t back = uint64_t(e.value < 0 ? 0 : e.value) * kTicks16th;
          origin_ = clock_.ticks() - back;
          for_engines([&](Engine& en) { en.transport(false, true, origin_); });
          push_note(e);  // to the tape (the core forwards non-note events to Tape::transport)
        }
        break;
      case EventType::ControlChange:
      case EventType::PitchBend:
      case EventType::ProgramChange:
      case EventType::ChannelPressure:
        if (e.src != Source::Midi) send(e);  // UI / COM controller output
        break;
    }
  }

  template <typename F>
  void for_engines(F f) noexcept {
    for (auto& s : sides_) {
      f(s.pattern); f(s.endless); f(s.tombola); f(s.finger); f(s.sketch); f(s.arp);
    }
  }

  void start(bool restart, bool from_midi) noexcept {
    running_ = true;
    if (restart) {
      origin_ = clock_.ticks();
      if (!clock_.external()) clock_.rephase();
      for (auto& o : out_) o.grid = origin_;
    }
    for (auto& o : out_) o.running = true;
    for_engines([&](Engine& en) { en.transport(true, restart, origin_); });
    if (sync_out_) send_rt(restart ? EventType::Start : EventType::Continue);
    if (from_midi) push_transport(restart ? EventType::Start : EventType::Continue);
  }
  void stop(bool from_midi) noexcept {
    const bool was = running_;
    running_ = false;
    for (auto& o : out_) o.running = false;
    for_engines([&](Engine& en) { en.transport(false, false, origin_); });
    if (sync_out_ && (was || from_midi)) send_rt(EventType::Stop);
    if (from_midi) push_transport(EventType::Stop);
  }
  void start_seq_only() noexcept {
    running_ = true;
    origin_ = clock_.ticks();
    if (!clock_.external()) clock_.rephase();
    for_engines([&](Engine& en) { en.transport(true, true, origin_); });
  }
  void stop_seq_only() noexcept {
    running_ = false;
    for_engines([&](Engine& en) { en.transport(false, false, origin_); });
  }

  void on_tick(uint64_t t) noexcept {
    for (int s = 0; s < 2; ++s) {
      out_[s].tick = t;
      for (auto& g : sides_[s].gates)
        if (g.note >= 0 && g.left > 0 && --g.left == 0) emit_off(s, g.note);
      if (sides_[s].enabled) sides_[s].engine(sides_[s].type).tick(t, out_[s]);
    }
    if (running_ && sync_out_ && t >= origin_ && (t - origin_) % kMidiClockDiv == 0) send_rt(EventType::Clock);
  }

  void release_gates(int side) noexcept {
    for (auto& g : sides_[side].gates)
      if (g.note >= 0) emit_off(side, g.note);
  }

  // Snapshot the notes held on MIDI out now (on the current channel); flush_step() sends their offs.
  void all_notes_off() noexcept {
    for (int n = 0; n < 128; ++n) {
      flush_mask_[n] = flush_mask_[n] || held_out_[n];
      held_out_[n] = false;
    }
    flush_ = 0;
    flush_channel_ = channel_;
  }
  // Note Off velocity 64 per held note (firmware 0x019E2F84), at most 32 per chunk so the sink never fills.
  void flush_step() noexcept {
    if (flush_ < 0) return;
    int sent = 0;
    for (; flush_ < 128 && sent < 32; ++flush_)
      if (flush_mask_[flush_]) {
        Event e;
        e.type = EventType::NoteOff;
        e.channel = flush_channel_;
        e.a = uint8_t(flush_);
        e.b = 64;
        e.offset = offset_;
        if (!midi_->push(e)) break;  // sink full: the rest next chunk
        flush_mask_[flush_] = false;
        ++sent;
      }
    if (flush_ >= 128) flush_ = -1;
  }

  void push_note(const Event& e) noexcept {
    if (!notes_ || !notes_->push(e)) ++dropped_;
  }
  void push_transport(EventType t) noexcept {
    Event e;
    e.type = t;
    e.src = Source::Midi;
    push_note(e);
  }
  void send(Event e) noexcept {
    e.channel = channel_;
    if (e.offset < 0) e.offset = offset_;
    if (!midi_ || !midi_->push(e)) ++dropped_;
  }
  void send_note(const Event& in) noexcept {
    Event e = in;
    if (e.type == EventType::NoteOn && e.b > 0) {
      held_out_[e.a & 127] = true;
    } else {
      if (!held_out_[e.a & 127]) return;  // never sent: nothing to end
      held_out_[e.a & 127] = false;
      e.type = EventType::NoteOff;
      e.b = 64;  // the firmware's note-off velocity
    }
    send(e);
  }
  void send_rt(EventType t) noexcept {
    Event e;
    e.type = t;
    e.src = Source::Sequencer;
    send(e);
  }

  Side sides_[2];
  SideOut out_[2];
  TickClock clock_;
  Event pending_[kPending];
  int npend_ = 0;
  Req req_[kReq] = {};
  int nreq_ = 0;
  EventSink* notes_ = nullptr;
  EventSink* midi_ = nullptr;
  int offset_ = 0;
  bool running_ = false;
  uint64_t origin_ = kTickBase;
  bool sync_out_ = true, metronome_ = false, knobs_relative_ = false, lr_transport_ = false;
  uint8_t channel_ = 0, flush_channel_ = 0;  // op1.db midi_channel default 0
  bool held_out_[128] = {}, flush_mask_[128] = {};
  int flush_ = -1;
  uint32_t dropped_ = 0;
};

void SideOut::on(int note, int vel, int gate) noexcept { s->emit_on(side, note, vel, gate); }
void SideOut::off(int note) noexcept { s->emit_off(side, note); }
void SideOut::pass(const Event& e) noexcept { s->emit_pass(side, e); }

}  // namespace

std::unique_ptr<Sequencer> create_sequencer() { return std::make_unique<SequencerImpl>(); }

}  // namespace op1
