// The six OG sequencers (owner: t5): pattern, endless, tombola, finger, sketch, arpeggio. Each one is a fixed-size
// object (no allocation): every side (synth, drum) owns one of each and the selected one is active.
//
// t5 (2026-10-23) rebuilt them on the firmware's players and controllers (decoded from R.00246 in the t5 notes
// decode-finger-pattern-sketch.md and decode-arp-endless-tombola.md; F = seen in the code, I = this port's reading).
// The common rules (F):
//   - clock 96 ticks per quarter (Timing.cpp table {96, 48, 32, 24, 16, 12, 8} = 1/4 .. 1/32t);
//   - a player starts on the next tick of the 48-tick (1/8) grid; the first key while nothing runs restarts that
//     grid, so then it starts at once (the Emitter's `grid`);
//   - swing: steps alternate A = T * swing / 50 and 2T - A (A first), swing 20..80, 50 = none;
//   - notes go out with velocity 127;
//   - the controllers read encoder events, index 0..3 = blue, green, white, orange and 4..7 = the same with shift;
//     several knobs need 4 detents per value ("x4" below);
//   - "transposed": a note plays shifted by (the most recently pressed key still held - key 7), i.e. holding C4
//     (note 60 at octave 0) plays as written. Here: + (last held note - 60).
//
// Per sequencer (F unless marked):
//   pattern   16 steps x 24 keys (bit k = keyboard key k = note 53 + k), length 1..16, start 0..15 (start + length
//             <= 16), pm 0 rev / 1 cycle (ping-pong, ends not repeated) / 2 fwd, step 1/16 with swing, gate one
//             step. Holding any key plays it (synth side transposed; drum side as written, I); latch with orange.
//             An empty pattern lets keys play normally. Encoders: blue cursor, green swing, white length,
//             orange right latch+start / left stop, shift+blue clear step, shift+green rotate, shift+white start,
//             shift+orange pm x4. Shift + key toggles that key at the cursor (at the playing step while running);
//             releasing the last entry key moves the cursor on. << >> move the cursor.
//   finger    14 slots on the 14 white keys (table 0x018E5F20), black keys do nothing, up to 32 steps x 2 notes
//             (absolute notes), length 1..32 per slot (default 8), pm 0 replace / 1 join (default) / 2 fill in,
//             two players A and B: the first held key gives A its slot from step 0, a second gives B its slot, a
//             third replaces B's; releasing all keys stops unless hold. Step 1/16 with swing, gate one step.
//             Encoders: blue cursor, green swing, white slot length, orange right hold / left hold off + stop,
//             shift+blue erase step, shift+orange pm x4. Shift + key writes 53 + key + 12 x finger octave into the
//             selected slot at the cursor (or the playing step), hand 0 then 1; releasing moves the cursor on.
//             << >> finger octave -4..4, shift+<< >> cursor, lift / drop copy / paste the slot.
//   endless   up to 128 steps, each up to 4 notes (keyboard key indices) with a length in steps, or a rest;
//             timing (1/4..1/32t), swing (only at 1/8 and 1/16), playmode 0 rev / 1 shuffle bag / 2 fwd,
//             euclidean 0 off / 1..47 a fixed rhythm table (a 0 is a rest that does not advance), offset = start
//             in the rhythm. Plays while keys are held (transposed), hold with orange; crank mode (shift+blue)
//             steps by hand with blue, 6 detents per step. Encoders: blue timing x4, green swing, white
//             euclidean, orange hold x4, shift+blue crank on/off, shift+white offset, shift+orange playmode x4.
//             Recording (I: the hint "shiftkey"): shift + keys, while stopped. The first key of a session clears
//             the sequence; a key while no other entry key is held starts a step, keys while one is held join it.
//   tombola   balls in a spinning hexagon (circumradius 6, walls 1.1 x side), gravity (0, -2g), wall restitution,
//             a ball sounds on a new contact faster than 5 m/s (ball-ball too), velocity min(v^2/4, 127), up to
//             16 balls, a key drops a ball with its note near the centre. Encoders: blue spin (+-10 rad/s), green
//             gravity 0..20, white angle 0..3pi/2, orange restitution 0..1.5, shift+blue crank. The firmware runs
//             Box2D 2.0 in fixed point; this is our own small physics with those numbers, so hit timing is close,
//             not exact (I). The spin step per detent, the ball radius and what the angle does are not decoded (I).
//   sketch    288 positions per bar (pos = tick x 0.75), 24 note rows; speed knob 0..10, grid type 0..3. How the
//             device draws, the speed multipliers and the grid types are not decoded (I: shift + key draws that
//             note at the pen, blue moves the pen, shift+blue erases, green speed, white grid type, orange hold;
//             speed s plays (s + 1) / 3 bars per bar). Plays while keys are held (transposed).
//   arpeggio  notes in the order the keys were pressed (no sorting); timing (default 48 = 1/8); octave mode 0 none
//             / 1 once (default) / 2 each / 3 all / 4 trig; op = an octave pattern of semitone offsets 0 {+12}, 1
//             {+12, -12}, 2 {+7, +9, -5}, 3 {+2, +4, +5, +7, +9}; skip 1..15: a 4-step mask 16 - skip, MSB first;
//             sm 0 skip (masked steps jumped) / 1 pause (rests); length = the gate in ticks (default 12); swing.
//             Plays while keys are held, or latched with hold (orange). Encoders: blue timing x4, green octave
//             x4, white skip, orange hold, shift+blue length, shift+green op x4, shift+white sm x4, shift+orange
//             swing. Drum side: the octave pattern does not transpose.
//
// Commands (Command.slot = side): SeqOp::SetParam / SetParamOf (index, value) set a stored value, SeqOp::Edit /
// EditOf (index, value) the data, SeqOp::Encoder (index = 0..7, value = detents), SeqOp::KeyEdit (index = key
// 0..23, value 1 down / 0 up; index -1 = shift released), SeqOp::Nav (index 0 <<, 1 >>, 2 lift, 3 drop; value =
// shift). SetParam indices (= get_params order):
//   pattern   0 length, 1 pm, 2 swing, 3 start, 4 latch
//   endless   0 timing (ticks), 1 playmode, 2 euclidean, 3 offset, 4 swing, 5 hold, 6 crank
//   tombola   0 gravity (milli), 1 restitution (milli), 2 spin (milli-rad/s), 3 angle (milli-rad)
//   finger    0 pm, 1 swing, 2..15 length of slot 0..13, 16 hold
//   sketch    0 point count (stored only), 1 speed 0..10, 2 grid type 0..3, 3 hold
//   arpeggio  0 op, 1 octave mode, 2 timing (ticks), 3 swing, 4 length (gate ticks), 5 skip, 6 sm, 7 hold
// Edit indices:
//   pattern   index = step 0..15, value = key 0..23 (toggle); index = 256 + step, value = the whole mask
//   endless   0 = append a rest (value kRest) ; 1 = clear; 2 = append a step with one note, value = key | len << 8;
//             3 = add a note to the last step (chord), value = key | len << 8
//   tombola   0 = remove all balls
//   finger    index = slot * 64 + step * 2 + hand, value = note (-1 = empty)
//   sketch    index = position 0..287, value = note 0..23 or -1
//
// View (SeqView::display, 32 int32 per side: synth = 0..31, drum = 32..63): [0] enabled, [1] SeqType, [2] current
// step/position (-1 = idle), [3] playing (0/1), [4..31] per type (d[k] = display[4 + k]):
//   pattern  d0 length, d1..16 masks of steps 0..15, d17 cursor, d18 start, d19 pm, d20 swing, d21 latch
//   endless  d0 step count, d1 timing, d2 recording, d3 current step, d4 playmode, d5 euclidean, d6 offset, d7 hold,
//            d8 crank, d9 swing, d10..25 first note (key index) of steps 0..15 (-1 rest, -2 none)
//   tombola  d0 angle (milli-rad), d1 ball count, d2..17 balls: x and y in milli-units packed (x & 0xFFFF) |
//            (y << 16) as int16 each, d18..25 notes of balls 0..7
//   finger   d0 slot of player A (-1), d1 slot of player B (-1), d2..15 slot lengths, d16 cursor, d17 selected slot,
//            d18 hold, d19 pm, d20 step of A, d21 step of B, d22 finger octave, d23 swing, d24 bit mask of the
//            non-empty steps of the selected slot
//   sketch   d0 length (positions), d1 speed, d2..25 note at positions 0, 12, 24, .. 276 (-1), d26 pen position,
//            d27 grid type
//   arpeggio d0 notes in the set, d1 op, d2 octave mode, d3..18 the set's notes, d19 timing, d20 skip, d21 sm,
//            d22 hold, d23 length, d24 swing, d25 skip counter, d26 pitch classes sounding (bit mask)
#pragma once

