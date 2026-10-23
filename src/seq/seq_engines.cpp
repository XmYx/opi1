// The six OG sequencers (owner: t5). Behaviour, commands and the view layout: seq_engines.h.
// Audio-thread code: no allocation, no locks, bounded loops.
#include "seq_engines.h"

#include <cmath>

#include "tick_clock.h"

namespace op1::seq {
namespace {

int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
bool is_on(const Event& e) noexcept { return e.type == EventType::NoteOn && e.b > 0; }
bool is_off(const Event& e) noexcept {
  return e.type == EventType::NoteOff || (e.type == EventType::NoteOn && e.b == 0);
}
// The keyboard key 0..23 of a note: the UI sends it (Event::value = key + 1); for MIDI notes the key of the
// same pitch class within the two octaves from F3.
int key_of(const Event& e) noexcept {
  if (e.src == Source::Keyboard && e.value > 0 && e.value <= kKeys) return e.value - 1;
  return ((int(e.a) - kKeyBase) % kKeys + kKeys) % kKeys;
}
uint32_t lcg_next(uint32_t& s) noexcept {
  s = s * 1664525u + 1013904223u;  // the firmware's NR LCG
  return s >> 8;
}
// Starts wait for the 48-tick grid (the first key while nothing runs moves the grid to itself).
bool on_grid(uint64_t t, const Emitter& out) noexcept {
  return t >= out.grid ? (t - out.grid) % kGrid == 0 : true;
}
// Transposition of the players: (last held note - 60), 0 when nothing is held.
int transpose_of(const KeySet& held, const int* note_of_key) noexcept {
  return held.n ? note_of_key[held.last()] - 60 : 0;
}

constexpr int kTimings[7] = {96, 48, 32, 24, 16, 12, 8};

// Endless euclidean rhythms 1..47 (table 0x01CF0964 / patterns 0x018D5D00..0x018D5FA8), x = play, . = rest.
const char* const kEuclid[48] = {
    "",
    "xx.", "xxx.", "x.x..", "x.x.x", "xxxx.", "xxxxx.", "x..x...", "x.x.x..", "x.x.x.x", "x.xx.xx",
    "xxxxxx.", "x..x..x.", "x.xx.xx.", "xxxxxxx.", "x.x.x.x..", "x.x.x.x.x", "x.xxx.xxx", "x..x..x...",
    "x.xx.xx.xx", "x...x...x..", "x..x..x..x.", "x.x.x.x.x..", "x..x.x..x.x.", "x.xx.x.xx.x.",
    "xxxxxxxxxxx.", "x..x.x..x.x..", "x.x.x.x.x.x..", "x.xx.xx.xx.xx", "x....x....x...", "x.xx.xx.xx.xx.",
    "x...x...x...x..", "x.x.x.x.x.x.x..", "x..x..x..x..x...", "x..x.x.x..x.x.x.", "x.xx.x.x.xx.x.x.",
    "x..x.x..x.x..x.x.", "x.x.x.x.x.x.x.x..", "x..x.x..x.x..x.x..", "x..x.x.x..x.x.x..x.",
    "x..x.x.x.x..x.x.x.x.", "x.xx.x.x.x.xx.x.x.x.", "x..x.x..x.x..x.x..x.x.", "x..x.x..x.x..x.x..x.x..",
    "x..x.x.x.x.x..x.x.x.x.x.", "x.xx.x.x.x.x.xx.x.x.x.x.", "x.xx.x.xx.xx.xx.x.xx.xx.xx.x.x",
    "x..x.x.x.x..x.x.x.x..x.x.x.x..x.x.",
};
int euclid_len(int k) noexcept {
  if (k <= 0 || k > 47) return 0;
  int n = 0;
  while (kEuclid[k][n]) ++n;
  return n;
}

}  // namespace

int timing_index(int ticks) noexcept {
  for (int i = 0; i < 7; ++i)
    if (kTimings[i] == ticks) return i;
  return 1;
}
int timing_ticks(int index) noexcept { return kTimings[clampi(index, 0, 6)]; }

// ------------------------------------------------------------------ helpers

void Sounding::add(int note, int left) noexcept {
  for (auto& x : n)
    if (x.note < 0) { x.note = int16_t(note); x.left = int16_t(left); return; }
}
void Sounding::step(Emitter& out) noexcept {
  for (auto& x : n)
    if (x.note >= 0 && --x.left <= 0) { out.off(x.note); x.note = -1; }
}
void Sounding::all_off(Emitter& out) noexcept {
  for (auto& x : n)
    if (x.note >= 0) { out.off(x.note); x.note = -1; }
}
bool Sounding::has(int note) const noexcept {
  for (const auto& x : n)
    if (x.note == note) return true;
  return false;
}
void Sounding::remove(int note) noexcept {
  for (auto& x : n)
    if (x.note == note) x.note = -1;
}

bool KeySet::add(int key) noexcept {
  if (has(key)) return false;
  const bool was_empty = n == 0;
  if (n < 24) k[n++] = key;
  return was_empty;
}
bool KeySet::remove(int key) noexcept {
  for (int i = 0; i < n; ++i)
    if (k[i] == key) {
      for (int j = i + 1; j < n; ++j) k[j - 1] = k[j];
      --n;
      break;
    }
  return n == 0;
}
bool KeySet::has(int key) const noexcept {
  for (int i = 0; i < n; ++i)
    if (k[i] == key) return true;
  return false;
}

// ------------------------------------------------------------------ pattern

PatternSeq::PatternSeq() noexcept { pm_knob_.set(pm_, 4); }

bool PatternSeq::empty() const noexcept {
  for (uint32_t m : notes_)
    if (m) return false;
  return true;
}
void PatternSeq::stop(Emitter& out) noexcept {
  running_ = pending_ = false;
  snd_.all_off(out);
}
void PatternSeq::key(const Event& e, Emitter& out) noexcept {
  const int k = key_of(e);
  if (is_on(e)) {
    if (empty()) { out.pass(e); return; }  // nothing to play: the key plays normally (0x01A081D0)
    held_note_[k] = e.a;
    const bool first = held_.add(k);
    transpose_ = out.drum ? 0 : transpose_of(held_, held_note_);
    if (first && !running_) { pending_ = true; clk_.restart(); }
    return;
  }
  if (!is_off(e)) return;
  if (!held_.has(k)) { out.pass(e); return; }
  held_.remove(k);
  if (held_.n) transpose_ = out.drum ? 0 : transpose_of(held_, held_note_);
  if (held_.n == 0 && !latch_) stop(out);
}
void PatternSeq::advance() noexcept {
  const int lo = start_, hi = start_ + length_ - 1;
  switch (pm_) {
    case 0:  // rev
      pos_ = pos_ - 1 < lo ? hi : pos_ - 1;
      break;
    case 1:  // cycle: ping-pong, the ends are not repeated
      if (fwd_) {
        if (pos_ + 1 > hi) { pos_ = hi - 1 > lo ? hi - 1 : lo; fwd_ = false; }
        else ++pos_;
      } else {
        if (pos_ - 1 < lo) { pos_ = lo + 1 < hi ? lo + 1 : hi; fwd_ = true; }
        else --pos_;
      }
      break;
    default:  // fwd
      pos_ = pos_ + 1 > hi ? lo : pos_ + 1;
      break;
  }
}
void PatternSeq::tick(uint64_t t, Emitter& out) noexcept {
  if (pending_ && on_grid(t, out)) {
    pending_ = false;
    running_ = true;
    pos_ = start_;
    fwd_ = true;
    clk_.restart();
  }
  if (!running_) return;
  if (!clk_.tick(kTicks16th, swing_)) return;
  snd_.all_off(out);  // the gate is one step
  const uint32_t m = notes_[pos_ & 15];
  for (int k = 0; k < kKeys; ++k)
    if (m & (1u << k)) {
      const int n = clampi(kKeyBase + k + transpose_, 0, 127);
      out.on(n, kVelocity, 0);
      snd_.add(n, 1);
    }
  advance();
}
void PatternSeq::cursor_move(int d) noexcept {
  const int lo = start_, len = length_;
  cursor_ = lo + (((cursor_ - lo + d) % len) + len) % len;
}
void PatternSeq::param(int i, int v) noexcept {
  switch (i) {
    case 0:
      length_ = clampi(v, 1, 16);
      if (start_ + length_ > 16) start_ = 16 - length_;
      break;
    case 1: pm_ = clampi(v, 0, 2); pm_knob_.set(pm_, 4); break;
    case 2: swing_ = clampi(v, 20, 80); break;
    case 3: if (clampi(v, 0, 15) + length_ <= 16) start_ = clampi(v, 0, 15); break;
    case 4: latch_ = v != 0; break;
    default: break;
  }
  cursor_ = clampi(cursor_, start_, start_ + length_ - 1);
}
void PatternSeq::edit(int i, int v) noexcept {
  if (i >= 0 && i < 16 && v >= 0 && v < kKeys) notes_[i] ^= 1u << v;
  else if (i >= 256 && i < 272) notes_[i - 256] = uint32_t(v) & 0xFFFFFFu;
}
void PatternSeq::encoder(int i, int d, Emitter& out) noexcept {
  switch (i) {
    case 0: cursor_move(d); break;
    case 1: swing_ = clampi(swing_ + d, 20, 80); break;
    case 2: param(0, length_ + d); break;
    case 3:
      if (d > 0) {
        latch_ = true;
        if (!running_ && !pending_ && !empty()) { pending_ = true; clk_.restart(); }
      } else {
        latch_ = false;
        stop(out);
      }
      break;
    case 4:
      notes_[cursor_ & 15] = 0;
      cursor_move(d > 0 ? 1 : -1);
      break;
    case 5: {  // rotate the window one step per detent
      const int lo = start_, hi = start_ + length_ - 1;
      for (int r = 0; r < (d < 0 ? -d : d); ++r) {
        if (d > 0) {
          const uint32_t last = notes_[hi];
          for (int s = hi; s > lo; --s) notes_[s] = notes_[s - 1];
          notes_[lo] = last;
        } else {
          const uint32_t first = notes_[lo];
          for (int s = lo; s < hi; ++s) notes_[s] = notes_[s + 1];
          notes_[hi] = first;
        }
      }
      break;
    }
    case 6: param(3, start_ + d); break;
    case 7: pm_ = pm_knob_.turn(d, 0, 2, 4); break;
    default: break;
  }
}
void PatternSeq::key_edit(int key, bool down, Emitter& out) noexcept {
  if (key < 0) {  // shift released
    if (entry_ > 0) {
      entry_ = 0;
      if (!running_) cursor_move(1);
    }
    return;
  }
  const int n = kKeyBase + clampi(key, 0, 23);
  if (down) {
    notes_[(running_ ? pos_ : cursor_) & 15] ^= 1u << clampi(key, 0, 23);
    ++entry_;
    out.on(n, kVelocity, 0);  // audition
  } else {
    out.off(n);
    if (entry_ > 0 && --entry_ == 0 && !running_) cursor_move(1);
  }
}
void PatternSeq::nav(int which, bool, Emitter&) noexcept {
  if (which == 0) cursor_move(-1);
  else if (which == 1) cursor_move(1);
}
void PatternSeq::release(Emitter& out) noexcept {
  held_.clear();
  latch_ = false;
  stop(out);
}
void PatternSeq::view(int32_t* d) const noexcept {
  d[0] = length_;
  for (int i = 0; i < 16; ++i) d[1 + i] = int32_t(notes_[i]);
  d[17] = cursor_;
  d[18] = start_;
  d[19] = pm_;
  d[20] = swing_;
  d[21] = latch_;
}
void PatternSeq::get_params(int32_t* p) const noexcept {
  p[0] = length_; p[1] = pm_; p[2] = swing_; p[3] = start_; p[4] = latch_;
}

// ------------------------------------------------------------------ endless

EndlessSeq::EndlessSeq() noexcept {
  timing_knob_.set(timing_index(timing_), 4);
  pm_knob_.set(pm_, 4);
}
void EndlessSeq::stop(Emitter& out) noexcept {
  running_ = pending_ = false;
  snd_.all_off(out);
}
void EndlessSeq::key(const Event& e, Emitter& out) noexcept {
  const int k = key_of(e);
  if (is_on(e)) {
    if (count_ == 0) { out.pass(e); return; }
    held_note_[k] = e.a;
    const bool first = held_.add(k);
    if (first && !running_ && !crank_) { pending_ = true; clk_.restart(); }
    return;
  }
  if (!is_off(e)) return;
  if (!held_.has(k)) { out.pass(e); return; }
  held_.remove(k);
  if (held_.n == 0 && !hold_ && !crank_) stop(out);
}
int EndlessSeq::next_index() noexcept {
  if (count_ <= 0) return -1;
  switch (pm_) {
    case 0: return cur_ - 1 < 0 ? count_ - 1 : cur_ - 1;  // rev
    case 1: {                                             // shuffle bag (Fisher-Yates, 0x019881F4)
      if (nbag_ <= 0) {
        nbag_ = count_;
        for (int i = 0; i < count_; ++i) bag_[i] = i;
        for (int i = count_ - 1; i > 0; --i) {
          const int j = int(lcg_next(lcg_) % uint32_t(i + 1));
          const int tmp = bag_[i];
          bag_[i] = bag_[j];
          bag_[j] = tmp;
        }
      }
      return bag_[--nbag_];
    }
    default: return (cur_ + 1) % count_;  // fwd
  }
}
void EndlessSeq::do_step(Emitter& out) noexcept {
  snd_.step(out);  // note lengths count in steps
  if (eucl_ > 0) {
    const int len = euclid_len(eucl_);
    if (len > 0) {
      if (epos_ < 0) epos_ = clampi(eucl_off_, 0, len - 1);
      const bool hit = kEuclid[eucl_][epos_] == 'x';
      epos_ = (epos_ + 1) % len;
      if (!hit) return;  // a rest that does not advance the sequence
    }
  }
  cur_ = next_index();
  if (cur_ < 0) return;
  const Step& st = steps_[cur_];
  const int tr = out.drum ? 0 : transpose_of(held_, held_note_);
  for (int i = 0; i < st.n; ++i) {
    const int n = clampi(kKeyBase + st.key[i] + tr, 0, 127);
    out.on(n, kVelocity, 0);
    snd_.remove(n);
    snd_.add(n, st.len[i] > 0 ? st.len[i] : 1);
  }
}
void EndlessSeq::tick(uint64_t t, Emitter& out) noexcept {
  if (pending_ && on_grid(t, out)) {
    pending_ = false;
    running_ = true;
    cur_ = pm_ == 0 ? 0 : -1;  // fwd starts at step 0; rev at the last step
    if (pm_ == 0) cur_ = 0;
    nbag_ = 0;
    epos_ = -1;
    clk_.restart();
  }
  if (!running_ || crank_) return;
  if (clk_.tick(timing_, swing_, timing_ == 48 || timing_ == 24)) do_step(out);
}
void EndlessSeq::param(int i, int v) noexcept {
  switch (i) {
    case 0: if (valid_timing(v)) { timing_ = v; timing_knob_.set(timing_index(v), 4); } break;
    case 1: pm_ = clampi(v, 0, 2); pm_knob_.set(pm_, 4); break;
    case 2: eucl_ = clampi(v, 0, 47); epos_ = -1; break;
    case 3: eucl_off_ = clampi(v, 0, 63); epos_ = -1; break;
    case 4: swing_ = clampi(v, 20, 80); break;
    case 5: hold_ = v != 0; hold_knob_.set(hold_, 4); break;
    case 6: crank_ = v != 0; break;
    default: break;
  }
}
void EndlessSeq::edit(int i, int v) noexcept {
  if (i == 1) { count_ = 0; cur_ = -1; nbag_ = 0; return; }
  if (i == 0 || i == 2) {
    if (count_ >= 128) return;
    Step& st = steps_[count_++];
    st.n = 0;
    if (i == 2 && v != kRest) {
      st.key[0] = int8_t(clampi(v & 0xFF, 0, 127));
      st.len[0] = int8_t(clampi((v >> 8) & 0x7F, 1, 127));
      st.n = 1;
    }
    return;
  }
  if (i == 3 && count_ > 0) {
    Step& st = steps_[count_ - 1];
    if (st.n < 4) {
      st.key[st.n] = int8_t(clampi(v & 0xFF, 0, 127));
      st.len[st.n] = int8_t(clampi((v >> 8) & 0x7F, 1, 127));
      ++st.n;
    }
  }
}
void EndlessSeq::encoder(int i, int d, Emitter& out) noexcept {
  switch (i) {
    case 0:
      if (crank_) {  // crank: 6 detents per step, by hand
        crank_acc_ += d;
        while (crank_acc_ >= 6) { crank_acc_ -= 6; do_step(out); }
        while (crank_acc_ <= -6) {
          crank_acc_ += 6;
          const int pm = pm_;
          pm_ = pm == 2 ? 0 : (pm == 0 ? 2 : pm);  // backwards swaps rev and fwd
          do_step(out);
          pm_ = pm;
        }
      } else {
        timing_ = timing_ticks(timing_knob_.turn(d, 0, 6, 4));
      }
      break;
    case 1: if (!crank_ && count_) swing_ = clampi(swing_ + d, 20, 80); break;
    case 2: eucl_ = clampi(eucl_ + d, 0, 47); epos_ = -1; break;
    case 3:
      if (!crank_ && count_) {
        hold_ = hold_knob_.turn(d, 0, 1, 4) != 0;
        if (!hold_ && held_.n == 0) stop(out);
      }
      break;
    case 4:
      if (d > 0) { crank_ = true; hold_ = false; hold_knob_.set(0, 4); running_ = true; pending_ = false; }
      else { crank_ = false; stop(out); }
      break;
    case 6: {
      const int len = euclid_len(eucl_);
      if (len > 0) eucl_off_ = ((eucl_off_ + d) % len + len) % len;
      epos_ = -1;
      break;
    }
    case 7: pm_ = pm_knob_.turn(d, 0, 2, 4); nbag_ = 0; break;
    default: break;
  }
}
void EndlessSeq::key_edit(int key, bool down, Emitter& out) noexcept {
  if (key < 0) {  // shift released: the recording session ends
    recording_ = false;
    rec_clear_ = true;
    rec_held_ = 0;
    return;
  }
  const int n = kKeyBase + clampi(key, 0, 23);
  if (!down) {
    out.off(n);
    if (rec_held_ > 0) --rec_held_;
    return;
  }
  if (running_) return;  // the firmware records only while stopped
  recording_ = true;
  if (rec_clear_) { count_ = 0; cur_ = -1; rec_clear_ = false; }
  if (rec_held_ == 0) edit(2, key | (1 << 8));  // a new step
  else edit(3, key | (1 << 8));                 // joins the step (a chord, up to 4 notes)
  ++rec_held_;
  out.on(n, kVelocity, 0);  // monitor
}
void EndlessSeq::release(Emitter& out) noexcept {
  held_.clear();
  stop(out);
}
void EndlessSeq::view(int32_t* d) const noexcept {
  d[0] = count_;
  d[1] = timing_;
  d[2] = recording_;
  d[3] = cur_;
  d[4] = pm_;
  d[5] = eucl_;
  d[6] = eucl_off_;
  d[7] = hold_;
  d[8] = crank_;
  d[9] = swing_;
  for (int i = 0; i < 16; ++i) d[10 + i] = i < count_ ? (steps_[i].n ? steps_[i].key[0] : -1) : -2;
}
void EndlessSeq::get_params(int32_t* p) const noexcept {
  p[0] = timing_; p[1] = pm_; p[2] = eucl_; p[3] = eucl_off_; p[4] = swing_; p[5] = hold_; p[6] = crank_;
}

// ------------------------------------------------------------------ tombola

namespace {
constexpr float kR = 6.0f;                    // hexagon circumradius (0x60000 16.16)
constexpr float kApothem = kR * 0.8660254f;   // cos 30
constexpr float kBall = 0.4f;                 // ball radius (I: not decoded)
constexpr float kDt = 64.0f / 44100.0f;       // one 64-frame control block
constexpr float kHit2 = 25.0f;                // v^2 >= 25: a contact sounds
constexpr float kPi = 3.14159265f;
constexpr int kTombolaGate = 20;              // ~102 ms at 125 BPM (I: the firmware counts 102 time units)
}  // namespace

void TombolaSeq::key(const Event& e, Emitter& out) noexcept {
  if (!is_on(e)) return;  // note offs are swallowed: a ball keeps its note
  int i;
  if (n_ < kMax) i = n_++;
  else { i = next_ % kMax; ++next_; out.off(b_[i].note); }
  Ball& b = b_[i];
  b.x = (float(int(lcg_next(lcg_) % 2001)) / 1000.0f - 1.0f) * 0.1f;  // near the centre, a little jitter
  b.y = (float(int(lcg_next(lcg_) % 2001)) / 1000.0f - 1.0f) * 0.1f;
  b.vx = b.vy = 0.0f;
  b.note = e.a;
  for (bool& t : b.touching) t = false;
}
void TombolaSeq::control(Emitter& out) noexcept {
  if (!crank_) angle_ += spin_ * kDt;
  if (angle_ > 2 * kPi) angle_ -= 2 * kPi;
  if (angle_ < 0) angle_ += 2 * kPi;
  const float th0 = angle_ + tilt_;
  float ux[6], uy[6];
  for (int i = 0; i < 6; ++i) {
    const float th = th0 + float(i) * (kPi / 3) + kPi / 6;  // outward normal of wall i
    ux[i] = std::cos(th);
    uy[i] = std::sin(th);
  }
  const float lim = kApothem - kBall;
  const float g = 2.0f * gravity_;
  auto hit = [&](Ball& b, float v2) {
    if (v2 < kHit2) return;
    const int vel = int(v2 / 4.0f) > 127 ? 127 : (int(v2 / 4.0f) < 1 ? 1 : int(v2 / 4.0f));
    out.on(b.note, vel, kTombolaGate);
    ++hits_;
  };
  for (int k = 0; k < n_; ++k) {
    Ball& b = b_[k];
    b.vy -= g * kDt;
    b.x += b.vx * kDt;
    b.y += b.vy * kDt;
    for (int i = 0; i < 6; ++i) {
      const float d = b.x * ux[i] + b.y * uy[i];
      if (d <= lim) { b.touching[i] = false; continue; }
      const float wspin = crank_ ? 0.0f : spin_;
      const float wx = -wspin * b.y, wy = wspin * b.x;  // wall velocity at the contact (rotation)
      float rx = b.vx - wx, ry = b.vy - wy;
      const float rn = rx * ux[i] + ry * uy[i];
      if (rn > 0) {
        if (!b.touching[i]) hit(b, rx * rx + ry * ry);  // a new contact (Box2D Add, not Persist)
        rx -= (1 + restitution_) * rn * ux[i];
        ry -= (1 + restitution_) * rn * uy[i];
        b.vx = rx + wx;
        b.vy = ry + wy;
      }
      b.touching[i] = true;
      b.x -= (d - lim) * ux[i];
      b.y -= (d - lim) * uy[i];
    }
    if (!(std::fabs(b.vx) < 200.0f) || !(std::fabs(b.vy) < 200.0f) || !(std::fabs(b.x) < kR) ||
        !(std::fabs(b.y) < kR)) {  // never let a ball escape or blow up
      b.x = b.y = b.vx = b.vy = 0.0f;
    }
  }
  // ball against ball: equal masses, the walls' restitution (Box2D mixes restitution as the max)
  for (int a = 0; a < n_; ++a)
    for (int c = a + 1; c < n_; ++c) {
      Ball& p = b_[a];
      Ball& q = b_[c];
      const float dx = q.x - p.x, dy = q.y - p.y, d2 = dx * dx + dy * dy;
      if (d2 >= 4 * kBall * kBall || d2 <= 1e-8f) continue;
      const float dist = std::sqrt(d2), nx = dx / dist, ny = dy / dist;
      const float rvx = p.vx - q.vx, rvy = p.vy - q.vy, rn = rvx * nx + rvy * ny;
      if (rn > 0) {
        const float v2 = rvx * rvx + rvy * rvy;
        if (v2 >= kHit2) { hit(p, v2); hit(q, v2); }
        const float j = (1 + restitution_) * rn / 2;
        p.vx -= j * nx; p.vy -= j * ny;
        q.vx += j * nx; q.vy += j * ny;
      }
      const float push = (2 * kBall - dist) / 2;
      p.x -= push * nx; p.y -= push * ny;
      q.x += push * nx; q.y += push * ny;
    }
}
void TombolaSeq::param(int i, int v) noexcept {
  switch (i) {
    case 0: gravity_ = float(clampi(v, 0, 20000)) / 1000.0f; break;
    case 1: restitution_ = float(clampi(v, 0, 1500)) / 1000.0f; break;
    case 2: spin_ = float(clampi(v, -10000, 10000)) / 1000.0f; break;
    case 3: tilt_ = float(clampi(v, 0, 4712)) / 1000.0f; break;
    default: break;
  }
}
void TombolaSeq::edit(int i, int) noexcept {
  if (i == 0) n_ = next_ = 0;
}
void TombolaSeq::encoder(int i, int d, Emitter&) noexcept {
  switch (i) {
    case 0:
      if (crank_) angle_ += float(d) * 0.05f;  // crank: turn the hexagon by hand
      else spin_ = std::fmax(-10.0f, std::fmin(10.0f, spin_ + float(d) * 0.25f));
      break;
    case 1: gravity_ = std::fmax(0.0f, std::fmin(20.0f, gravity_ + float(d) * 0.25f)); break;
    case 2: tilt_ = std::fmax(0.0f, std::fmin(4.71239f, tilt_ + float(d) * 0.05f)); break;
    case 3: restitution_ = std::fmax(0.0f, std::fmin(1.5f, restitution_ + float(d) * 0.015f)); break;
    case 4: crank_ = d > 0; break;
    default: break;
  }
}
void TombolaSeq::release(Emitter& out) noexcept {
  for (int k = 0; k < n_; ++k) out.off(b_[k].note);
  n_ = next_ = 0;
}
void TombolaSeq::view(int32_t* d) const noexcept {
  d[0] = int32_t((angle_ + tilt_) * 1000.0f);
  d[1] = n_;
  for (int k = 0; k < kMax; ++k) {
    const int16_t x = int16_t(k < n_ ? b_[k].x * 1000.0f : 0), y = int16_t(k < n_ ? b_[k].y * 1000.0f : 0);
    d[2 + k] = int32_t(uint32_t(uint16_t(x)) | (uint32_t(uint16_t(y)) << 16));
  }
  for (int k = 0; k < 8; ++k) d[18 + k] = k < n_ ? b_[k].note : -1;
}
void TombolaSeq::get_params(int32_t* p) const noexcept {
  p[0] = int32_t(gravity_ * 1000.0f + 0.5f);
  p[1] = int32_t(restitution_ * 1000.0f + 0.5f);
  p[2] = int32_t(std::lround(spin_ * 1000.0f));
  p[3] = int32_t(std::lround(tilt_ * 1000.0f));
}

// ------------------------------------------------------------------ finger

namespace {
constexpr int8_t kFingerSlot[24] = {0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9, -1, 10, 11, -1, 12, -1, 13};
}  // namespace

int FingerSeq::slot_of_key(int key) noexcept { return key >= 0 && key < 24 ? kFingerSlot[key] : -1; }

FingerSeq::FingerSeq() noexcept {
  for (auto& slot : notes_)
    for (auto& st : slot) st[0] = st[1] = -1;
  for (int& l : length_) l = 8;
  for (auto& st : clip_) st[0] = st[1] = -1;
  pm_knob_.set(pm_, 4);
}
void FingerSeq::stop(Emitter& out) noexcept {
  running_ = pending_ = false;
  slot_[0] = slot_[1] = -1;
  snd_.all_off(out);
}
void FingerSeq::key(const Event& e, Emitter& out) noexcept {
  const int k = key_of(e);
  const int slot = slot_of_key(k);
  if (is_on(e)) {
    if (slot < 0) return;  // black keys do nothing in finger (0x019ED95A)
    if (slot_[1] == slot) {
      slot_[1] = -1;
    } else if (slot_[0] == slot) {  // pressing a playing slot again (hold) turns it off
      slot_[0] = slot_[1];
      step_[0] = step_[1];
      slot_[1] = -1;
    } else if (held_.n == 0) {  // a fresh start: player A from step 0
      slot_[0] = slot;
      slot_[1] = -1;
      step_[0] = step_[1] = 0;
      edited_ = 0;
      if (!running_) { pending_ = true; clk_.restart(); }
    } else {  // a second key: A if it is free, else B (replacing B's slot)
      const int a = slot_[0] < 0 ? 0 : 1;
      slot_[a] = slot;
      edited_ = a;
    }
    selected_ = slot;
    held_.add(k);
    return;
  }
  if (!is_off(e) || slot < 0) return;
  held_.remove(k);
  if (hold_) return;
  if (held_.n == 0) { stop(out); return; }
  if (slot_[1] == slot) slot_[1] = -1;
  else if (slot_[0] == slot) { slot_[0] = slot_[1]; step_[0] = step_[1]; slot_[1] = -1; }
}
void FingerSeq::trigger(int slot, int step, Emitter& out) noexcept {
  if (slot < 0) return;
  for (int h = 0; h < 2; ++h) {
    const int n = notes_[slot][step % kSteps][h];
    if (n >= 0) {
      out.on(n, kVelocity, 0);
      snd_.add(n, 1);
    }
  }
}
void FingerSeq::tick(uint64_t t, Emitter& out) noexcept {
  if (pending_ && on_grid(t, out)) {
    pending_ = false;
    running_ = true;
    step_[0] = step_[1] = 0;
    clk_.restart();
  }
  if (!running_) return;
  if (slot_[0] < 0 && slot_[1] < 0) { stop(out); return; }
  if (!clk_.tick(kTicks16th, swing_)) return;
  snd_.all_off(out);  // the gate is one step
  auto len = [&](int a) { return slot_[a] >= 0 ? length_[slot_[a]] : 1; };
  switch (pm_) {
    case 0:  // replace: only A; at its end B (if waiting) takes over
      trigger(slot_[0], step_[0], out);
      if (++step_[0] >= len(0)) {
        step_[0] = 0;
        if (slot_[1] >= 0) { slot_[0] = slot_[1]; slot_[1] = -1; edited_ = 0; }
      }
      break;
    case 2:  // fill in: B plays once while A pauses, then A resumes
      if (slot_[1] >= 0) {
        trigger(slot_[1], step_[1], out);
        if (++step_[1] >= len(1)) { step_[1] = 0; slot_[1] = -1; edited_ = 0; selected_ = slot_[0] >= 0 ? slot_[0] : selected_; }
      } else {
        trigger(slot_[0], step_[0], out);
        if (++step_[0] >= len(0)) step_[0] = 0;
      }
      break;
    default:  // join: both, each looping its own length
      trigger(slot_[0], step_[0], out);
      trigger(slot_[1], step_[1], out);
      if (slot_[0] >= 0 && ++step_[0] >= len(0)) step_[0] = 0;
      if (slot_[1] >= 0 && ++step_[1] >= len(1)) step_[1] = 0;
      break;
  }
}
void FingerSeq::param(int i, int v) noexcept {
  if (i == 0) { pm_ = clampi(v, 0, 2); pm_knob_.set(pm_, 4); }
  else if (i == 1) swing_ = clampi(v, 20, 80);
  else if (i >= 2 && i < 2 + kSlots) length_[i - 2] = clampi(v, 1, kSteps);
  else if (i == 16) hold_ = v != 0;
}
void FingerSeq::edit(int i, int v) noexcept {
  if (i < 0) return;
  const int slot = i / 64, step = (i % 64) / 2, hand = i % 2;
  if (slot >= kSlots) return;
  notes_[slot][step][hand] = int8_t(clampi(v, -1, 127));
}
void FingerSeq::encoder(int i, int d, Emitter& out) noexcept {
  const int len = length_[selected_];
  auto wrap = [&](int c) { return ((c % len) + len) % len; };
  switch (i) {
    case 0: cursor_ = wrap(cursor_ + d); break;
    case 1: swing_ = clampi(swing_ + d, 20, 80); break;
    case 2:
      length_[selected_] = clampi(length_[selected_] + d, 1, kSteps);
      if (cursor_ > length_[selected_] - 1) cursor_ = length_[selected_] - 1;
      break;
    case 3:
      if (d > 0) {
        hold_ = true;
      } else {
        hold_ = false;
        cursor_ = step_[edited_] % length_[selected_];
        if (held_.n == 0) stop(out);
      }
      break;
    case 4:
      notes_[selected_][cursor_][0] = notes_[selected_][cursor_][1] = -1;
      cursor_ = wrap(cursor_ + (d > 0 ? 1 : -1));
      break;
    case 7: pm_ = pm_knob_.turn(d, 0, 2, 4); break;
    default: break;  // shift+green, shift+white: nothing
  }
}
void FingerSeq::key_edit(int key, bool down, Emitter& out) noexcept {
  if (key < 0) {  // shift released
    if (entry_ > 0) {
      entry_ = 0;
      if (!running_) cursor_ = (cursor_ + 1) % length_[selected_];
    }
    return;
  }
  const int n = clampi(kKeyBase + clampi(key, 0, 23) + 12 * octave_, 0, 127);
  if (!down) {
    out.off(n);
    if (entry_ > 0 && --entry_ == 0 && !running_) cursor_ = (cursor_ + 1) % length_[selected_];
    return;
  }
  cursor_ = clampi(cursor_, 0, length_[selected_] - 1);
  const int step = running_ ? step_[edited_] % length_[selected_] : cursor_;
  int8_t* st = notes_[selected_][step];
  if (st[0] < 0) st[0] = int8_t(n);
  else if (st[1] < 0) st[1] = int8_t(n);
  ++entry_;
  if (!running_) out.on(n, kVelocity, 0);  // audition
}
void FingerSeq::nav(int which, bool shift, Emitter&) noexcept {
  const int len = length_[selected_];
  switch (which) {
    case 0:
      if (shift) cursor_ = (cursor_ - 1 + len) % len;
      else octave_ = clampi(octave_ - 1, -4, 4);
      break;
    case 1:
      if (shift) cursor_ = (cursor_ + 1) % len;
      else octave_ = clampi(octave_ + 1, -4, 4);
      break;
    case 2:  // lift: copy the selected slot
      clip_len_ = len;
      for (int s = 0; s < kSteps; ++s) { clip_[s][0] = notes_[selected_][s][0]; clip_[s][1] = notes_[selected_][s][1]; }
      break;
    case 3:  // drop: paste
      if (clip_len_ > 0) {
        length_[selected_] = clip_len_;
        for (int s = 0; s < kSteps; ++s) { notes_[selected_][s][0] = clip_[s][0]; notes_[selected_][s][1] = clip_[s][1]; }
      }
      break;
    default: break;
  }
}
void FingerSeq::release(Emitter& out) noexcept {
  held_.clear();
  hold_ = false;
  stop(out);
}
void FingerSeq::view(int32_t* d) const noexcept {
  d[0] = slot_[0];
  d[1] = slot_[1];
  for (int i = 0; i < kSlots; ++i) d[2 + i] = length_[i];
  d[16] = cursor_;
  d[17] = selected_;
  d[18] = hold_;
  d[19] = pm_;
  d[20] = step_[0];
  d[21] = step_[1];
  d[22] = octave_;
  d[23] = swing_;
  uint32_t m = 0;
  for (int s = 0; s < kSteps; ++s)
    if (notes_[selected_][s][0] >= 0 || notes_[selected_][s][1] >= 0) m |= 1u << s;
  d[24] = int32_t(m);
}
void FingerSeq::get_params(int32_t* p) const noexcept {
  p[0] = pm_; p[1] = swing_;
  for (int i = 0; i < kSlots; ++i) p[2 + i] = length_[i];
  // p[16] would be hold; SeqView::params has 16 slots, so hold is in the view (d18)
}

// ------------------------------------------------------------------ sketch

SketchSeq::SketchSeq() noexcept {
  for (auto& p : pts_) p = -1;
}
void SketchSeq::stop(Emitter& out) noexcept {
  running_ = false;
  if (sounding_ >= 0) out.off(sounding_);
  sounding_ = -1;
}
void SketchSeq::key(const Event& e, Emitter& out) noexcept {
  const int k = key_of(e);
  if (is_on(e)) {
    held_note_[k] = e.a;
    const bool first = held_.add(k);
    if (first && !running_) {
      running_ = true;
      origin_ = out.tick;
      pos_ = 0;
      sounding_ = -1;
    }
    return;
  }
  if (!is_off(e)) return;
  if (!held_.has(k)) { out.pass(e); return; }
  held_.remove(k);
  if (held_.n == 0 && !hold_) stop(out);
}
void SketchSeq::tick(uint64_t t, Emitter& out) noexcept {
  if (!running_ || t < origin_) return;
  // 288 positions per bar of 384 ticks (pos = tick x 0.75), at (speed + 1) / 3 bars per bar (I)
  const uint64_t rel = (t - origin_) * uint64_t(speed_ + 1);
  const int pos = int((rel * 3 / 4 / 3) % kPos);
  if (pos == pos_ && sounding_ >= 0) return;
  pos_ = pos;
  const int n = pts_[pos_];
  const int tr = out.drum ? 0 : transpose_of(held_, held_note_);
  const int note = n < 0 ? -1 : clampi(kKeyBase + n + tr, 0, 127);
  if (note == sounding_) return;  // a flat line is one held note
  if (sounding_ >= 0) out.off(sounding_);
  if (note >= 0) out.on(note, kVelocity, 0);
  sounding_ = note;
}
void SketchSeq::param(int i, int v) noexcept {
  if (i == 1) speed_ = clampi(v, 0, 10);
  else if (i == 2) grid_ = clampi(v, 0, 3);
  else if (i == 3) hold_ = v != 0;
}
void SketchSeq::edit(int i, int v) noexcept {
  if (i >= 0 && i < kPos) pts_[i] = int8_t(v < 0 ? -1 : clampi(v, 0, 23));
}
void SketchSeq::encoder(int i, int d, Emitter& out) noexcept {
  switch (i) {
    case 0: pen_ = ((pen_ + d * 6) % kPos + kPos) % kPos; last_drawn_ = -1; break;
    case 1: speed_ = clampi(speed_ + d, 0, 10); break;
    case 2: grid_ = clampi(grid_ + d, 0, 3); break;
    case 3:
      hold_ = d > 0;
      if (!hold_ && held_.n == 0) stop(out);
      break;
    case 4:  // erase at the pen and move
      for (int k = 0; k < 6; ++k) pts_[(pen_ + k) % kPos] = -1;
      pen_ = ((pen_ + (d > 0 ? 6 : -6)) % kPos + kPos) % kPos;
      last_drawn_ = -1;
      break;
    default: break;
  }
}
void SketchSeq::key_edit(int key, bool down, Emitter& out) noexcept {
  if (key < 0) { entry_ = 0; last_drawn_ = -1; return; }
  const int n = clampi(key, 0, 23);
  if (!down) { out.off(kKeyBase + n); return; }
  // draw a line from the last point to the pen at this note, then move the pen one sixteenth (18 positions)
  const int from = last_drawn_ >= 0 ? last_drawn_ : pen_;
  for (int p = from; p != (pen_ + 18) % kPos; p = (p + 1) % kPos) pts_[p] = int8_t(n);
  last_drawn_ = (pen_ + 18) % kPos;
  pen_ = last_drawn_;
  ++entry_;
  out.on(kKeyBase + n, kVelocity, 0);
}
void SketchSeq::release(Emitter& out) noexcept {
  held_.clear();
  hold_ = false;
  stop(out);
}
void SketchSeq::view(int32_t* d) const noexcept {
  int count = 0;
  for (int8_t p : pts_) count += p >= 0;
  d[0] = count;
  d[1] = speed_;
  for (int i = 0; i < 24; ++i) d[2 + i] = pts_[i * 12];
  d[26] = pen_;
  d[27] = grid_;
}
void SketchSeq::get_params(int32_t* p) const noexcept {
  int count = 0;
  for (int8_t q : pts_) count += q >= 0;
  p[0] = count; p[1] = speed_; p[2] = grid_; p[3] = hold_;
}

// ------------------------------------------------------------------ arpeggio

namespace {
// op = the octave pattern (table 0x018FAB84, lengths 0x018FAC04)
constexpr int kArpPat[4][5] = {{12, 0, 0, 0, 0}, {12, -12, 0, 0, 0}, {7, 9, -5, 0, 0}, {2, 4, 5, 7, 9}};
constexpr int kArpLen[4] = {1, 2, 3, 5};
}  // namespace

ArpSeq::ArpSeq() noexcept {
  timing_knob_.set(timing_index(timing_), 4);
  octave_knob_.set(octave_, 4);
  op_knob_.set(op_, 4);
  sm_knob_.set(sm_, 4);
}
void ArpSeq::key(const Event& e, Emitter& out) noexcept {
  const int n = e.a & 127;
  if (is_on(e)) {
    const bool first = held_.add(n);
    if (!first) { latched_.add(n); return; }
    pending_ = true;  // the start waits for the 1/8 grid (at once when nothing else runs)
    latched_.clear();
    latched_.add(n);
    return;
  }
  if (!is_off(e)) return;
  if (!held_.has(n)) { out.pass(e); return; }
  held_.remove(n);
  if (hold_) return;
  latched_.remove(n);
  if (held_.n == 0) {
    playing_ = pending_ = false;
    latched_.clear();
  }
}
int ArpSeq::next_offset() noexcept {
  const int o = op_ & 3;
  if (pidx_ >= kArpLen[o]) pidx_ = 0;
  return kArpPat[o][pidx_++];
}
void ArpSeq::advance() noexcept {
  const KeySet& set = hold_ ? latched_ : held_;
  const int n = set.n;
  if (n == 0) return;
  switch (octave_) {
    case 1:  // once: the last note again, transposed
      if (s_ == n - 1 && t_ == 0) { t_ = next_offset(); return; }
      break;
    case 2:  // each: every note followed by a transposed copy
      if (t_ == 0) { t_ = next_offset(); return; }
      break;
    default: break;
  }
  s_ = s_ + 1 >= n ? 0 : s_ + 1;
  if (octave_ == 3) {  // all: every other run transposed
    if (s_ >= n - 1) flag_ = !flag_;
    t_ = flag_ ? next_offset() : 0;
  } else {
    t_ = 0;
  }
}
void ArpSeq::play_note(const KeySet& set, Emitter& out) noexcept {
  if (set.n == 0) return;
  if (s_ >= set.n) s_ = 0;
  const int note = clampi(set.k[s_] + (out.drum ? 0 : t_), 0, 127);
  out.on(note, kVelocity, length_);  // the emitter ends a still-sounding copy first
  pcs_ = 1u << (note % 12);
  if (octave_ == 4) {  // trig: an extra chord note on some steps (c 0 -> set[0], c 2 -> set[2])
    int extra = -1;
    if (c_ == 0) extra = set.k[0];
    else if (c_ == 2 && set.n > 2) extra = set.k[2];
    if (extra >= 0 && extra != set.k[s_]) {
      out.on(clampi(extra, 0, 127), kVelocity, length_);
      pcs_ |= 1u << (extra % 12);
    }
  }
}
void ArpSeq::do_step(Emitter& out) noexcept {
  const KeySet& set = hold_ ? latched_ : held_;
  const int mask = 16 - clampi(skip_, 1, 15);
  auto bit = [&]() {
    const int b = (mask >> (3 - c_)) & 1;
    c_ = (c_ + 1) & 3;
    return b;
  };
  if (sm_ == 0) {  // skip: masked positions are jumped over (no gap)
    play_note(set, out);
    advance();
    for (int guard = 0; guard < 4 && bit() == 0; ++guard) advance();
  } else {  // pause: a masked position is a rest
    if (bit() == 0) return;
    play_note(set, out);
    advance();
  }
}
void ArpSeq::reset(Emitter&) noexcept {
  clk_.restart();
  c_ = 0;
  flag_ = false;
  t_ = 0;
  pidx_ = 0;
}
void ArpSeq::tick(uint64_t t, Emitter& out) noexcept {
  if (pending_ && on_grid(t, out)) {
    pending_ = false;
    playing_ = true;
    reset(out);
  }
  if (!playing_) {
    if (!hold_ && held_.n == 0) latched_.clear();
    return;
  }
  if (clk_.tick(timing_, swing_)) do_step(out);
}
void ArpSeq::param(int i, int v) noexcept {
  switch (i) {
    case 0: op_ = clampi(v, 0, 3); op_knob_.set(op_, 4); break;
    case 1: octave_ = clampi(v, 0, 4); octave_knob_.set(octave_, 4); break;
    case 2: if (valid_timing(v)) { timing_ = v; timing_knob_.set(timing_index(v), 4); } break;
    case 3: swing_ = clampi(v, 20, 80); break;
    case 4: length_ = clampi(v, 1, 64); break;
    case 5: skip_ = clampi(v, 1, 15); break;
    case 6: sm_ = clampi(v, 0, 1); sm_knob_.set(sm_, 4); break;
    case 7: hold_ = v != 0; break;
    default: break;
  }
}
void ArpSeq::encoder(int i, int d, Emitter&) noexcept {
  switch (i) {
    case 0: timing_ = timing_ticks(timing_knob_.turn(d, 0, 6, 4)); break;
    case 1: octave_ = octave_knob_.turn(d, 0, 4, 4); break;
    case 2: skip_ = clampi(skip_ + d, 1, 15); break;
    case 3:
      if (d > 0) {
        hold_ = true;
      } else {
        if (held_.n == 0) playing_ = pending_ = false;
        hold_ = false;
      }
      break;
    case 4: length_ = clampi(length_ + d, 1, 64); break;
    case 5: op_ = op_knob_.turn(d, 0, 3, 4); break;
    case 6: sm_ = sm_knob_.turn(d, 0, 1, 4); break;
    case 7: swing_ = clampi(swing_ + d, 20, 80); break;
    default: break;
  }
}
void ArpSeq::release(Emitter& out) noexcept {
  held_.clear();
  latched_.clear();
  playing_ = pending_ = hold_ = false;
  snd_.all_off(out);
}
void ArpSeq::view(int32_t* d) const noexcept {
  const KeySet& set = hold_ ? latched_ : held_;
  d[0] = set.n;
  d[1] = op_;
  d[2] = octave_;
  for (int i = 0; i < 16; ++i) d[3 + i] = i < set.n ? set.k[i] : -1;
  d[19] = timing_;
  d[20] = skip_;
  d[21] = sm_;
  d[22] = hold_;
  d[23] = length_;
  d[24] = swing_;
  d[25] = c_;
  d[26] = int32_t(playing_ ? pcs_ : 0);
}
void ArpSeq::get_params(int32_t* p) const noexcept {
  p[0] = op_; p[1] = octave_; p[2] = timing_; p[3] = swing_; p[4] = length_; p[5] = skip_; p[6] = sm_; p[7] = hold_;
}

}  // namespace op1::seq
