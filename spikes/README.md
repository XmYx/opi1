# Language spikes (t3): Rust vs C++

Two throw-away programs with the same behaviour and the same algorithms, written once in each
language. They only exist to measure the language, so the system layers are the same in both:
ALSA PCM for audio, the ALSA sequencer for MIDI (USB MIDI devices are kernel sequencer clients,
so they arrive on the same path), SDL3 for the window. The results and the decision are in
`~/RIGS-Shared/files/op1-og-native/docs/language-benchmark.md`.

| | `cpp/spike.cpp` | `rust/src/main.rs` |
|---|---|---|
| audio | libasound, blocking duplex loop (capture linked to playback) | `alsa` crate 0.9 (same libasound calls) |
| MIDI | libasound seq, own thread, poll() | `alsa` crate seq, own thread, poll() |
| window | SDL3 (C API) | `sdl3-sys` (same C API) |
| MIDI/key → audio queue | hand-written SPSC ring | hand-written SPSC ring (same code shape) |
| scope → UI | wait-free triple buffer | same |
| build | `g++ -O3` (one file) | `cargo build --release` |

## Shared behaviour

- **Audio**: 44,100 Hz (the OP-1 rate), stereo, S32_LE (else S16_LE), `--period` frames x `--periods`.
  The playback buffer is pre-filled with silence, capture is linked to playback and started with it, and
  every loop iteration is: read one capture period (or wait for one period of playback space) -> drain
  the event queues -> render -> `snd_pcm_delay` -> write one period. Round trip is therefore the buffer
  size, output latency is the playback delay at write time.
- **DSP**: 6 voices (OP-1 polyphony). Each voice is a PolyBLEP saw into a TPT state-variable low-pass
  whose cutoff follows a linear attack/exponential release envelope (`tanf` per sample). The capture
  input goes through a DC-blocking high-pass into the mix (the input-to-sound path).
- **MIDI**: a sequencer client `op1og-spike-<lang>` with one input port. It subscribes to every
  hardware/USB MIDI source at start. Note on/off go to the audio thread through a lock-free SPSC ring
  with the CLOCK_MONOTONIC receive time. The audio thread logs, per note, the time its first sample
  reaches the DAC (`write time + delay/rate`).
- **Window**: 640x320 (the 320x160 display at 2x), SDL3 renderer with vsync, a streaming 320x160
  texture drawn in software every frame (scope, 4 encoder dials in the OP-1 colours, 6 voice LEDs).
  Keys A W S E D F T G Y H U J K play notes 60-72.
- **Stats** (JSON at exit): xruns, DSP time and load per period, playback delay, note latency,
  frame CPU time and interval, process and audio-thread CPU %, max RSS.
- `--bench-dsp SECONDS`: offline render, no devices: the float voice and a Blackfin-style fixed-point
  voice (Q31 phase, Q15 saturating SVF), best of 5 runs.

Options (both): `--device hw:0,0 --period 64 --periods 2 --duration 30 --no-window --no-capture
--mute --rt --rt-prio N --json FILE --notes FILE --shot FILE.bmp --bench-dsp SECONDS`.

`bench/` holds the MIDI probe (sends timed notes, logs send times) and the scripts that ran the
measurements.