#include <cstdint>

#include "op1/messages.h"

namespace op1::seq {

inline constexpr int kKeyBase = 53;      // keyboard index 0 = note 53 (og-behaviour-details.md §2.1)
inline constexpr int kKeys = 24;         // drum keys / pattern bits
inline constexpr int kVelocity = 127;    // the firmware's sequencer velocity
inline constexpr int16_t kRest = -32768;
inline constexpr int kGrid = 48;         // the players' start grid (1/8)

// What an engine plays. The sequencer implements it per side (target synth or drum).
class Emitter {
 public:
  virtual void on(int note, int vel, int gate_ticks) noexcept = 0;  // gate 0 = until off()
  virtual void off(int note) noexcept = 0;
  virtual void pass(const Event& e) noexcept = 0;                    // a played key, unchanged
  uint64_t tick = 0;  // the tick firing now (or the next one, outside tick())
  uint64_t grid = 0;  // a tick on the 48-tick start grid
  bool drum = false;  // the drum side
  bool running = false;  // the transport (tape or MIDI) runs
 protected:
  ~Emitter() = default;
};

// The countdown every player keeps between steps, with the firmware's swing pair.
struct StepClock {
  int left = 0;
  bool even = true;
  void restart() noexcept { left = 0; even = true; }
  // One tick. True when a step fires; T = step length in ticks, swing 20..80 (use_swing false = straight).
  bool tick(int T, int swing, bool use_swing = true) noexcept {
    if (--left > 0) return false;
    const int a = use_swing ? T * swing / 50 : T;
    left = even ? a : 2 * T - a;
    if (left < 1) left = 1;
    even = !even;
    return true;
  }
};

// A knob that needs `per` detents per value (the firmware's scale), min..max.
struct Knob {
  int raw = 0;
  int value(int per) const noexcept { return raw / per; }
  void set(int v, int per) noexcept { raw = v * per; }
  int turn(int d, int lo, int hi, int per) noexcept {
    raw += d;
    if (raw < lo * per) raw = lo * per;
    if (raw > hi * per + per - 1) raw = hi * per + per - 1;
    return raw / per;
  }
};

// Notes sounding with their own countdown (in ticks or steps), so the players can end them.
struct Sounding {
  static constexpr int kMax = 32;
  struct N { int16_t note = -1; int16_t left = 0; };
  N n[kMax];
  void add(int note, int left) noexcept;
  void step(Emitter& out) noexcept;     // every unit: count down, note off at 0
  void all_off(Emitter& out) noexcept;
  bool has(int note) const noexcept;
  void remove(int note) noexcept;
};

// The keys held, in the order they were pressed (the firmware's key sets).
struct KeySet {
  int k[24] = {};
  int n = 0;
  bool add(int key) noexcept;     // true if the set was empty before
  bool remove(int key) noexcept;  // true if the set is empty now
  bool has(int key) const noexcept;
  int last() const noexcept { return n ? k[n - 1] : -1; }
  void clear() noexcept { n = 0; }
};

int timing_index(int ticks) noexcept;  // index into {96, 48, 32, 24, 16, 12, 8}, 1 (48) if unknown
int timing_ticks(int index) noexcept;

class Engine {
 public:
  virtual ~Engine() = default;
  virtual void key(const Event& e, Emitter& out) noexcept { out.pass(e); }
  virtual void tick(uint64_t t, Emitter& out) noexcept {}
  virtual void control(Emitter& out) noexcept {}  // once per 64-frame block
  // running: the transport (tape or MIDI) runs. restart: the song grid starts at tick `origin`.
  virtual void transport(bool running, bool restart, uint64_t origin) noexcept {}
  virtual void param(int i, int v) noexcept {}
  virtual void edit(int i, int v) noexcept {}
  virtual void encoder(int i, int d, Emitter& out) noexcept {}
  virtual void key_edit(int key, bool down, Emitter& out) noexcept {}
  virtual void nav(int which, bool shift, Emitter& out) noexcept {}
  virtual void release(Emitter& out) noexcept {}  // stop sounding; forget held keys
  virtual void view(int32_t* d) const noexcept {}  // d[0..27] = display[4..31]
  virtual int step() const noexcept { return -1; }
  virtual bool playing() const noexcept { return false; }
  // The current SetParam values, p[i] = what param(i, v) sets (16 slots, unused stay 0).
  virtual void get_params(int32_t* p) const noexcept {}
};

class PatternSeq final : public Engine {
 public:
  PatternSeq() noexcept;
  void key(const Event& e, Emitter& out) noexcept override;
  void tick(uint64_t t, Emitter& out) noexcept override;
  void param(int i, int v) noexcept override;
  void edit(int i, int v) noexcept override;
  void encoder(int i, int d, Emitter& out) noexcept override;
  void key_edit(int key, bool down, Emitter& out) noexcept override;
  void nav(int which, bool shift, Emitter& out) noexcept override;
  void release(Emitter& out) noexcept override;
  void view(int32_t* d) const noexcept override;
  void get_params(int32_t* p) const noexcept override;
  int step() const noexcept override { return running_ ? pos_ : -1; }
  bool playing() const noexcept override { return running_ || pending_; }
  uint32_t mask(int step) const noexcept { return notes_[step & 15]; }

