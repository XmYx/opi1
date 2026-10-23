// Fixed facts of the OG OP-1 that the whole port shares. Each value is taken from the firmware
// (R.00246) as documented in ~/RIGS-Shared/files/op1-og-native/docs/firmware-behaviour.md.
// Owner: t3 (architecture). Change only through docs/interface-changes.md (see architecture.md).
#pragma once

#include <cstddef>
#include <cstdint>

namespace op1 {

inline constexpr int kSampleRate = 44100;          // §3: immediate 0xAC44 at 10+ sites
inline constexpr int kBlock = 64;                  // §3: SPORT0 DMA period, 64 stereo frames = 1.45 ms
inline constexpr int kSynthVoices = 6;             // §3: kSynthVoiceCount
inline constexpr int kDrumKeys = 24;               // §4: per-key arrays of 24 in drum patches
inline constexpr int kKnobs = 4;                   // §4: kKnobCount (blue, green, white, orange)
inline constexpr int kParams = 8;                  // §4: knobs/adsr/fx_params/lfo_params are 8 x int16
inline constexpr int kTapeTracks = 4;              // §7: kTapeNumTracks
inline constexpr int32_t kTapeLength = 15876000;   // §7: kTape_length 0xF23FA0 = 6:00 at 44.1 kHz
inline constexpr int kDisplayW = 320;              // §10
inline constexpr int kDisplayH = 160;
inline constexpr int kTempoMin = 40, kTempoMax = 220;  // §8: tempo.cpp:129 (tenths via tempobpmdecimal)

// Audio inside the core is Q1.31 fixed point: the OG's SPORT0 carries 32-bit slots and the
// Blackfin BF524 has no FPU, so the engines are integer code (see fract.h). Device I/O in S32_LE
// is a straight copy; S16_LE is a shift.
using Sample = int32_t;

// A stored parameter value: patches keep eight signed 16-bit values per group, mostly 0..32767.
using Param = int16_t;

}  // namespace op1
