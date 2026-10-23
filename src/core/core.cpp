// Core: queues, routing, the 64-frame chunk grid, engine switching and the screen snapshot.
// Owner: t3. Everything reachable from process() follows the realtime rules in architecture.md.
#include "op1/core.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <new>
#include <stdexcept>

#include "op1/dsp.h"
#include "op1/fract.h"
#include "op1/sequencer.h"
#include "op1/spsc.h"
#include "op1/tape.h"

namespace op1 {
namespace {

int64_t now_ns() noexcept {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// Storage for one instrument slot. Allocated and zeroed (so prefaulted) once at startup; engines are
// placement-constructed into it, on the audio thread when the user switches engine.
struct Slot {
  std::byte* mem = nullptr;
  Instrument* inst = nullptr;
  const SampleData* sample = nullptr;  // current SampleData, retired when replaced
  Domain domain;
  Slot(InstrumentType t, Domain d) : domain(d) {
    mem = static_cast<std::byte*>(::operator new(kInstrumentStorage, std::align_val_t(64)));
    std::memset(mem, 0, kInstrumentStorage);
    inst = create_instrument(t, mem, kInstrumentStorage);
  }
  ~Slot() {
    if (inst) inst->~Instrument();
    ::operator delete(mem, std::align_val_t(64));
  }
  Slot(const Slot&) = delete;
  Slot& operator=(const Slot&) = delete;
  // Audio thread. Keeps the old engine if the new one cannot be built.
  void switch_to(InstrumentType t) noexcept {
    if (inst && inst->type() == t) return;
    const InstrumentType old = inst ? inst->type() : t;
    if (inst) inst->~Instrument();
    inst = create_instrument(t, mem, kInstrumentStorage);
    if (!inst) inst = create_instrument(old, mem, kInstrumentStorage);
    // A fresh engine starts without a sample: hand it the one this slot still owns (not retired), so
    // switching away from the sampler and back keeps the user's sample (reported by t4).
    if (inst && sample) {
      Command c;
      c.domain = domain;
      c.op = uint16_t(InstrumentOp::SetSample);
      c.ptr = const_cast<SampleData*>(sample);
      inst->command(c);
    }
  }
};

}  // namespace

struct Core::Impl {
  // ---- queues (one producer, one consumer each)
  SpscQueue<Event, 1024> ui_events, midi_events, midi_out;
  SpscQueue<Command, 1024> ui_commands, worker_commands, retired;
  SpscQueue<NoteTiming, 4096> note_timing;
  TripleBuffer<CoreSnapshot> snap;

  // ---- modules
  Slot synth{InstrumentType::Fm, Domain::Synth};
  Slot drum{InstrumentType::Drum, Domain::Drum};
  std::unique_ptr<Sequencer> seq = create_sequencer();
  std::unique_ptr<Tape> tape = create_tape();
  std::unique_ptr<Mixer> mixer = create_mixer();

  // ---- audio-thread state
  Mode mode = Mode::Synth;
  int64_t frame = 0;
  int64_t dac_ns = 0, chunk_dac_ns = 0;
  int32_t input_gain = 0;  // Q1.15; input monitoring is off until the UI turns it on (no mic feedback)
  int32_t out_volume = 32767;  // Q1.15, the volume knob (CoreOp::SetOutputVolume); full = untouched
  ControlInputs controls;
  EngineStats stats;
  Sample scope[kDisplayW] = {};
  int scope_pos = 0;
  EventSink notes, midi_send;
  Sample zero[kBlock] = {}, in_l[kBlock] = {}, in_r[kBlock] = {};
  bool tape_was_playing = false;  // t5: the tape stopping by itself stops the sequencer transport
  Sample syn_l[kBlock] = {}, syn_r[kBlock] = {}, drm_l[kBlock] = {}, drm_r[kBlock] = {};
  Sample live_l[kBlock] = {}, live_r[kBlock] = {}, rec[kBlock] = {};
  Sample trk[kTapeTracks][kBlock] = {};

  Slot& slot_for(Domain d) noexcept { return d == Domain::Drum ? drum : synth; }

  void handle_event(Event e) noexcept {
    switch (e.type) {
      case EventType::NoteOn:
      case EventType::NoteOff:
        if (e.target == Target::Auto) e.target = mode == Mode::Drum ? Target::Drum : Target::Synth;
        seq->event(e);
        break;
      case EventType::ControlChange:
        controls.cc[e.a & 127] = e.b;
        seq->event(e);  // t5: the sequencer owns MIDI out (UI/COM-mode controllers are sent from there)
        break;
      case EventType::PitchBend:
        controls.pitch_bend = e.value;
        seq->event(e);
        break;
      default:  // clock, transport, song position: the sequencer/tempo owner decides
        seq->event(e);
        break;
    }
  }