 private:
  bool empty() const noexcept;
  void stop(Emitter& out) noexcept;
  void advance() noexcept;
  void cursor_move(int d) noexcept;
  uint32_t notes_[16] = {};
  int length_ = 16, pm_ = 2, start_ = 0, swing_ = 50;
  Knob pm_knob_;
  bool latch_ = false, running_ = false, pending_ = false, fwd_ = true;
  int pos_ = 0, cursor_ = 0, entry_ = 0, transpose_ = 0;
  KeySet held_;
  int held_note_[24] = {};
  StepClock clk_;
  Sounding snd_;
};

class EndlessSeq final : public Engine {
 public:
  EndlessSeq() noexcept;
  void key(const Event& e, Emitter& out) noexcept override;
  void tick(uint64_t t, Emitter& out) noexcept override;
  void param(int i, int v) noexcept override;
  void edit(int i, int v) noexcept override;
  void encoder(int i, int d, Emitter& out) noexcept override;
  void key_edit(int key, bool down, Emitter& out) noexcept override;
  void release(Emitter& out) noexcept override;
  void view(int32_t* d) const noexcept override;
  void get_params(int32_t* p) const noexcept override;
  int step() const noexcept override { return running_ ? cur_ : -1; }
  bool playing() const noexcept override { return running_ || pending_; }
  int count() const noexcept { return count_; }

