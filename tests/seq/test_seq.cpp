// Sequencer tests (owner: t5): clock sync drift (internal and following MIDI clock in), transport and song
// position rules, MIDI out (channel, note-off velocity 64, all-notes-off, no echo), and each sequencer.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "check.h"
#include "op1/io.h"
#include "op1/sequencer.h"
#include "op1/tape.h"

using namespace op1;

namespace {

constexpr int kRest = -32768;  // endless rest step (src/seq/seq_engines.h)

struct Out {
  Event e;
  int64_t frame;  // absolute frame of the event (chunk start + offset)
};

// Drives a Sequencer like the core does: chunks on the 64-frame grid.
struct Rig {
  std::unique_ptr<Sequencer> s = create_sequencer();
  EventSink notes, midi;
  int64_t frame = 0;
  std::vector<Out> got_notes, got_midi;

  void cmd(Domain d, uint16_t op, int32_t value, int32_t index = 0, uint8_t slot = 0) {
    Command c;
    c.domain = d; c.op = op; c.value = value; c.index = index; c.slot = slot;
    s->command(c);
  }
  void ev(Event e) { s->event(e); }
  void run(int64_t frames, int chunk = 64) {
    while (frames > 0) {
      const int to_grid = kBlock - int(frame % kBlock);
      const int n = int(std::min<int64_t>(frames, std::min(chunk, to_grid)));
      ProcessContext ctx;
      ctx.frame = frame;
      ctx.frames = n;
      ctx.block_start = frame % kBlock == 0;
      ctx.bpm_tenths = s->bpm_tenths();
      notes.clear();
      midi.clear();
      s->process(ctx, notes, midi);
      for (int k = 0; k < notes.n; ++k)
        got_notes.push_back({notes.ev[k], frame + (notes.ev[k].offset < 0 ? 0 : notes.ev[k].offset)});
      for (int k = 0; k < midi.n; ++k)
        got_midi.push_back({midi.ev[k], frame + (midi.ev[k].offset < 0 ? 0 : midi.ev[k].offset)});
      frame += n;
      frames -= n;
    }
  }
  int count_midi(EventType t) const {
    int n = 0;
    for (auto& o : got_midi) n += o.e.type == t;
    return n;
  }
  std::vector<Out> note_ons() const {
    std::vector<Out> v;
    for (auto& o : got_notes)
      if (o.e.type == EventType::NoteOn && o.e.b > 0) v.push_back(o);
    return v;
  }
};

Event midi_rt(EventType t, int64_t ns = 0) {
  Event e;
  e.type = t;
  e.src = Source::Midi;
  e.time_ns = ns;
  return e;
}

// ---------------------------------------------------------------- clock drift, internal clock
void test_internal_clock_drift() {
  for (int bpm_tenths : {1250, 1200, 999, 2200}) {
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), bpm_tenths);
    r.cmd(Domain::Tape, uint16_t(TapeOp::Play), 0);  // transport start: MIDI Start + clock out
    const int64_t frames = int64_t(kSampleRate) * 600;  // 10 minutes
    r.run(frames, 32);
    CHECK_EQ(r.count_midi(EventType::Start), 1);
    // MIDI clock k (24 PPQN) belongs at k * 44100 * 60 / (bpm * 24) samples after the start.
    const double spc = 44100.0 * 60.0 * 10.0 / (bpm_tenths * 24.0);
    int k = 0;
    double worst = 0;
    for (auto& o : r.got_midi) {
      if (o.e.type != EventType::Clock) continue;
      worst = std::max(worst, std::fabs(double(o.frame) - k * spc));
      ++k;
    }
    const int expect = int(double(frames) / spc) + 1;
    std::printf("internal %5.1f BPM: %d clocks in 10 min (expected %d), worst offset %.2f samples\n",
                bpm_tenths / 10.0, k, expect, worst);
    CHECK(std::abs(k - expect) <= 1);
    CHECK(worst < 1.0);  // no drift: every clock within one sample of its ideal time
  }
}

