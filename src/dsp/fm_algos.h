// fm operator model shared by the engine (eng_fm.cpp) and its screen (t2/t3, 2026-10-23).
//
// Firmware facts (fm screen code, R.00246):
//   - layout = int(white * 8.0f + 0.5f) -> 0..8, one of the nine authored groups A..I of fm.svg (op%i%i cubes;
//     0x019A4D72: [knob 2] * 0x41000000, rounded at 0x019A4D8A). The default white 3000 shows layout B.
//   - code = int(green * 4095.0f + 0.5f) (0x019A5412: [knob 1] * 0x457FF000, rounded at 0x019A5422); three
//     3-bit fields pick the three ratio
//     labels: A = (code >> 9) & 7, B = (code >> 6) & 7, C = (code >> 3) & 7 (masks 0xE00, 0x1C0, 0x38), each
//     from a pair of 8-entry string tables (fraction strings 0x018DC64C / 69C / 6EC, numbers 674 / 6C4 / 714).
// ASSUMED: knob float = value / 32767; the three ratios belong to operators 2, 3 and 4 (operator 1 = 1:1);
// the modulation graph of each layout (read from cube adjacency in fm.svg: a cube resting on another
// modulates it; a cube with nothing below it is a carrier). The firmware's FM DSP itself is not decoded.
#pragma once

#include <cstdint>

namespace op1::dsp {

inline constexpr int kFmAlgos = 9;

struct FmAlgo {
  uint8_t mods[4];   // bit mask of the operators that modulate operator i (always higher-numbered)
  uint8_t carriers;  // bit mask of the operators heard
  int8_t fb;         // operator with self-feedback, -1 = none
};

// Operators 0..3 here are 1..4 on screen; group letter = index ('A' + i).
inline constexpr FmAlgo kFmAlgos9[kFmAlgos] = {
    {{0b0010, 0b0100, 0b1000, 0}, 0b0001, 3},  // A: 4 > 3 > 2 > 1
    {{0b0010, 0b1000, 0, 0}, 0b0101, -1},      // B: 4 > 2 > 1, 3 alone
    {{0b0100, 0b1000, 0, 0}, 0b0011, -1},      // C: 3 > 1, 4 > 2
    {{0b0100, 0, 0b1000, 0}, 0b0011, -1},      // D: 4 > 3 > 1, 2 alone
    {{0b0110, 0, 0b1000, 0}, 0b0001, -1},      // E: (2 + (4 > 3)) > 1
    {{0, 0b1100, 0, 0}, 0b0011, -1},           // F: (3 + 4) > 2, 1 alone
    {{0b1110, 0, 0, 0}, 0b0001, 3},            // G: (2 + 3 + 4) > 1
    {{0b1010, 0b0100, 0, 0}, 0b0001, -1},      // H: ((3 > 2) + 4) > 1
    {{0, 0, 0, 0}, 0b1111, 3},                 // I: 1 + 2 + 3 + 4
};

// Ratio tables of the three green fields, in millionths (the firmware's labels: 1/64 = 0.015625 etc.).
inline constexpr int32_t kFmRatioA[8] = {15625, 93750, 125000, 312500, 500000, 625000, 1000000, 2000000};
inline constexpr int32_t kFmRatioB[8] = {2000000, 1500000, 1000000, 500000, 750000, 250000, 156250, 31250};
inline constexpr int32_t kFmRatioC[8] = {62500, 437500, 625000, 1000000, 1750000, 2000000, 3500000, 4000000};
// The firmware's label strings, same order (screens draw these).
inline constexpr const char* kFmLabelA[8] = {"1/64", "3/32", "1/8", "5/16", "1/2", "5/8", "1", "2"};
inline constexpr const char* kFmLabelB[8] = {"2", "1.5", "1", "1/2", "3/4", "1/4", "5/32", "1/32"};
inline constexpr const char* kFmLabelC[8] = {"1/16", "7/16", "5/8", "1", "1.75", "2", "3.5", "4"};

// Knob value (0..32767 stored) -> layout 0..8 and ratio code 0..4095, rounded as the firmware does:
// int(v / 32767 * 8 + 0.5) and int(v / 32767 * 4095 + 0.5) in integer arithmetic.
constexpr int fm_layout(int32_t white) noexcept {
  const int32_t w = white < 0 ? 0 : (white > 32767 ? 32767 : white);
  return int((int64_t(w) * 16 + 32767) / 65534);
}
constexpr int fm_code(int32_t green) noexcept {
  const int32_t g = green < 0 ? 0 : (green > 32767 ? 32767 : green);
  return int((int64_t(g) * 8190 + 32767) / 65534);
}

}  // namespace op1::dsp
