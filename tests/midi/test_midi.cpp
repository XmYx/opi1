// MIDI tests (owner: t5):
//   1. note -> engine routing through the whole core (MIDI in -> sequencer -> synth or drum instrument),
//   2. the realtime rule with tape, sequencers and MIDI out all running (no heap use in Core::process),
//   3. the COM-mode controller wire format (og-behaviour-details.md §2.1),
//   4. a live ALSA round trip through the real "OP-1 Midi" port: notes in reach the engine, notes, clock
//      and Start/Stop come out, MIDI clock in is followed. Skipped (and said so) without /dev/snd/seq.
#include <alsa/asoundlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "op1/core.h"
#include "op1/dsp.h"
#include "op1/io.h"
#include "op1/sequencer.h"
#include "op1/tape.h"
#include "../../src/midi/com_controller.h"

// ---- allocation counter (global operator new/delete for this test binary)
static std::atomic<bool> g_counting{false};
static std::atomic<long> g_allocs{0}, g_frees{0};
static void* counted_alloc(size_t n, size_t align) {
  if (g_counting) ++g_allocs;
  void* p = align > alignof(std::max_align_t) ? std::aligned_alloc(align, (n + align - 1) / align * align)
                                               : std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
static void counted_free(void* p) {
  if (p && g_counting) ++g_frees;
  std::free(p);
}
void* operator new(size_t n) { return counted_alloc(n, 0); }
void* operator new[](size_t n) { return counted_alloc(n, 0); }
void* operator new(size_t n, std::align_val_t a) { return counted_alloc(n, size_t(a)); }
void* operator new[](size_t n, std::align_val_t a) { return counted_alloc(n, size_t(a)); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, size_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { counted_free(p); }

using namespace op1;

namespace {

Command cmd(Domain d, uint16_t op, int32_t value, int32_t index = 0, uint8_t slot = 0) {
  Command c;
  c.domain = d; c.op = op; c.value = value; c.index = index; c.slot = slot;
  return c;
}

// Fixed buffers: this runs inside the allocation-counted region.
Sample g_l[256], g_r[256];
void periods(Core& core, int n, int frames = 64) {
  for (int i = 0; i < n; ++i) core.process(nullptr, nullptr, g_l, g_r, frames);
}

// ---------------------------------------------------------------- 1 + 2: routing and realtime rule
void test_routing_and_rt(Core& core) {
  periods(core, 4);
  CHECK_EQ(int(core.snapshot().synth.voices_active), 0);

  // MIDI note in, synth mode (Target::Auto): the synth plays, the drum does not
  CHECK(core.post_midi(note_on(60, 100, Source::Midi, 1)));
  periods(core, 4);
  const CoreSnapshot& a = core.snapshot();
  std::printf("synth mode: synth voices %02x, drum voices %02x\n", a.synth.voices_active, a.drum.voices_active);
  CHECK(a.synth.voices_active != 0);
  CHECK_EQ(int(a.drum.voices_active), 0);
  core.post_midi(note_off(60, Source::Midi, 2));

  // drum mode: the same MIDI note goes to the drum instrument. dbox (the drum synth) sounds without a kit
  // sample; the sampled drum kit needs the user's sample first.
  core.post_ui(cmd(Domain::Drum, uint16_t(InstrumentOp::SetType), int32_t(InstrumentType::Dbox)));
  core.post_ui(cmd(Domain::Core, uint16_t(CoreOp::SetMode), int32_t(Mode::Drum)));
  periods(core, 4);
  CHECK(core.post_midi(note_on(62, 100, Source::Midi, 3)));
  periods(core, 4);
  const CoreSnapshot& b = core.snapshot();
  std::printf("drum mode: synth voices %02x, drum voices %02x\n", b.synth.voices_active, b.drum.voices_active);
  CHECK(b.drum.voices_active != 0);
  core.post_midi(note_off(62, Source::Midi, 4));
  core.post_ui(cmd(Domain::Core, uint16_t(CoreOp::SetMode), int32_t(Mode::Synth)));
  periods(core, 4);

  // through a sequencer: arpeggio on the synth side turns a held MIDI note into notes on the engine
  core.post_ui(cmd(Domain::Seq, uint16_t(SeqOp::Select), int32_t(SeqType::Arpeggio), 0, 0));
  core.post_ui(cmd(Domain::Seq, uint16_t(SeqOp::SetEnabled), 1, 0, 0));
  periods(core, 1);
  CHECK(core.post_midi(note_on(64, 100, Source::Midi, 5)));
  periods(core, 4);
  CHECK(core.snapshot().synth.voices_active != 0);
  CHECK(core.snapshot().seq.display[3] == 1);  // the arpeggio is playing
  core.post_midi(note_off(64, Source::Midi, 6));
  core.post_ui(cmd(Domain::Seq, uint16_t(SeqOp::SetEnabled), 0, 0, 0));
  periods(core, 4);

  // realtime rule: tape recording and playing in a loop, a pattern running with MIDI clock out, keyboard
  // notes going out, engine notes from MIDI in, a lift and a drop: no heap use inside Core::process
  core.post_ui(cmd(Domain::Seq, uint16_t(SeqOp::Select), int32_t(SeqType::Pattern), 0, 1));
  core.post_ui(cmd(Domain::Seq, uint16_t(SeqOp::SetEnabled), 1, 0, 1));
  core.post_ui(cmd(Domain::Seq, uint16_t(SeqOp::Edit), 0, 0, 1));
  core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::SetLoopIn), 0));
  core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::SetLoopOut), 20000));
  core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::SetLoop), 1));
  core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::Record), 1));
  core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::Play), 0));
  periods(core, 2);
  Event drain;
  while (core.pop_midi_out(drain)) {}
  g_allocs = 0;
  g_frees = 0;
  g_counting = true;
  for (int i = 0; i < 3000; ++i) {
    if (i % 100 == 0) core.post_midi(note_on(uint8_t(60 + i % 12), 90, Source::Midi, 7));
    if (i % 100 == 50) core.post_ui(note_on(72, 100, Source::Keyboard, 8));
    if (i % 100 == 60) core.post_ui(note_off(72, Source::Keyboard, 9));
    if (i == 1500) core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::Lift), 0));
    if (i == 2500) core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::Drop), 0));
    periods(core, 1, i % 3 == 0 ? 48 : 32);
  }
  g_counting = false;
  int clocks = 0, notes = 0;
  while (core.pop_midi_out(drain)) {
    clocks += drain.type == EventType::Clock;
    notes += drain.type == EventType::NoteOn;
  }
  std::printf("realtime: %ld allocations, %ld frees in 3000 periods; %d clocks and %d notes sent; clipboard %d\n",
              g_allocs.load(), g_frees.load(), clocks, notes, core.snapshot().tape.display[1]);
  CHECK_EQ(g_allocs.load(), 0);
  CHECK_EQ(g_frees.load(), 0);
  CHECK(clocks > 50);
  CHECK(notes > 20);
  CHECK_EQ(core.snapshot().tape.display[1], 20000);  // the lift took the loop
  CHECK(core.snapshot().tape.recording);
  core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::Stop), 0));
  core.post_ui(cmd(Domain::Seq, uint16_t(SeqOp::SetEnabled), 0, 0, 1));
  periods(core, 4);
}

