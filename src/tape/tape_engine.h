// The 4-track tape and the mixer (owner: t5): constants, value meanings and the view layout for t6.
//
// From the firmware (firmware-behaviour.md §7, og-behaviour-details.md §2.2, §5):
//   4 tracks, 6:00 each = kTapeLength 15,876,000 samples at 44.1 kHz; clips {ch, start, stop}, at most 512;
//   lift/drop through an audio clipboard of one track or all 4, a lift shorter than kClipboardMaxLength
//   (3,969,000 samples = 90 s); tape state loop / loop_in / loop_out / pos / track; MIDI Song Position
//   seeks to SPP * 10,584,000 / BPM / 16 + 0.5 samples (float, as the firmware computes it) and is
//   ignored past the tape's end; MIDI Start seeks to 0 and plays, Continue plays, Stop stops.
// Decided here (the firmware is not known on these points; each is listed as a gap in the t5 report):
//   - Samples are stored as 16-bit, like the device's AIFF files (open item in architecture.md §8):
//     4 x 6:00 = 127 MB, allocated and prefaulted once at start-up, plus 32 MB of clipboard.
//   - Speed is in permille of normal (1000 = 1.0x, 0..2000) and varies pitch and time together like a
//     tape machine; the firmware's speed curve (`tapespeed`) is not located.
//   - Record mode: 0 = replace (punch-in: the selected track is silent under the head while recording,
//     the live signal is heard), 1 = overdub (new audio is summed onto what is there).
//   - Recording runs while the tape plays with record on; record armed while stopped waits for Play.
//   - Lift takes the loop region when a loop is set, else the clip under the head on the selected
//     track; index 1 = all four tracks (the same region). Drop writes the clipboard at the head, onto
//     the selected track (one-track clipboard) or all four. Both run in slices of kJobSlice samples per
//     audio period so the audio thread stays within its deadline.
//   - The end of the tape (or its start, in reverse) stops the transport.
//   - Input routing (SetInputRouting) is stored and shown, not applied: the core hands the tape one mono
//     record signal (live instruments + input).
#pragma once

#include <cstdint>

#include "op1/config.h"

namespace op1::tape {

inline constexpr int32_t kClipboardMaxLength = 3969000;  // 90.0 s (0x0191BBEC)
inline constexpr int32_t kClipboardQuarter = kClipboardMaxLength / 4;  // 22.5 s path (use unknown)
inline constexpr int kSpeedNormal = 1000, kSpeedMax = 2000;           // permille
inline constexpr int32_t kJobSlice = 16384;                          // samples per channel per period
enum RecordMode : int { kReplace = 0, kOverdub = 1 };
enum LiftState : int { kIdle = 0, kHolding = 1, kLifting = 2, kDropping = 3, kClearing = 4 };

// Q1.31 <-> 16-bit tape sample (round to nearest, saturate).
inline int16_t to_tape(int32_t s) noexcept {
  const int64_t r = (int64_t(s) + 0x8000) >> 16;
  return int16_t(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
}

// TapeView::display layout (t6 reads it):
//   [0] LiftState, [1] clipboard length (samples), [2] clipboard is all tracks (0/1), [3] clip count,
//   [4] record mode, [5] speed permille, [6 + 2t], [7 + 2t] start/stop of the clip under the head on
//   track t (-1 if none), [14] input routing, [15] clips refused (MAX_CLIPS), [16] jobs refused (busy)
//   TapeView::level[t] = peak of track t's playback in the last block (int16 scale).

// MixerView: volume/pan/eq/master as stored (op1.db `master`); meter_l/r = output peak (int16 scale).
// Gain law (ASSUMED, the firmware's is not located): volume 0..32767 linear; pan -32768..32767
// linear balance (0 = centre, both sides full); EQ and master_params are stored only (not applied).

}  // namespace op1::tape
