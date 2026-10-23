// Tape tests (owner: t5): record/playback round trip, replace vs overdub, reverse, speed, loop, lift/drop,
// the 512-clip limit, MIDI song-position seeks and transport, the mixer's gains.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "check.h"
#include "op1/tape.h"
#include "../../src/tape/tape_engine.h"

using namespace op1;

namespace {

struct Deck {
  std::unique_ptr<Tape> t = create_tape();
  int64_t frame = 0;
  std::vector<Sample> out[kTapeTracks];

  void cmd(TapeOp op, int32_t value = 0, int32_t index = 0, uint8_t slot = 0) {
    Command c;
    c.domain = Domain::Tape; c.op = uint16_t(op); c.value = value; c.index = index; c.slot = slot;
    t->command(c);
  }
  // Runs `n` frames in chunks of <= 64 on the grid, feeding rec(i) and collecting the 4 outputs.
  template <typename F>
  void run(int n, F rec) {
    for (auto& o : out) o.clear();
    Sample r[kBlock], tr[kTapeTracks][kBlock];
    int done = 0;
    while (done < n) {
      const int k = std::min(n - done, kBlock - int(frame % kBlock));
      for (int i = 0; i < k; ++i) r[i] = rec(done + i);
      ProcessContext ctx;
      ctx.frame = frame;
      ctx.frames = k;
      ctx.block_start = frame % kBlock == 0;
      Sample* tp[kTapeTracks] = {tr[0], tr[1], tr[2], tr[3]};
      t->process(ctx, r, tp);
      for (int c = 0; c < kTapeTracks; ++c) out[c].insert(out[c].end(), tr[c], tr[c] + k);
      frame += k;
      done += k;
    }
  }
  void run(int n) { run(n, [](int) { return Sample(0); }); }
  TapeView view() const {
    TapeView v;
    t->view(v);
    return v;
  }
  void settle() {  // let a lift/drop/clear job finish
    for (int i = 0; i < 2000 && view().display[0] >= tape::kLifting; ++i) run(32);
  }
};

// Test signals that 16-bit storage holds exactly (multiples of 65536).
Sample sig(int i, int seed) { return Sample(((i * 37 + seed * 1001) % 30000 - 15000) * 65536); }
Sample small(int i, int seed) { return Sample(((i * 13 + seed * 7) % 8000 - 4000) * 65536); }

void test_round_trip(Deck& d) {
  const int n = 44100;  // 1 s
  d.cmd(TapeOp::SelectTrack, 1);
  d.cmd(TapeOp::Seek, 1000);
  d.cmd(TapeOp::Record, 1);
  d.cmd(TapeOp::Play);
  d.run(n, [](int i) { return sig(i, 1); });
  d.cmd(TapeOp::Stop);
  d.cmd(TapeOp::Record, 0);
  TapeView v = d.view();
  CHECK_EQ(v.position, 1000 + n);
  CHECK_EQ(v.display[3], 1);  // one clip
  d.cmd(TapeOp::Seek, 1000);
  CHECK_EQ(d.view().display[6 + 2], 1000);  // the clip under the head on track 1: [1000, 1000 + n)
  CHECK_EQ(d.view().display[7 + 2], 1000 + n);
  d.cmd(TapeOp::Play);
  d.run(n);
  d.cmd(TapeOp::Stop);
  int bad = 0, other = 0;
  for (int i = 0; i < n; ++i) {
    bad += d.out[1][size_t(i)] != sig(i, 1);
    other += d.out[0][size_t(i)] != 0 || d.out[2][size_t(i)] != 0 || d.out[3][size_t(i)] != 0;
  }
  std::printf("round trip: %d of %d samples differ, %d samples leaked to other tracks\n", bad, n, other);
  CHECK_EQ(bad, 0);
  CHECK_EQ(other, 0);

  // any Q1.31 value comes back within half a 16-bit step (rounded, not truncated)
  d.cmd(TapeOp::SelectTrack, 2);
  d.cmd(TapeOp::Seek, 0);
  d.cmd(TapeOp::Record, 1);
  d.cmd(TapeOp::Play);
  d.run(1000, [](int i) { return Sample(i * 2147371 - 1073741824); });
  d.cmd(TapeOp::Record, 0);
  d.cmd(TapeOp::Seek, 0);
  d.run(1000);
  d.cmd(TapeOp::Stop);
  int64_t worst = 0;
  for (int i = 0; i < 1000; ++i)
    worst = std::max<int64_t>(worst, std::llabs(int64_t(d.out[2][size_t(i)]) - (int64_t(i) * 2147371 - 1073741824)));
  std::printf("arbitrary values: worst error %lld (half a 16-bit step = 32768)\n", (long long)worst);
  CHECK(worst <= 32768);
}

void test_overdub_and_replace(Deck& d) {
  const int n = 4410;
  d.cmd(TapeOp::SelectTrack, 3);
  d.cmd(TapeOp::Seek, 50000);
  d.cmd(TapeOp::Record, 1);
  d.cmd(TapeOp::Play);
  d.run(n, [](int i) { return small(i, 2); });
  d.cmd(TapeOp::Record, 0);
  // overdub: the new signal is summed with the old, which stays audible meanwhile
  d.cmd(TapeOp::SetRecordMode, tape::kOverdub);
  d.cmd(TapeOp::Seek, 50000);
  d.cmd(TapeOp::Record, 1);
  d.run(n, [](int i) { return small(i, 3); });
  CHECK(d.out[3][100] == small(100, 2));
  d.cmd(TapeOp::Record, 0);
  d.cmd(TapeOp::Seek, 50000);
  d.run(n);
  int bad = 0;
  for (int i = 0; i < n; ++i) bad += d.out[3][size_t(i)] != small(i, 2) + small(i, 3);
  CHECK_EQ(bad, 0);
  // replace: the track is silent under the head while recording, then holds only the new signal
  d.cmd(TapeOp::SetRecordMode, tape::kReplace);
  d.cmd(TapeOp::Seek, 50000);
  d.cmd(TapeOp::Record, 1);
  d.run(n, [](int i) { return sig(i, 4); });
  CHECK_EQ(d.out[3][100], 0);
  d.cmd(TapeOp::Record, 0);
  d.cmd(TapeOp::Seek, 50000);
  d.run(n);
  d.cmd(TapeOp::Stop);
  bad = 0;
  for (int i = 0; i < n; ++i) bad += d.out[3][size_t(i)] != sig(i, 4);
  CHECK_EQ(bad, 0);
  CHECK_EQ(d.view().display[3], 3);  // recordings over the same place join: tracks 1, 2, 3 have one each
}

void test_reverse_speed_loop(Deck& d) {
  // track 1 holds sig(i, 1) at 1000 + i
  d.cmd(TapeOp::SelectTrack, 1);
  d.cmd(TapeOp::SetReverse, 1);
  d.cmd(TapeOp::Seek, 1000 + 999);
  d.cmd(TapeOp::Play);
  d.run(1000);
  d.cmd(TapeOp::Stop);
  d.cmd(TapeOp::SetReverse, 0);
  int bad = 0;
  for (int i = 0; i < 1000; ++i) bad += d.out[1][size_t(i)] != sig(999 - i, 1);
  CHECK_EQ(bad, 0);

  // double speed: every second sample; half speed: each sample, then the midpoint
  d.cmd(TapeOp::SetSpeed, 2000);
  d.cmd(TapeOp::Seek, 1000);
  d.cmd(TapeOp::Play);
  d.run(500);
  bad = 0;
  for (int i = 0; i < 500; ++i) bad += d.out[1][size_t(i)] != sig(2 * i, 1);
  CHECK_EQ(bad, 0);
  CHECK_EQ(d.view().position, 2000);
  d.cmd(TapeOp::SetSpeed, 500);
  d.cmd(TapeOp::Seek, 1000);
  d.run(200);
  bad = 0;
  for (int i = 0; i < 100; ++i) {
    bad += d.out[1][size_t(2 * i)] != sig(i, 1);
    bad += d.out[1][size_t(2 * i + 1)] != Sample((int64_t(sig(i, 1)) + sig(i + 1, 1)) / 2);
  }
  CHECK_EQ(bad, 0);
  d.cmd(TapeOp::SetSpeed, tape::kSpeedNormal);

  // loop 1100..1200: the head wraps
  d.cmd(TapeOp::SetLoopIn, 1100);
  d.cmd(TapeOp::SetLoopOut, 1200);
  d.cmd(TapeOp::SetLoop, 1);
  d.cmd(TapeOp::Seek, 1100);
  d.run(250);
  bad = 0;
  for (int i = 0; i < 250; ++i) bad += d.out[1][size_t(i)] != sig(100 + i % 100, 1);
  CHECK_EQ(bad, 0);
  d.cmd(TapeOp::Stop);
  d.cmd(TapeOp::SetLoop, 0);

  // the end of the tape stops the transport
  d.cmd(TapeOp::Seek, kTapeLength - 10);
  d.cmd(TapeOp::Play);
  d.run(64);
  CHECK(!d.view().playing);
}

void test_lift_drop(Deck& d) {
  // track 1 has the clip [1000, 45100). Lift it with the head in its middle, drop it on track 0 at 200000.
  d.cmd(TapeOp::SelectTrack, 1);
  d.cmd(TapeOp::Seek, 20000);
  d.cmd(TapeOp::Lift, 0, 0);
  d.settle();
  TapeView v = d.view();
  CHECK_EQ(v.display[0], tape::kHolding);
  CHECK_EQ(v.display[1], 44100);
  CHECK_EQ(v.display[6 + 2], -1);  // the clip is gone from under the head
  d.cmd(TapeOp::Seek, 1000);
  d.cmd(TapeOp::Play);
  d.run(1000);
  d.cmd(TapeOp::Stop);
  int nz = 0;
  for (Sample s : d.out[1]) nz += s != 0;
  CHECK_EQ(nz, 0);  // the lifted audio is gone from the tape

  d.cmd(TapeOp::SelectTrack, 0);
  d.cmd(TapeOp::Seek, 200000);
  d.cmd(TapeOp::Drop);
  d.settle();
  d.cmd(TapeOp::Seek, 200000);
  d.cmd(TapeOp::Play);
  d.run(44100);
  d.cmd(TapeOp::Stop);
  int bad = 0;
  for (int i = 0; i < 44100; ++i) bad += d.out[0][size_t(i)] != sig(i, 1);
  CHECK_EQ(bad, 0);
  d.cmd(TapeOp::Seek, 200000);
  CHECK_EQ(d.view().display[6], 200000);
  CHECK_EQ(d.view().display[7], 244100);

  // a lift is limited to just under 90 s (kClipboardMaxLength)
  d.cmd(TapeOp::SetLoopIn, 0);
  d.cmd(TapeOp::SetLoopOut, 100 * 44100);
  d.cmd(TapeOp::SetLoop, 1);
  d.cmd(TapeOp::Lift, 0, 1);  // all tracks
  CHECK_EQ(d.view().display[1], tape::kClipboardMaxLength - 1);
  CHECK_EQ(d.view().display[2], 1);
  d.settle();
  CHECK_EQ(d.view().display[0], tape::kHolding);
  d.cmd(TapeOp::SetLoop, 0);
}

void test_clip_limit_and_song_position() {
  Deck d;
  d.cmd(TapeOp::Record, 1);
  for (int k = 0; k < 513; ++k) {  // 513 separate short recordings on track 0
    d.cmd(TapeOp::Seek, k * 100);
    d.cmd(TapeOp::Play);
    d.run(10, [](int) { return Sample(1 << 20); });
    d.cmd(TapeOp::Stop);
  }
  TapeView v = d.view();
  std::printf("clips: %d stored, %d refused (MAX_CLIPS 512)\n", v.display[3], v.display[15]);
  CHECK_EQ(v.display[3], 512);
  CHECK_EQ(v.display[15], 1);
  d.cmd(TapeOp::Record, 0);

  // MIDI song position: SPP * 10,584,000 / BPM / 16 + 0.5 samples, only while stopped, ignored past the end
  Event e;
  e.type = EventType::SongPosition;
  e.value = 16;  // one bar
  d.t->transport(e, 1200);
  CHECK_EQ(d.view().position, 88200);  // one bar at 120 BPM = 2 s
  e.value = 4000;
  d.t->transport(e, 1250);  // 4000 sixteenths at 125 BPM = 8:00, past 6:00: ignored
  CHECK_EQ(d.view().position, 88200);
  e.value = 7;
  d.t->transport(e, 1000);  // 7 * 10584000 / 100 / 16 + 0.5 = 46305.5 -> 46305
  CHECK_EQ(d.view().position, 46305);
  Event st;
  st.type = EventType::Start;
  d.t->transport(st, 1200);  // MIDI Start: seek to 0 and play
  CHECK(d.view().playing);
  CHECK_EQ(d.view().position, 0);
  e.value = 16;
  d.t->transport(e, 1200);  // playing: ignored
  CHECK_EQ(d.view().position, 0);
  st.type = EventType::Stop;
  d.t->transport(st, 1200);
  CHECK(!d.view().playing);
}

void test_mixer() {
  auto m = create_mixer();
  Sample tr[kTapeTracks][64] = {}, ll[64] = {}, lr[64] = {}, ol[64], orr[64];
  for (int i = 0; i < 64; ++i) tr[2][i] = 1 << 28;
  Command c;
  c.domain = Domain::Mixer;
  c.op = uint16_t(MixerOp::SetPan);
  c.slot = 2;
  c.value = 32767;  // hard right
  m->command(c);
  const Sample* tp[kTapeTracks] = {tr[0], tr[1], tr[2], tr[3]};
  ProcessContext ctx;
  ctx.frames = 64;
  ctx.block_start = true;
  m->process(ctx, tp, ll, lr, ol, orr);
  CHECK(ol[10] < (1 << 16));                // nothing left
  CHECK(orr[10] > (1 << 28) - (1 << 16));   // full right
  MixerView v;
  m->view(v);
  CHECK_EQ(v.pan[2], 32767);
  CHECK(v.meter_r > 4000);
}

}  // namespace

int main() {
  {
    Deck d;
    test_round_trip(d);
    test_overdub_and_replace(d);
    test_reverse_speed_loop(d);
    test_lift_drop(d);
  }
  test_clip_limit_and_song_position();
  test_mixer();
  return check_result("tape");
}