// ---------------------------------------------------------------- clock drift, following MIDI clock in
void test_external_clock_follow() {
  Rig r;
  r.cmd(Domain::Tempo, uint16_t(TempoOp::SetSync), 4);  // firmware sync type 4 = external
  r.ev(midi_rt(EventType::Start, 1));
  r.run(64);
  // F8s from a sender whose tempo ramps 118 -> 123 BPM over 10 minutes, delivered at chunk boundaries
  // (up to 32 frames late) with +-0.5 ms timestamp jitter.
  const int64_t frames = int64_t(kSampleRate) * 600;
  double t = 0;  // seconds of the next F8
  int f8 = 0, max_lag = 0;
  uint32_t lcg = 7;
  while (r.frame < frames) {
    const double now = double(r.frame) / kSampleRate;
    while (t <= now) {
      const double bpm = 118.0 + 5.0 * (t / 600.0);
      lcg = lcg * 1664525u + 1013904223u;
      const double jitter = (double(lcg >> 16) / 65536.0 - 0.5) * 0.001;
      r.ev(midi_rt(EventType::Clock, int64_t((t + jitter) * 1e9) + 1000000000));
      ++f8;
      t += 60.0 / (bpm * 24.0);
    }
    r.run(32, 32);
    max_lag = std::max(max_lag, std::abs(f8 - r.count_midi(EventType::Clock)));
  }
  TempoView tv;
  SeqView sv;
  r.s->view(sv, tv);
  const int out = r.count_midi(EventType::Clock);
  std::printf("external: %d F8 in, %d clocks out, max lag %d, tempo estimate %.1f BPM (sender at end 123.0)\n",
              f8, out, max_lag, tv.bpm_tenths / 10.0);
  CHECK(tv.external_clock);
  CHECK_EQ(out, f8);  // slaved: one clock out per clock in, none gained or lost over 10 min
  CHECK(max_lag <= 1);
  CHECK(std::abs(tv.bpm_tenths - 1230) <= 10);
  CHECK_EQ(r.count_midi(EventType::Start), 1);  // FA re-emitted
}

// ---------------------------------------------------------------- transport and song position
void test_transport_rules() {
  Rig r;
  // internal sync: Song Position is ignored (the firmware acts on it only for sync types 2/4)
  Event spp = midi_rt(EventType::SongPosition);
  spp.value = 16;
  r.ev(spp);
  r.run(64);
  int spp_to_tape = 0;
  for (auto& o : r.got_notes) spp_to_tape += o.e.type == EventType::SongPosition;
  CHECK_EQ(spp_to_tape, 0);

  r.cmd(Domain::Tempo, uint16_t(TempoOp::SetSync), 2);
  r.ev(spp);
  r.run(64);
  for (auto& o : r.got_notes) spp_to_tape += o.e.type == EventType::SongPosition;
  CHECK_EQ(spp_to_tape, 1);

  // MIDI Start/Stop reach the tape (note sink) and are re-emitted on MIDI out; SPP while running is ignored
  r.ev(midi_rt(EventType::Start));
  r.run(64);
  r.ev(spp);
  r.ev(midi_rt(EventType::Stop));
  r.run(64);
  int start_tape = 0, stop_tape = 0, spp2 = 0;
  for (auto& o : r.got_notes) {
    start_tape += o.e.type == EventType::Start;
    stop_tape += o.e.type == EventType::Stop;
    spp2 += o.e.type == EventType::SongPosition;
  }
  CHECK_EQ(start_tape, 1);
  CHECK_EQ(stop_tape, 1);
  CHECK_EQ(spp2, 1);
  CHECK_EQ(r.count_midi(EventType::Start), 1);
  CHECK_EQ(r.count_midi(EventType::Stop), 1);

  // sync out off: nothing realtime goes out
  Rig q;
  q.cmd(Domain::Tempo, uint16_t(TempoOp::SetSyncOut), 0);
  q.cmd(Domain::Tape, uint16_t(TapeOp::Play), 0);
  q.run(44100);
  CHECK_EQ(q.count_midi(EventType::Clock), 0);
  CHECK_EQ(q.count_midi(EventType::Start), 0);
}

