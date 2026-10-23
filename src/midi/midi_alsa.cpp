// MIDI in/out over the ALSA sequencer (owner: t5).
//
// The port: one duplex sequencer port named "OP-1 Midi", as the OG's USB-MIDI interface names its port
// (firmware-behaviour.md §9: MidiStreaming, port name "OP-1 Midi"). Other programs connect to it like to
// a device. MIDI on the OG is USB-only (no DIN/UART, §2), so every route here is a USB-MIDI route:
//   - host side (a USB MIDI device plugged into this machine): every hardware sequencer port is connected
//     both ways at start-up, and ports that appear later (hot-plug, via System:Announce) too;
//   - device side (this machine plugged into a computer as a USB MIDI device): on a board with a USB
//     device controller, usb-midi-gadget.sh creates the Linux f_midi gadget; its port is a hardware port,
//     so the same auto-connection joins it to "OP-1 Midi" (see that script; not testable on magix-system).
// The thread polls the sequencer (input wakes it at once) and every 1 ms sends what the core queued.
// Messages are built by the sequencer (src/seq/), which applies channel, note-off velocity 64, clock,
// transport and all-notes-off as the firmware does; this file only moves bytes.
#include <alsa/asoundlib.h>
#include <poll.h>

#include <atomic>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "op1/core.h"
#include "op1/io.h"

namespace op1 {
namespace {

int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

class AlsaMidi final : public MidiIo {
 public:
  explicit AlsaMidi(std::string name) : name_(std::move(name)) {}
  ~AlsaMidi() override { stop(); }

  bool start(Core& core, std::string& err) override {
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
      err = "cannot open the ALSA sequencer";
      return false;
    }
    snd_seq_set_client_name(seq_, name_.c_str());
    port_ = snd_seq_create_simple_port(
        seq_, "OP-1 Midi",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
            SND_SEQ_PORT_CAP_DUPLEX,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port_ < 0) {
      err = "cannot create the sequencer port";
      snd_seq_close(seq_);
      seq_ = nullptr;
      return false;
    }
    me_ = snd_seq_client_id(seq_);
    snd_seq_connect_from(seq_, port_, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);  // hot-plug
    connect_all();
    run_ = true;
    thread_ = std::thread([this, &core] { loop(core); });
    return true;
  }

  void stop() override {
    run_ = false;
    if (thread_.joinable()) thread_.join();
    if (seq_) snd_seq_close(seq_);
    seq_ = nullptr;
  }

  std::string describe() const override {
    if (!seq_) return "closed";
    return name_ + " " + std::to_string(me_) + ":" + std::to_string(port_) + " 'OP-1 Midi', " +
           std::to_string(sources_.load()) + " hardware source(s), " + std::to_string(sinks_.load()) +
           " hardware destination(s)";
  }

 private:
  // A hardware port (USB MIDI device, f_midi gadget, virmidi) gets connected both ways. Midi Through,
  // the system client, user-space clients (PipeWire, DAWs: they connect to us themselves) are skipped.
  void connect_port(int client, int port) {
    if (client == SND_SEQ_CLIENT_SYSTEM || client == me_) return;
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    if (snd_seq_get_any_client_info(seq_, client, ci) < 0) return;
    if (std::strstr(snd_seq_client_info_get_name(ci), "Through")) return;
    if (snd_seq_get_any_port_info(seq_, client, port, pi) < 0) return;
    const unsigned caps = snd_seq_port_info_get_capability(pi);
    if (!(snd_seq_port_info_get_type(pi) & SND_SEQ_PORT_TYPE_HARDWARE) || (caps & SND_SEQ_PORT_CAP_NO_EXPORT))
      return;
    const unsigned rd = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
    const unsigned wr = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    if ((caps & rd) == rd && snd_seq_connect_from(seq_, port_, client, port) >= 0) ++sources_;
    if ((caps & wr) == wr && snd_seq_connect_to(seq_, port_, client, port) >= 0) ++sinks_;
  }