  void handle_command(const Command& c, Core& core) noexcept {
    switch (c.domain) {
      case Domain::Core:
        switch (CoreOp(c.op)) {
          case CoreOp::SetMode: mode = Mode(c.value); break;
          case CoreOp::AllNotesOff: synth.inst->all_notes_off(); drum.inst->all_notes_off(); break;
          case CoreOp::SetInputGain: input_gain = std::clamp(c.value, 0, 32767); break;
          case CoreOp::SetOutputVolume: out_volume = std::clamp(c.value, 0, 32767); break;
        }
        break;
      case Domain::Synth:
      case Domain::Drum: {
        Slot& s = slot_for(c.domain);
        const auto op = InstrumentOp(c.op);
        if (op == InstrumentOp::SetType) {
          const auto t = InstrumentType(c.value);
          if (is_drum(t) == (c.domain == Domain::Drum)) s.switch_to(t);  // the synth factory rejects drums
        } else if (op == InstrumentOp::LoadSynthPatch && c.ptr) {
          const auto* p = static_cast<const SynthPatch*>(c.ptr);
          if (c.domain == Domain::Synth && !is_drum(p->type)) s.switch_to(p->type);
          s.inst->command(c);
          core.retire(c);
        } else if (op == InstrumentOp::LoadDrumPatch && c.ptr) {
          const auto* p = static_cast<const DrumPatch*>(c.ptr);
          if (c.domain == Domain::Drum && is_drum(p->type)) s.switch_to(p->type);
          s.inst->command(c);
          core.retire(c);
        } else if (op == InstrumentOp::SetSample) {
          s.inst->command(c);
          if (s.sample) {
            Command old = c;
            old.ptr = const_cast<SampleData*>(s.sample);
            core.retire(old);
          }
          s.sample = static_cast<const SampleData*>(c.ptr);
        } else {
          s.inst->command(c);
        }
        break;
      }
      case Domain::Tape:
        tape->command(c);
        seq->command(c);  // t5: tape play/stop is the transport that runs sequencers and MIDI clock out
        break;
      case Domain::Mixer: mixer->command(c); break;
      case Domain::Seq:
      case Domain::Tempo:
      case Domain::Midi: seq->command(c); break;  // t5: MIDI settings act on MIDI out, which the sequencer builds
      case Domain::Count: break;
    }
  }

  // One chunk: n frames (1..kBlock) that do not cross the 64-frame grid. `at` is the chunk's offset in
  // the caller's buffers.
  void render_chunk(const Sample* il, const Sample* ir, Sample* out_l, Sample* out_r, int n, int at) noexcept {
    // live input
    if (il) {
      int32_t peak = 0;
      for (int i = 0; i < n; ++i) {
        in_l[i] = il[at + i];
        in_r[i] = ir ? ir[at + i] : il[at + i];
        peak = std::max(peak, bf::abs_fr1x32(in_l[i]));
      }
      controls.input_level = peak;
    }
    ProcessContext ctx;
    ctx.frame = frame;
    ctx.frames = n;
    ctx.block_start = frame % kBlock == 0;
    ctx.bpm_tenths = seq->bpm_tenths();
    ctx.in_l = il ? in_l : zero;
    ctx.in_r = il ? in_r : zero;
    ctx.controls = &controls;
    chunk_dac_ns = dac_ns + int64_t(at) * 1000000000 / kSampleRate;

    // notes: sequencer first (it passes played notes through when idle), then the instruments
    notes.clear();
    midi_send.clear();
    seq->process(ctx, notes, midi_send);
    for (int k = 0; k < notes.n; ++k) {
      const Event& e = notes.ev[k];
      Instrument* inst = e.target == Target::Drum ? drum.inst : synth.inst;
      const int off = e.offset >= 0 && e.offset < n ? e.offset : -1;
      if (e.type == EventType::NoteOn && e.b > 0) {
        inst->note_on(e.a, e.b, off);
        if (e.time_ns > 0)
          note_timing.push(NoteTiming{e.time_ns, chunk_dac_ns + int64_t(off < 0 ? 0 : off) * 1000000000 / kSampleRate,
                                      e.a, e.src});
      } else if (e.type == EventType::NoteOn || e.type == EventType::NoteOff) {
        inst->note_off(e.a, off);
      } else {
        tape->transport(e, ctx.bpm_tenths);  // t5: MIDI Start/Continue/Stop/Song Position move the tape
      }
    }
    for (int k = 0; k < midi_send.n; ++k) midi_out.push(midi_send.ev[k]);

    synth.inst->process(ctx, syn_l, syn_r);
    drum.inst->process(ctx, drm_l, drm_r);
    for (int i = 0; i < n; ++i) {
      live_l[i] = bf::add_fr1x32(syn_l[i], drm_l[i]);
      live_r[i] = bf::add_fr1x32(syn_r[i], drm_r[i]);
    }
    if (il && input_gain > 0) {
      for (int i = 0; i < n; ++i) {
        live_l[i] = bf::add_fr1x32(live_l[i], bf::sat32((int64_t(in_l[i]) * input_gain) >> 15));
        live_r[i] = bf::add_fr1x32(live_r[i], bf::sat32((int64_t(in_r[i]) * input_gain) >> 15));
      }
    }
    for (int i = 0; i < n; ++i) rec[i] = Sample((int64_t(live_l[i]) + live_r[i]) >> 1);

    Sample* tracks[kTapeTracks] = {trk[0], trk[1], trk[2], trk[3]};
    tape->process(ctx, rec, tracks);
    const Sample* ctracks[kTapeTracks] = {trk[0], trk[1], trk[2], trk[3]};
    mixer->process(ctx, ctracks, live_l, live_r, out_l + at, out_r + at);

    for (int i = 0; i < n; ++i) {
      scope[scope_pos] = out_l[at + i];
      scope_pos = (scope_pos + 1) % kDisplayW;
    }
    if (out_volume < 32767)  // the volume knob scales what leaves the device, not what the screens show
      for (int i = at; i < at + n; ++i) {
        out_l[i] = Sample((int64_t(out_l[i]) * out_volume) >> 15);
        out_r[i] = Sample((int64_t(out_r[i]) * out_volume) >> 15);
      }
    frame += n;
    if (frame % kBlock == 0) publish();
  }