// ---------------------------------------------------------------- MIDI out rules
void test_midi_out() {
  Rig r;
  r.cmd(Domain::Midi, uint16_t(MidiOp::SetChannel), 5);
  // keyboard notes pass through (same chunk) and go out on channel 5; MIDI-in notes are not echoed
  r.ev(note_on(60, 100, Source::Keyboard, 1, Target::Synth));
  r.ev(note_on(62, 90, Source::Keyboard, 1, Target::Drum));
  r.ev(note_on(64, 80, Source::Midi, 1, Target::Synth));
  r.run(64);
  CHECK_EQ(int(r.note_ons().size()), 3);
  if (r.note_ons().size() == 3) CHECK(r.note_ons()[1].e.target == Target::Drum);
  CHECK_EQ(r.count_midi(EventType::NoteOn), 2);
  for (auto& o : r.got_midi) CHECK_EQ(o.e.channel, 5);
  // a note off goes out as 0x80 velocity 64
  r.ev(note_off(60, Source::Keyboard, 2, Target::Synth));
  r.run(64);
  bool off64 = false;
  for (auto& o : r.got_midi) off64 |= o.e.type == EventType::NoteOff && o.e.a == 60 && o.e.b == 64;
  CHECK(off64);
  // all notes off: Note Off velocity 64 for every held note (here 62), nothing else (no CC 123)
  r.got_midi.clear();
  r.cmd(Domain::Midi, uint16_t(MidiOp::AllNotesOff), 0);
  r.run(64);
  CHECK_EQ(int(r.got_midi.size()), 1);
  if (!r.got_midi.empty())
    CHECK(r.got_midi[0].e.type == EventType::NoteOff && r.got_midi[0].e.a == 62 && r.got_midi[0].e.b == 64);
  // controller messages from the UI (COM mode) go out; the same from MIDI in do not
  r.got_midi.clear();
  Event cc;
  cc.type = EventType::ControlChange; cc.src = Source::Keyboard; cc.a = 1; cc.b = 1;
  r.ev(cc);
  cc.src = Source::Midi;
  r.ev(cc);
  r.run(64);
  CHECK_EQ(r.count_midi(EventType::ControlChange), 1);
}

// ---------------------------------------------------------------- sequencers
void enable(Rig& r, int side, SeqType t) {
  r.cmd(Domain::Seq, uint16_t(SeqOp::Select), int32_t(t), 0, uint8_t(side));
  r.cmd(Domain::Seq, uint16_t(SeqOp::SetEnabled), 1, 0, uint8_t(side));
}

// The tests below follow the firmware's players (src/seq/seq_engines.h, decoded from R.00246): at 120 BPM a
// sixteenth (24 ticks) is 5512.5 samples, an eighth 11025.
std::vector<int> notes_of(const Rig& r) {
  std::vector<int> v;
  for (auto& o : r.note_ons()) v.push_back(o.e.a);
  return v;
}

void test_pattern() {
  Rig r;
  r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
  enable(r, 1, SeqType::Pattern);
  r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 0, 0, 1);  // step 0: key 0 (note 53)
  r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 2, 4, 1);  // step 4: key 2 (note 55)
  r.cmd(Domain::Tape, uint16_t(TapeOp::Play), 0);       // the transport alone does not play a pattern
  r.run(20000);
  CHECK_EQ(int(r.note_ons().size()), 0);
  r.cmd(Domain::Tape, uint16_t(TapeOp::Stop), 0);
  r.run(64);
  // holding a key plays it: step 0 at once (nothing else runs), step 4 a quarter later, step 0 again a bar later
  const int64_t t0 = r.frame;
  r.ev(note_on(60, 100, Source::Keyboard, 1, Target::Drum));
  r.run(int64_t(kSampleRate) * 2 + 100);
  auto on = r.note_ons();
  CHECK_EQ(int(on.size()), 3);
  if (on.size() == 3) {
    CHECK_EQ(on[0].e.a, 53);
    CHECK(on[0].e.target == Target::Drum);
    CHECK(std::llabs(on[0].frame - t0) <= 1);
    CHECK(std::llabs(on[1].frame - t0 - 22050) <= 1);
    CHECK_EQ(on[1].e.a, 55);
    CHECK(std::llabs(on[2].frame - t0 - 88200) <= 1);
    CHECK_EQ(on[0].e.b, 127);  // the firmware's sequencer velocity
  }
  int offs = 0;
  for (auto& o : r.got_notes) offs += o.e.type == EventType::NoteOff;
  CHECK(offs >= 2);  // the gate is one step
  r.ev(note_off(60, Source::Keyboard, 2, Target::Drum));
  const size_t n = r.note_ons().size();
  r.run(44100);
  CHECK_EQ(r.note_ons().size(), n);  // released, not latched: stops

  // synth side: transposed by the held key - C4 (holding D4 plays a tone up); pm 0 = rev, 1 = cycle
  Rig q;
  q.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
  enable(q, 0, SeqType::Pattern);
  q.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), 3, 0, 0);  // length 3
  for (int st = 0; st < 3; ++st) q.cmd(Domain::Seq, uint16_t(SeqOp::Edit), st, st, 0);  // key 0, 1, 2
  q.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), 1, 1, 0);  // pm cycle
  q.ev(note_on(62, 100, Source::Keyboard, 1, Target::Synth));
  q.run(5513 * 6 - 100);
  CHECK(notes_of(q) == (std::vector<int>{55, 56, 57, 56, 55, 56}));  // ping-pong, ends not repeated, +2
}

