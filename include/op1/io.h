// Services that own a thread: the audio backend (t3, src/platform/), MIDI in/out (t5, src/midi/) and
// the worker that loads/saves patches and samples and frees retired objects (t5, src/state/).
// Owner: t3 (interface).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "op1/messages.h"

namespace op1 {

class Core;
class AssetProvider;

// ---------------------------------------------------------------- audio (t3)

struct AudioOptions {
  // ALSA PCM: "pipewire" for the desktop graph, hw:X,Y for the lowest latency on a headless device. Empty =
  // default_audio_device(). (Until 2026-10-23 the default was hw:0,0, which on a desktop is usually the
  // motherboard codec with nothing plugged in: the port played into an empty jack, PARITY-AUDIT.md §1.)
  std::string device;
  int period = 32;                // frames per device period (measured: 32 x 4 is the smallest glitch-free
  int fill = 4;                   //  setting on magix-system's onboard codec, language-benchmark.md)
  bool capture = true;            // duplex: live input (mic/line) paces the loop
  bool rt = true;                 // ask rtkit for SCHED_RR on the audio thread
  int rt_prio = 20;               // rtkit's MaxRealtimePriority on Ubuntu
};

struct AudioStats {
  std::string device, format;
  int period = 0, buffer = 0, fill = 0;
  bool rt = false, capture = false, linked = false;
  uint64_t periods = 0, xruns = 0;
  double out_latency_ms = 0;      // mean playback delay at write time
  double dsp_us_p50 = 0, dsp_us_p99 = 0, dsp_us_max = 0;
};

class AudioBackend {
 public:
  virtual ~AudioBackend() = default;
  // Opens the device and starts the realtime thread that calls core.process(). false + err on failure.
  virtual bool start(Core& core, const AudioOptions& opt, std::string& err) = 0;
  virtual void stop() = 0;
  virtual AudioStats stats() const = 0;  // after stop()
};
std::unique_ptr<AudioBackend> create_alsa_backend();
// The device to open when none is given: $OP1_OG_DEVICE; else "pipewire" when the user's desktop session runs
// PipeWire and ALSA knows the "pipewire" PCM (the sound reaches the output the user picked, e.g. a USB
// interface); else "default" when only another sound server runs; else "hw:0,0" (a headless Pi).
std::string default_audio_device();

// ---------------------------------------------------------------- MIDI (t5)

// Domain::Midi operations (settings from op1.db `knobs`, firmware-behaviour.md §8). t5 may append.
enum class MidiOp : uint16_t {
  SetChannel = 1, SetKnobsRelative = 2, SetLrIsTransport = 3, SetSyncIn = 4, SetSyncOut = 5,
  // t5 addition: send Note Off velocity 64 for every note held on MIDI out (the firmware's all-notes-off,
  // og-behaviour-details.md §2). The core routes Domain::Midi to the sequencer, which owns MIDI out.
  AllNotesOff = 6,
};

class MidiIo {
 public:
  virtual ~MidiIo() = default;
  // Creates the ALSA sequencer client, subscribes hardware/USB MIDI sources, starts its thread, which
  // posts incoming events with core.post_midi() and sends what core.pop_midi_out() returns.
  virtual bool start(Core& core, std::string& err) = 0;
  virtual void stop() = 0;
  virtual std::string describe() const = 0;  // "client:port, N sources"
};
std::unique_ptr<MidiIo> create_midi_io(const std::string& client_name);

// ---------------------------------------------------------------- worker (t5)

// Worker requests (t2, 2026-10-23). Command.domain = Synth or Drum, op = WorkerOp, index = preset number
// (0-based, in the order of that domain's factory preset table in op1_factory.db). The worker builds the
// patch, reads the preset's sample or kit audio from the firmware image and posts LoadSynthPatch /
// LoadDrumPatch and SetSample to the core.
enum class WorkerOp : uint16_t { LoadFactoryPreset = 1 };

class Worker {
 public:
  virtual ~Worker() = default;
  // Starts the non-realtime thread: patch/preset/sample loading (posts Commands with ptr through
  // core.post_worker()), saving, and freeing what core.pop_retired() hands back.
  virtual bool start(Core& core, AssetProvider* assets, std::string& err) = 0;
  virtual void stop() = 0;
  // UI thread -> worker, asynchronous (preset browse/load/save, sample import). t5 defines the ops.
  virtual bool request(const Command& c) = 0;
};
std::unique_ptr<Worker> create_worker();

}  // namespace op1
