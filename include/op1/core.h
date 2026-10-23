// The audio-side hub: owns the modules (instruments, sequencer, tape, mixer), every queue into and out
// of the audio thread, and the per-block snapshot for the screens.
//
// Threads (architecture.md): the backend's realtime thread calls process(); the MIDI thread, the UI
// (main) thread and the worker each own one producer queue; nothing else crosses into the audio thread.
// Owner: t3.
#pragma once

#include <cstdint>
#include <memory>

#include "op1/config.h"
#include "op1/messages.h"
#include "op1/snapshot.h"

namespace op1 {

// One played note: when it entered the process and when its first sample reached the DAC (the
// backend's estimate: write time + snd_pcm_delay / rate). Same method as language-benchmark.md.
struct NoteTiming {
  int64_t received_ns = 0;
  int64_t dac_ns = 0;
  uint8_t note = 0;
  Source src = Source::Internal;
};

class Core {
 public:
  Core();  // main thread: builds every module, allocates and prefaults all audio memory
  ~Core();
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  // ---- audio thread only. Never blocks, never allocates.
  // Any frame count (the device period); the core cuts it into chunks on the 64-frame grid.
  // in_l/in_r may be null (no capture).
  void process(const Sample* in_l, const Sample* in_r, Sample* out_l, Sample* out_r, int frames) noexcept;
  void set_dac_time(int64_t t_ns) noexcept;  // backend, before process(): when its first frame reaches the DAC
  void note_xrun() noexcept;                 // backend
  void retire(const Command& c) noexcept;    // hand a ptr back to the worker for freeing
  bool emit_midi(const Event& e) noexcept;   // queue a MIDI message for the MIDI thread

  // ---- producers: each queue has exactly one producer thread
  bool post_ui(const Event& e) noexcept;        // UI thread (computer/on-screen keyboard notes)
  bool post_ui(const Command& c) noexcept;      // UI thread
  bool post_midi(const Event& e) noexcept;      // MIDI thread
  bool post_worker(const Command& c) noexcept;  // worker thread

  // ---- consumers: one thread each
  const CoreSnapshot& snapshot() noexcept;       // UI thread
  bool pop_midi_out(Event& e) noexcept;          // MIDI thread
  bool pop_retired(Command& c) noexcept;         // worker thread
  bool pop_note_timing(NoteTiming& t) noexcept;  // stats (main thread)

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace op1