void test_endless_arp_finger_sketch_tombola() {
  {  // endless: keyboard-key steps, transposed by the held key - C4, 1/16 (24 ticks)
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
    enable(r, 0, SeqType::Endless);
    r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 7 | (1 << 8), 2, 0);   // key 7 = C4
    r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 14 | (1 << 8), 2, 0);  // G4
    r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), kRest, 0, 0);          // a rest
    r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 19 | (1 << 8), 2, 0);  // C5
    r.ev(note_on(60, 100, Source::Keyboard, 1, Target::Synth));
    r.run(5513 * 5 - 100);
    CHECK(notes_of(r) == (std::vector<int>{60, 67, 72, 60}));
    r.ev(note_off(60, Source::Keyboard, 2, Target::Synth));
    const size_t before = r.note_ons().size();
    r.run(44100);
    CHECK_EQ(r.note_ons().size(), before);  // released: stops
  }
  {  // arpeggio: the order the keys were pressed, octave mode "once" (factory) adds the last note +12, 1/8
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
    enable(r, 0, SeqType::Arpeggio);
    r.ev(note_on(67, 100, Source::Keyboard, 1, Target::Synth));
    r.ev(note_on(60, 100, Source::Keyboard, 1, Target::Synth));
    r.ev(note_on(64, 100, Source::Keyboard, 1, Target::Synth));
    r.run(11025 * 8 - 10);
    CHECK(notes_of(r) == (std::vector<int>{67, 60, 64, 76, 67, 60, 64, 76}));
  }
  {  // finger: slot 0 = the lowest white key (F); a black key does nothing
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
    enable(r, 1, SeqType::Finger);
    r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 53, 0 * 64 + 0 * 2 + 0, 1);  // slot 0 step 0
    r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 57, 0 * 64 + 2 * 2 + 1, 1);  // slot 0 step 2, second hand
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), 4, 2, 1);                // slot 0 length 4
    r.ev(note_on(53, 100, Source::Keyboard, 1, Target::Drum));
    r.ev(note_on(70, 100, Source::Keyboard, 1, Target::Drum));  // key 17 = a black key
    r.run(5513 * 8 - 100);
    CHECK(notes_of(r) == (std::vector<int>{53, 57, 53, 57}));
  }
  {  // sketch: a drawn line plays while a key is held; a flat line is one held note
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
    enable(r, 0, SeqType::Sketch);
    for (int p = 0; p < 18; ++p) r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 0, p, 0);
    for (int p = 18; p < 36; ++p) r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 5, p, 0);
    r.ev(note_on(60, 100, Source::Keyboard, 1, Target::Synth));
    r.run(5513 * 2);
    CHECK(notes_of(r) == (std::vector<int>{53, 58}));
  }
  {  // tombola: a ball dropped in the hexagon hits the walls and plays its note
    Rig r;
    enable(r, 0, SeqType::Tombola);
    r.ev(note_on(72, 100, Source::Keyboard, 1, Target::Synth));
    r.run(44100 * 3);
    int n72 = 0;
    for (auto& o : r.note_ons()) n72 += o.e.a == 72;
    CHECK(n72 >= 1);
    SeqView sv;
    TempoView tv;
    r.s->view(sv, tv);
    CHECK_EQ(sv.display[4 + 1], 1);  // one ball
  }
}

// Fixed inputs -> the note sequences the firmware's players produce (decode notes, seq_engines.h)
void dump(const char* what, const std::vector<int>& v) {
  std::printf("%s:", what);
  for (int n : v) std::printf(" %d", n);
  std::printf("\n");
}

