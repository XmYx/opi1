// Tape clips (owner: t5): the firmware keeps every recording as a clip {id, ch, start, stop} in tape.db
// (Clips.cpp; firmware-behaviour.md §7), at most MAX_CLIPS = 512 over all tracks
// (og-behaviour-details.md §5: 0x0195BC40 R0 = 0x200). Overlapping or touching recordings on one track
// join into one clip (Clips.cpp:398 joining). Fixed array, no allocation: safe on the audio thread.
#pragma once

#include <cstdint>

#include "op1/config.h"

namespace op1::tape {

inline constexpr int kMaxClips = 512;
// kMinClipLength is read from RAM in the firmware (value unknown, og-behaviour-details.md §5): here a
// clip must be at least one sample long (ASSUMED).
inline constexpr int32_t kMinClipLength = 1;

struct Clip {
  int32_t start = 0, stop = 0;  // [start, stop) in samples; start < kTapeLength, stop <= kTapeLength
  uint8_t ch = 0;
};

class Clips {
 public:
  int count() const noexcept { return n_; }
  const Clip& at(int i) const noexcept { return c_[i]; }

  // Adds [start, stop) on ch, joining every clip it overlaps or touches. false = MAX_CLIPS reached.
  bool add(int ch, int32_t start, int32_t stop) noexcept {
    if (start > stop) { const int32_t t = start; start = stop; stop = t; }
    if (start < 0) start = 0;
    if (stop > kTapeLength) stop = kTapeLength;
    if (stop - start < kMinClipLength) return true;
    for (int i = 0; i < n_;) {
      if (c_[i].ch == ch && c_[i].start <= stop && start <= c_[i].stop) {
        if (c_[i].start < start) start = c_[i].start;
        if (c_[i].stop > stop) stop = c_[i].stop;
        remove(i);
      } else {
        ++i;
      }
    }
    if (n_ == kMaxClips) return false;
    c_[n_++] = Clip{start, stop, uint8_t(ch)};
    return true;
  }

  // Removes [a, b) from ch: clips inside go, clips across an edge are trimmed, a clip around it is split.
  void erase(int ch, int32_t a, int32_t b) noexcept {
    for (int i = 0; i < n_;) {
      Clip& c = c_[i];
      if (c.ch != ch || c.stop <= a || b <= c.start) { ++i; continue; }
      if (a <= c.start && c.stop <= b) { remove(i); continue; }
      if (c.start < a && b < c.stop) {  // split: the right part becomes a new clip when there is room
        const Clip right{b, c.stop, c.ch};
        c.stop = a;
        if (n_ < kMaxClips) c_[n_++] = right;
      } else if (c.start < a) {
        c.stop = a;
      } else {
        c.start = b;
      }
      ++i;
    }
  }

  // Split key: the take under pos ends at pos and a new take starts there (the two touch but stay apart
  // until something joins them). false = MAX_CLIPS reached. Nothing under pos, or pos on an edge: no-op.
  bool split(int ch, int32_t pos) noexcept {
    const int i = find(ch, pos);
    if (i < 0 || pos <= c_[i].start) return true;
    if (n_ == kMaxClips) return false;
    const Clip right{pos, c_[i].stop, c_[i].ch};
    c_[i].stop = pos;
    c_[n_++] = right;
    return true;
  }
  // Shift + split (join): the take under pos and the nearest other take on ch become one take.
  void join(int ch, int32_t pos) noexcept {
    const int i = find(ch, pos);
    if (i < 0) return;
    int best = -1;
    int64_t dist = 0;
    for (int j = 0; j < n_; ++j) {
      if (j == i || c_[j].ch != ch) continue;
      const int64_t d = c_[j].start >= c_[i].stop ? int64_t(c_[j].start) - c_[i].stop : int64_t(c_[i].start) - c_[j].stop;
      if (best < 0 || d < dist) { best = j; dist = d; }
    }
    if (best < 0) return;
    if (c_[best].start < c_[i].start) c_[i].start = c_[best].start;
    if (c_[best].stop > c_[i].stop) c_[i].stop = c_[best].stop;
    remove(best);
  }

  int find(int ch, int32_t pos) const noexcept {
    for (int i = 0; i < n_; ++i)
      if (c_[i].ch == ch && c_[i].start <= pos && pos < c_[i].stop) return i;
    return -1;
  }
  void clear(int ch) noexcept {
    for (int i = 0; i < n_;)
      if (c_[i].ch == ch) remove(i);
      else ++i;
  }

 private:
  void remove(int i) noexcept { c_[i] = c_[--n_]; }
  Clip c_[kMaxClips];
  int n_ = 0;
};

}  // namespace op1::tape
