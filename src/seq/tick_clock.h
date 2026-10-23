// The sequencer clock (owner: t5). 96 ticks per quarter note, as Timing.cpp in the firmware
// (og-behaviour-details.md §3: 96 = 1/4, 48 = 1/8, 32 = 1/8t, 24 = 1/16, 16 = 1/16t, 12 = 1/32, 8 = 1/32t).
//
// Internal clock: one tick every 275625 / bpm_tenths samples (44100 * 60 * 10 / 96). The phase is an
// exact integer accumulator, so the tick count never drifts against the sample count.
//
// External clock (MIDI 0xF8, 24 per quarter): every F8 allows 4 more ticks (x4, §2.2). The tick on the
// F8 itself fires at once; the 3 between are spaced by the tempo measured from the F8 intervals, and
// never run ahead of the F8s. A lag of more than one F8 is caught up immediately. So over any run the
// tick count is exactly 4 x the F8 count: no drift, whatever the sender's tempo does.
#pragma once

#include <cstdint>

namespace op1::seq {

inline constexpr int kPpqn = 96;
inline constexpr int kMidiClockDiv = kPpqn / 24;  // internal ticks per MIDI clock
inline constexpr int64_t kTickUnits = 275625;     // samples per tick x bpm_tenths
inline constexpr int kTicks16th = 24;
// Tick counts start here, so a grid origin moved back by a MIDI song position (up to 16383 sixteenths
// = 393,192 ticks) never goes below zero.
inline constexpr uint64_t kTickBase = uint64_t(1) << 32;

constexpr bool valid_timing(int t) {
  return t == 96 || t == 48 || t == 32 || t == 24 || t == 16 || t == 12 || t == 8;
}

class TickClock {
 public:
  void set_bpm_tenths(int b) noexcept { bpm_ = b < 400 ? 400 : (b > 2200 ? 2200 : b); }
  int bpm_internal() const noexcept { return bpm_; }

  void set_external(bool on) noexcept {
    if (on == external_) return;
    external_ = on;
    fired_ext_ = target_ = 0;
    n_iv_ = iv_pos_ = 0;
    last_ns_ = 0;
  }
  bool external() const noexcept { return external_; }

  // MIDI F8. t_ns: receive time (CLOCK_MONOTONIC), 0 if unknown (the tempo estimate then stays).
  void midi_clock(int64_t t_ns) noexcept {
    if (!external_) return;
    target_ += kMidiClockDiv;
    if (t_ns > 0) {
      if (last_ns_ > 0) {
        const int64_t dt = t_ns - last_ns_;
        if (dt > 0 && dt < 1000000000) {  // a gap over 1 s = the sender stopped: start measuring again
          iv_[iv_pos_] = dt;
          iv_pos_ = (iv_pos_ + 1) % kIv;
          if (n_iv_ < kIv) ++n_iv_;
        } else {
          n_iv_ = iv_pos_ = 0;
        }
      }
      last_ns_ = t_ns;
    }
  }
  // MIDI FA: the next F8 is the first tick of the song.
  void midi_start() noexcept {
    fired_ext_ = target_ = 0;
    acc_ = 0;
  }
  // Next sample fires a tick (internal clock): used when the transport starts so step 0 is on time.
  // Tick j after this then fires on sample ceil(j * kTickUnits / bpm): on time or up to one sample late.
  void rephase() noexcept { acc_ = kTickUnits - bpm_; }

  // Advance one sample. true = a tick fires on this sample (its index is ticks() - 1).
  bool step() noexcept {
    if (!external_) {
      acc_ += bpm_;
      if (acc_ < kTickUnits) return false;
      acc_ -= kTickUnits;
      ++ticks_;
      return true;
    }
    if (fired_ext_ >= target_) return false;
    const bool on_f8 = fired_ext_ % kMidiClockDiv == 0;
    if (on_f8 || target_ - fired_ext_ > kMidiClockDiv) {
      acc_ = 0;
    } else {
      acc_ += est_bpm_tenths();
      if (acc_ < kTickUnits) return false;
      acc_ -= kTickUnits;
    }
    ++fired_ext_;
    ++ticks_;
    return true;
  }

  uint64_t ticks() const noexcept { return ticks_; }  // ticks fired since start-up (monotonic)
  int64_t f8_ticks() const noexcept { return target_; }
  int64_t ext_fired() const noexcept { return fired_ext_; }

  // Tempo for screens and the song-position conversion: internal, or measured from MIDI clock.
  int16_t bpm_tenths() const noexcept {
    if (!external_ || n_iv_ == 0) return int16_t(bpm_);
    const int b = est_bpm_tenths();
    return int16_t(b < 400 ? 400 : (b > 2200 ? 2200 : b));
  }

 private:
  // tenths of BPM = 25e9 / (ns per F8)
  int est_bpm_tenths() const noexcept {
    if (n_iv_ == 0) return bpm_;
    int64_t sum = 0;
    for (int i = 0; i < n_iv_; ++i) sum += iv_[i];
    const int64_t b = (int64_t(25000000000) * n_iv_ + sum / 2) / sum;
    return int(b < 100 ? 100 : (b > 6000 ? 6000 : b));  // wider than 40-220 so the follower keeps up
  }

  static constexpr int kIv = 24;  // one beat of F8 intervals
  int bpm_ = 1250;                // op1.db tempobpmdecimal default 1250 = 125.0 BPM
  bool external_ = false;
  int64_t acc_ = 0;
  uint64_t ticks_ = kTickBase;
  int64_t fired_ext_ = 0, target_ = 0;
  int64_t iv_[kIv] = {};
  int iv_pos_ = 0, n_iv_ = 0;
  int64_t last_ns_ = 0;
};

}  // namespace op1::seq