void test_firmware_sequences() {
  auto arp = [](int octave, int op, int skip, int sm) {
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
    enable(r, 0, SeqType::Arpeggio);
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), octave, 1, 0);
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), op, 0, 0);
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), skip, 5, 0);
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), sm, 6, 0);
    for (int n : {60, 64, 67}) r.ev(note_on(uint8_t(n), 100, Source::Keyboard, 1, Target::Synth));
    r.run(11025 * 8 - 10);
    if (std::getenv("SEQ_DUMP")) dump("arp", notes_of(r));
    return notes_of(r);
  };
  CHECK(arp(0, 0, 1, 0) == (std::vector<int>{60, 64, 67, 60, 64, 67, 60, 64}));  // none
  CHECK(arp(1, 0, 1, 0) == (std::vector<int>{60, 64, 67, 79, 60, 64, 67, 79}));  // once
  CHECK(arp(2, 0, 1, 0) == (std::vector<int>{60, 72, 64, 76, 67, 79, 60, 72}));  // each
  CHECK(arp(1, 3, 1, 0) == (std::vector<int>{60, 64, 67, 69, 60, 64, 67, 71}));  // once, op 3: +2, +4, ...
  // skip 4 = mask 12 = 1100, read MSB first, one bit per step. sm 0 (skip): after each note the order advances,
  // and while the next bit is 0 it advances again in the same tick (no gap); sm 1 (pause): a 0 bit is a rest
  CHECK(arp(0, 0, 4, 0) == (std::vector<int>{60, 64, 67, 67, 60, 60, 64, 64}));
  CHECK(arp(0, 0, 4, 1) == (std::vector<int>{60, 64, 67, 60}));

  // arpeggio hold (orange right): keeps playing after release; a new chord replaces it
  {
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
    enable(r, 0, SeqType::Arpeggio);
    r.cmd(Domain::Seq, uint16_t(SeqOp::Encoder), 1, 3, 0);
    r.ev(note_on(60, 100, Source::Keyboard, 1, Target::Synth));
    r.run(64);
    r.ev(note_off(60, Source::Keyboard, 2, Target::Synth));
    r.run(11025 * 3 - 10);
    CHECK_EQ(int(r.note_ons().size()), 4);  // 0, 1/8, 2/8 and 3/8 after the key, all after its release
  }

  // finger play modes: slot 0 = key 0 plays 53 x 2 steps, slot 1 = key 2 plays 55 x 2 steps
  auto finger = [](int pm) {
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
    enable(r, 0, SeqType::Finger);
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), pm, 0, 0);
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), 2, 2, 0);  // slot 0 length 2
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), 2, 3, 0);  // slot 1 length 2
    for (int st = 0; st < 2; ++st) {
      r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 53 + st, 0 * 64 + st * 2, 0);
      r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), 60 + st, 1 * 64 + st * 2, 0);
    }
    r.ev(note_on(53, 100, Source::Keyboard, 1, Target::Synth));
    r.run(64);
    r.ev(note_on(55, 100, Source::Keyboard, 1, Target::Synth));
    r.run(5513 * 6 - 200);
    if (std::getenv("SEQ_DUMP")) dump("finger", notes_of(r));
    return notes_of(r);
  };
  // A starts at once; B (pressed 64 samples later) joins from its own step 0 at the next step
  CHECK(finger(1) == (std::vector<int>{53, 54, 60, 53, 61, 54, 60, 53, 61, 54, 60}));  // join: both
  CHECK(finger(0) == (std::vector<int>{53, 54, 60, 61, 60, 61}));                          // replace: B after A
  CHECK(finger(2) == (std::vector<int>{53, 60, 61, 54, 53, 54}));  // fill in: B once while A pauses

  // endless euclidean 3 (x.x..): play, rest, play, rest, rest
  {
    Rig r;
    r.cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 1200);
    enable(r, 0, SeqType::Endless);
    for (int k : {7, 9, 11}) r.cmd(Domain::Seq, uint16_t(SeqOp::Edit), k | (1 << 8), 2, 0);
    r.cmd(Domain::Seq, uint16_t(SeqOp::SetParam), 3, 2, 0);
    r.ev(note_on(60, 100, Source::Keyboard, 1, Target::Synth));
    r.run(5513 * 10 - 100);
    CHECK(notes_of(r) == (std::vector<int>{60, 62, 64, 60}));
  }

  // step entry: shift + key on the pattern page toggles that key at the cursor and moves the cursor on
  {
    Rig r;
    enable(r, 1, SeqType::Pattern);
    r.cmd(Domain::Seq, uint16_t(SeqOp::KeyEdit), 1, 3, 1);
    r.cmd(Domain::Seq, uint16_t(SeqOp::KeyEdit), 0, 3, 1);
    r.cmd(Domain::Seq, uint16_t(SeqOp::KeyEdit), 1, 5, 1);
    r.cmd(Domain::Seq, uint16_t(SeqOp::KeyEdit), 0, 5, 1);
    SeqView sv;
    TempoView tv;
    r.s->view(sv, tv);
    CHECK_EQ(sv.display[32 + 4 + 1], 1 << 3);  // step 0: key 3
    CHECK_EQ(sv.display[32 + 4 + 2], 1 << 5);  // step 1: key 5
  }
}

}  // namespace

int main() {
  test_internal_clock_drift();
  test_external_clock_follow();
  test_transport_rules();
  test_midi_out();
  test_pattern();
  test_endless_arp_finger_sketch_tombola();
  test_firmware_sequences();
  return check_result("seq");
}