// ---------------------------------------------------------------- 3: COM controller
void test_com_controller() {
  midi::ComController com;
  Event e[midi::ComController::kMaxOut];
  // absolute: accumulator clamped 0..127
  CHECK_EQ(com.encoder(0, 5, e), 1);
  CHECK(e[0].type == EventType::ControlChange && e[0].a == 1 && e[0].b == 5);
  com.encoder(0, -20, e);
  CHECK_EQ(e[0].b, 0);
  com.encoder(3, 200, e);
  CHECK(e[0].a == 4 && e[0].b == 127);
  // relative: 7-bit two's complement
  com.set_relative(true);
  com.encoder(1, 1, e);
  CHECK(e[0].a == 2 && e[0].b == 1);
  com.encoder(1, -1, e);
  CHECK_EQ(e[0].b, 127);
  com.encoder(2, -3, e);
  CHECK(e[0].a == 3 && e[0].b == 125);
  // page: CC 53 + enc + 4 (page - 1)
  com.set_page(2);
  com.encoder(1, 1, e);
  CHECK_EQ(e[0].a, 58);
  // keys: CC code + 5, 127 / 0; track keys T1..T4 (codes 6..9) after CC 110 = 118
  CHECK_EQ(com.key(2, true, e), 1);  // synth key
  CHECK(e[0].a == 7 && e[0].b == 127);
  CHECK_EQ(com.key(2, false, e), 1);
  CHECK_EQ(e[0].b, 0);
  CHECK_EQ(com.key(6, true, e), 2);  // T1
  CHECK(e[0].a == 110 && e[0].b == 118 && e[1].a == 11 && e[1].b == 127);
  // note keys: (code - 13) + 53 + 12 octave at velocity 100
  com.set_octave(1);
  CHECK_EQ(com.key(13, true, e), 1);
  CHECK(e[0].type == EventType::NoteOn && e[0].a == 65 && e[0].b == 100);
  // program change: CC 110 = 118 first
  CHECK_EQ(com.program(7, e), 2);
  CHECK(e[0].a == 110 && e[0].b == 118 && e[1].type == EventType::ProgramChange && e[1].a == 7);
}

// ---------------------------------------------------------------- 4: live ALSA round trip
struct Probe {
  snd_seq_t* seq = nullptr;
  int port = -1;
  std::vector<snd_seq_event_t> got;
  bool open(int client, int their_port) {
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) return false;
    snd_seq_set_client_name(seq, "op1-og-test-probe");
    port = snd_seq_create_simple_port(seq, "probe",
                                      SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE |
                                          SND_SEQ_PORT_CAP_SUBS_WRITE,
                                      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    return snd_seq_connect_to(seq, port, client, their_port) >= 0 &&
           snd_seq_connect_from(seq, port, client, their_port) >= 0;
  }
  ~Probe() {
    if (seq) snd_seq_close(seq);
  }
  void send(snd_seq_event_t ev) {
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(seq, &ev);
  }
  void rt(int type) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    ev.type = snd_seq_event_type_t(type);
    send(ev);
  }
  void collect(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      snd_seq_event_t* ev = nullptr;
      if (snd_seq_event_input(seq, &ev) >= 0 && ev) got.push_back(*ev);
      else std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  }
  int count(int type) const {
    int n = 0;
    for (auto& e : got) n += e.type == type;
    return n;
  }
};