  void publish() noexcept {
    CoreSnapshot& s = snap.back();
    s.frame = frame;
    s.mode = mode;
    synth.inst->view(s.synth);
    drum.inst->view(s.drum);
    tape->view(s.tape);
    // t5: the tape stops by itself at its end (6:00): the sequencer's transport stops with it, so MIDI clock out
    // ends with a Stop instead of running on (PARITY-AUDIT.md §4.3). A Stop it already had is a no-op there.
    if (tape_was_playing && !s.tape.playing) {
      Command stop;
      stop.domain = Domain::Tape;
      stop.op = uint16_t(TapeOp::Stop);
      seq->command(stop);
    }
    tape_was_playing = s.tape.playing;
    mixer->view(s.mixer);
    seq->view(s.seq, s.tempo);
    s.stats = stats;
    for (int x = 0; x < kDisplayW; ++x) s.scope[x] = scope[(scope_pos + x) % kDisplayW];
    snap.publish();
  }
};

Core::Core() {
  dsp_init();
  impl_ = std::make_unique<Impl>();
  if (!impl_->synth.inst || !impl_->drum.inst || !impl_->seq || !impl_->tape || !impl_->mixer)
    throw std::runtime_error("op1 core: a module factory returned nothing");
}

Core::~Core() = default;

void Core::process(const Sample* in_l, const Sample* in_r, Sample* out_l, Sample* out_r, int frames) noexcept {
  Impl& m = *impl_;
  const int64_t t0 = now_ns();
  Command c;
  while (m.ui_commands.pop(c)) m.handle_command(c, *this);
  while (m.worker_commands.pop(c)) m.handle_command(c, *this);
  Event e;
  while (m.ui_events.pop(e)) m.handle_event(e);
  while (m.midi_events.pop(e)) m.handle_event(e);

  for (int done = 0; done < frames;) {
    const int to_grid = kBlock - int(m.frame % kBlock);
    const int n = std::min(frames - done, to_grid);
    m.render_chunk(in_l, in_r, out_l, out_r, n, done);
    done += n;
  }

  const double budget_ns = double(frames) * 1e9 / kSampleRate;
  m.stats.dsp_load_pct = float(double(now_ns() - t0) / budget_ns * 100.0);
  m.stats.dsp_load_peak_pct = std::max(m.stats.dsp_load_peak_pct, m.stats.dsp_load_pct);
  m.stats.blocks = uint64_t(m.frame / kBlock);
}

void Core::set_dac_time(int64_t t_ns) noexcept { impl_->dac_ns = t_ns; }
void Core::note_xrun() noexcept { impl_->stats.xruns++; }
void Core::retire(const Command& c) noexcept { impl_->retired.push(c); }  // full queue: leak, never block
bool Core::emit_midi(const Event& e) noexcept { return impl_->midi_out.push(e); }

bool Core::post_ui(const Event& e) noexcept { return impl_->ui_events.push(e); }
bool Core::post_ui(const Command& c) noexcept { return impl_->ui_commands.push(c); }
bool Core::post_midi(const Event& e) noexcept { return impl_->midi_events.push(e); }
bool Core::post_worker(const Command& c) noexcept { return impl_->worker_commands.push(c); }

const CoreSnapshot& Core::snapshot() noexcept { return impl_->snap.read(); }
bool Core::pop_midi_out(Event& e) noexcept { return impl_->midi_out.pop(e); }
bool Core::pop_retired(Command& c) noexcept { return impl_->retired.pop(c); }
bool Core::pop_note_timing(NoteTiming& t) noexcept { return impl_->note_timing.pop(t); }

}  // namespace op1