 private:
  struct Step {
    int8_t key[4];
    int8_t len[4];
    int8_t n;  // 0 = rest
  };
  void do_step(Emitter& out) noexcept;
  int next_index() noexcept;
  void stop(Emitter& out) noexcept;
  Step steps_[128] = {};
  int count_ = 0, timing_ = 24, pm_ = 2, eucl_ = 0, eucl_off_ = 0, swing_ = 50;
  Knob timing_knob_, hold_knob_, pm_knob_;
  bool hold_ = false, crank_ = false, running_ = false, pending_ = false;
  bool recording_ = false, rec_clear_ = true;
  int rec_held_ = 0, crank_acc_ = 0;
  int cur_ = -1, epos_ = -1;
  int bag_[128] = {}, nbag_ = 0;
  uint32_t lcg_ = 12345;
  KeySet held_;
  int held_note_[24] = {};
  StepClock clk_;
  Sounding snd_;
};

class TombolaSeq final : public Engine {
 public:
  void key(const Event& e, Emitter& out) noexcept override;
  void control(Emitter& out) noexcept override;
  void param(int i, int v) noexcept override;
  void edit(int i, int v) noexcept override;
  void encoder(int i, int d, Emitter& out) noexcept override;
  void release(Emitter& out) noexcept override;
  void view(int32_t* d) const noexcept override;
  void get_params(int32_t* p) const noexcept override;
  bool playing() const noexcept override { return n_ > 0; }
  int balls() const noexcept { return n_; }
  uint64_t hits() const noexcept { return hits_; }