void test_alsa_round_trip(Core& core) {
  snd_seq_t* check = nullptr;
  if (snd_seq_open(&check, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
    std::printf("alsa round trip: SKIPPED (no ALSA sequencer on this machine)\n");
    return;
  }
  snd_seq_close(check);

  const std::string name = "OP-1 OG test " + std::to_string(getpid());
  auto midi = create_midi_io(name);
  std::string err;
  CHECK(midi->start(core, err));
  const std::string desc = midi->describe();
  std::printf("alsa: %s\n", desc.c_str());
  int client = -1, port = -1;
  std::sscanf(desc.c_str() + name.size(), " %d:%d", &client, &port);
  CHECK(client > 0);

  // a stand-in for the audio thread: 64-frame periods at about the real rate
  std::atomic<bool> run{true};
  std::thread audio([&] {
    std::vector<Sample> l(64), r(64);
    while (run) {
      core.process(nullptr, nullptr, l.data(), r.data(), 64);
      std::this_thread::sleep_for(std::chrono::microseconds(1450));
    }
  });

  Probe p;
  CHECK(p.open(client, port));
  // note in -> engine
  snd_seq_event_t ev;
  snd_seq_ev_clear(&ev);
  snd_seq_ev_set_noteon(&ev, 0, 60, 100);
  p.send(ev);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const uint8_t voices = core.snapshot().synth.voices_active;
  std::printf("alsa: note in -> synth voices %02x\n", voices);
  CHECK(voices != 0);
  snd_seq_ev_clear(&ev);
  snd_seq_ev_set_noteoff(&ev, 0, 60, 0);
  p.send(ev);

  // tape Play -> Start + clock out; a keyboard note -> note on/off (velocity 64) out; Stop -> Stop out
  core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::Play), 0));
  p.collect(300);
  core.post_ui(note_on(67, 100, Source::Keyboard, 1));
  p.collect(50);
  core.post_ui(note_off(67, Source::Keyboard, 2));
  p.collect(50);
  core.post_ui(cmd(Domain::Tape, uint16_t(TapeOp::Stop), 0));
  p.collect(100);
  int off64 = 0;
  for (auto& e : p.got) off64 += e.type == SND_SEQ_EVENT_NOTEOFF && e.data.note.note == 67 && e.data.note.velocity == 64;
  std::printf("alsa out: %d start, %d clock, %d note on, %d note off (vel 64), %d stop\n",
              p.count(SND_SEQ_EVENT_START), p.count(SND_SEQ_EVENT_CLOCK), p.count(SND_SEQ_EVENT_NOTEON), off64,
              p.count(SND_SEQ_EVENT_STOP));
  CHECK_EQ(p.count(SND_SEQ_EVENT_START), 1);
  CHECK(p.count(SND_SEQ_EVENT_CLOCK) >= 15);   // 50 per second at 125 BPM
  CHECK_EQ(p.count(SND_SEQ_EVENT_NOTEON), 1);  // the keyboard note; MIDI-in note 60 is not echoed
  CHECK_EQ(off64, 1);
  CHECK_EQ(p.count(SND_SEQ_EVENT_STOP), 1);

  // follow MIDI clock in: Start + 2 s of F8 at 100 BPM
  core.post_ui(cmd(Domain::Tempo, uint16_t(TempoOp::SetSync), 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  p.got.clear();
  p.rt(SND_SEQ_EVENT_START);
  const auto t0 = std::chrono::steady_clock::now();
  for (int k = 0; k < 80; ++k) {
    std::this_thread::sleep_until(t0 + std::chrono::microseconds(int64_t(k) * 25000));  // 100 BPM: 25 ms
    p.rt(SND_SEQ_EVENT_CLOCK);
    p.collect(1);
  }
  p.collect(50);
  const TempoView tv = core.snapshot().tempo;
  std::printf("alsa clock in: 80 F8 sent, %d clocks re-sent, tempo %.1f BPM, external %d\n",
              p.count(SND_SEQ_EVENT_CLOCK), tv.bpm_tenths / 10.0, tv.external_clock);
  CHECK(tv.external_clock);
  CHECK(std::abs(tv.bpm_tenths - 1000) <= 30);
  CHECK_EQ(p.count(SND_SEQ_EVENT_START), 1);   // FA re-emitted
  CHECK_EQ(p.count(SND_SEQ_EVENT_CLOCK), 80);  // one out per one in
  p.rt(SND_SEQ_EVENT_STOP);
  p.collect(30);

  run = false;
  audio.join();
  midi->stop();
}

}  // namespace

int main() {
  {
    Core core;
    test_routing_and_rt(core);
  }
  test_com_controller();
  {
    Core core;
    test_alsa_round_trip(core);
  }
  return check_result("midi");
}