  void connect_all() {
    snd_seq_client_info_t* ci;
    snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq_, ci) >= 0) {
      const int cl = snd_seq_client_info_get_client(ci);
      snd_seq_port_info_set_client(pi, cl);
      snd_seq_port_info_set_port(pi, -1);
      while (snd_seq_query_next_port(seq_, pi) >= 0) connect_port(cl, snd_seq_port_info_get_port(pi));
    }
  }

  void loop(Core& core) {
    const int n = snd_seq_poll_descriptors_count(seq_, POLLIN);
    std::vector<pollfd> fds(static_cast<size_t>(n));
    snd_seq_poll_descriptors(seq_, fds.data(), static_cast<unsigned>(n), POLLIN);
    while (run_) {
      if (poll(fds.data(), static_cast<nfds_t>(n), 1) > 0) {
        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input(seq_, &ev) >= 0 && ev) {
          if (ev->type == SND_SEQ_EVENT_PORT_START) {
            connect_port(ev->data.addr.client, ev->data.addr.port);
          } else {
            Event e;
            if (translate(*ev, e)) core.post_midi(e);  // queue full: dropped, never blocks the core
          }
          ev = nullptr;
        }
      }
      Event o;
      bool sent = false;
      while (core.pop_midi_out(o)) sent |= send(o);
      if (sent) snd_seq_drain_output(seq_);
    }
  }

  static bool translate(const snd_seq_event_t& ev, Event& e) {
    e = Event{};
    e.time_ns = now_ns();
    e.src = Source::Midi;
    switch (ev.type) {
      case SND_SEQ_EVENT_NOTEON:
        e.type = ev.data.note.velocity ? EventType::NoteOn : EventType::NoteOff;
        e.channel = ev.data.note.channel; e.a = ev.data.note.note; e.b = ev.data.note.velocity;
        return true;
      case SND_SEQ_EVENT_NOTEOFF:
        e.type = EventType::NoteOff;
        e.channel = ev.data.note.channel; e.a = ev.data.note.note; e.b = ev.data.note.velocity;
        return true;
      case SND_SEQ_EVENT_CONTROLLER:
        e.type = EventType::ControlChange;
        e.channel = uint8_t(ev.data.control.channel); e.a = uint8_t(ev.data.control.param & 127);
        e.b = uint8_t(ev.data.control.value & 127);
        return true;
      case SND_SEQ_EVENT_PGMCHANGE:
        e.type = EventType::ProgramChange;
        e.channel = uint8_t(ev.data.control.channel); e.a = uint8_t(ev.data.control.value & 127);
        return true;
      case SND_SEQ_EVENT_CHANPRESS:
        e.type = EventType::ChannelPressure;
        e.channel = uint8_t(ev.data.control.channel); e.b = uint8_t(ev.data.control.value & 127);
        return true;
      case SND_SEQ_EVENT_PITCHBEND:
        e.type = EventType::PitchBend;
        e.channel = uint8_t(ev.data.control.channel); e.value = int16_t(ev.data.control.value);
        return true;
      case SND_SEQ_EVENT_SONGPOS:
        e.type = EventType::SongPosition;
        e.value = int16_t(ev.data.control.value & 0x3FFF);
        return true;
      case SND_SEQ_EVENT_CLOCK: e.type = EventType::Clock; return true;
      case SND_SEQ_EVENT_START: e.type = EventType::Start; return true;
      case SND_SEQ_EVENT_STOP: e.type = EventType::Stop; return true;
      case SND_SEQ_EVENT_CONTINUE: e.type = EventType::Continue; return true;
      default: return false;
    }
  }

  bool send(const Event& e) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port_);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    const uint8_t ch = e.channel & 15;
    switch (e.type) {
      case EventType::NoteOn: snd_seq_ev_set_noteon(&ev, ch, e.a & 127, e.b & 127); break;
      case EventType::NoteOff: snd_seq_ev_set_noteoff(&ev, ch, e.a & 127, e.b & 127); break;
      case EventType::ControlChange: snd_seq_ev_set_controller(&ev, ch, e.a & 127, e.b & 127); break;
      case EventType::ProgramChange: snd_seq_ev_set_pgmchange(&ev, ch, e.a & 127); break;
      case EventType::ChannelPressure: snd_seq_ev_set_chanpress(&ev, ch, e.b & 127); break;
      case EventType::PitchBend: snd_seq_ev_set_pitchbend(&ev, ch, e.value); break;
      case EventType::SongPosition:
        ev.type = SND_SEQ_EVENT_SONGPOS;
        ev.data.control.value = e.value & 0x3FFF;
        break;
      case EventType::Clock: ev.type = SND_SEQ_EVENT_CLOCK; break;
      case EventType::Start: ev.type = SND_SEQ_EVENT_START; break;
      case EventType::Stop: ev.type = SND_SEQ_EVENT_STOP; break;
      case EventType::Continue: ev.type = SND_SEQ_EVENT_CONTINUE; break;
      default: return false;
    }
    return snd_seq_event_output(seq_, &ev) >= 0;
  }

  std::string name_;
  snd_seq_t* seq_ = nullptr;
  int port_ = 0, me_ = -1;
  std::atomic<int> sources_{0}, sinks_{0};
  std::atomic<bool> run_{false};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<MidiIo> create_midi_io(const std::string& client_name) {
  return std::make_unique<AlsaMidi>(client_name);
}

}  // namespace op1