 private:
  static constexpr int kMax = 16;
  struct Ball { float x, y, vx, vy; int note; bool touching[7]; };  // 6 walls + any ball
  Ball b_[kMax] = {};
  int n_ = 0, next_ = 0;
  float gravity_ = 20.0f, restitution_ = 0.87f, spin_ = 0.0f, angle_ = 0.0f, tilt_ = 0.0f;
  bool crank_ = false;
  uint64_t hits_ = 0;
  uint32_t lcg_ = 777;
};

class FingerSeq final : public Engine {
 public:
  FingerSeq() noexcept;
  void key(const Event& e, Emitter& out) noexcept override;
  void tick(uint64_t t, Emitter& out) noexcept override;
  void param(int i, int v) noexcept override;
  void edit(int i, int v) noexcept override;
  void encoder(int i, int d, Emitter& out) noexcept override;
  void key_edit(int key, bool down, Emitter& out) noexcept override;
  void nav(int which, bool shift, Emitter& out) noexcept override;
  void release(Emitter& out) noexcept override;
  void view(int32_t* d) const noexcept override;
  void get_params(int32_t* p) const noexcept override;
  int step() const noexcept override { return running_ ? step_[0] : -1; }
  bool playing() const noexcept override { return running_ || pending_; }
  static int slot_of_key(int key) noexcept;  // the white keys, -1 for black keys (table 0x018E5F20)

 private:
  static constexpr int kSlots = 14, kSteps = 32;
  void stop(Emitter& out) noexcept;
  void trigger(int slot, int step, Emitter& out) noexcept;
  int8_t notes_[kSlots][kSteps][2];
  int length_[kSlots];
  int pm_ = 1, swing_ = 50;
  Knob pm_knob_;
  int slot_[2] = {-1, -1}, step_[2] = {0, 0};  // players A (primary) and B
  int edited_ = 0, selected_ = 0, cursor_ = 0, octave_ = 0, entry_ = 0;
  bool hold_ = false, running_ = false, pending_ = false;
  KeySet held_;
  int8_t clip_[kSteps][2];
  int clip_len_ = 0;
  StepClock clk_;
  Sounding snd_;
};

class SketchSeq final : public Engine {
 public:
  SketchSeq() noexcept;
  void key(const Event& e, Emitter& out) noexcept override;
  void tick(uint64_t t, Emitter& out) noexcept override;
  void param(int i, int v) noexcept override;
  void edit(int i, int v) noexcept override;
  void encoder(int i, int d, Emitter& out) noexcept override;
  void key_edit(int key, bool down, Emitter& out) noexcept override;
  void release(Emitter& out) noexcept override;
  void view(int32_t* d) const noexcept override;
  void get_params(int32_t* p) const noexcept override;
  int step() const noexcept override { return running_ ? pos_ : -1; }
  bool playing() const noexcept override { return running_; }

 private:
  static constexpr int kPos = 288;
  void stop(Emitter& out) noexcept;
  int8_t pts_[kPos];
  int speed_ = 2, grid_ = 0, pen_ = 0, last_drawn_ = -1, entry_ = 0;
  bool hold_ = false, running_ = false;
  KeySet held_;
  int held_note_[24] = {};
  uint64_t origin_ = 0;
  int pos_ = 0, sounding_ = -1;
};

class ArpSeq final : public Engine {
 public:
  ArpSeq() noexcept;
  void key(const Event& e, Emitter& out) noexcept override;
  void tick(uint64_t t, Emitter& out) noexcept override;
  void param(int i, int v) noexcept override;
  void encoder(int i, int d, Emitter& out) noexcept override;
  void release(Emitter& out) noexcept override;
  void view(int32_t* d) const noexcept override;
  void get_params(int32_t* p) const noexcept override;
  int step() const noexcept override { return playing_ ? s_ : -1; }
  bool playing() const noexcept override { return playing_ || pending_; }

 private:
  void do_step(Emitter& out) noexcept;
  void advance() noexcept;
  int next_offset() noexcept;
  void play_note(const KeySet& set, Emitter& out) noexcept;
  void reset(Emitter& out) noexcept;
  KeySet held_, latched_;
  int op_ = 0, octave_ = 1, timing_ = 48, swing_ = 50, length_ = 12, skip_ = 1, sm_ = 0;
  Knob timing_knob_, octave_knob_, op_knob_, sm_knob_;
  bool hold_ = false, playing_ = false, pending_ = false, flag_ = false;
  int s_ = 0, t_ = 0, pidx_ = 0, c_ = 0;
  uint32_t pcs_ = 0;  // pitch classes sounding (screen)
  StepClock clk_;
  Sounding snd_;
};

}  // namespace op1::seq
