// Core tests (owner: t3): fract.h semantics, the lock-free primitives, and an offline run of the whole
// audio graph (no devices) that counts heap allocations and frees made during Core::process(),
// including an engine switch. The count must be zero: the realtime rule as a test.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
#include <vector>

#include "check.h"
#include "op1/core.h"
#include "op1/dsp.h"
#include "op1/fract.h"
#include "op1/spsc.h"

// ---- allocation counter (global operator new/delete for this test binary)
static std::atomic<bool> g_counting{false};
static std::atomic<long> g_allocs{0}, g_frees{0};

static void* counted_alloc(size_t n, size_t align) {
  if (g_counting) ++g_allocs;
  void* p = align > alignof(std::max_align_t) ? std::aligned_alloc(align, (n + align - 1) / align * align)
                                               : std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
static void counted_free(void* p) {
  if (p && g_counting) ++g_frees;
  std::free(p);
}
void* operator new(size_t n) { return counted_alloc(n, 0); }
void* operator new[](size_t n) { return counted_alloc(n, 0); }
void* operator new(size_t n, std::align_val_t a) { return counted_alloc(n, size_t(a)); }
void* operator new[](size_t n, std::align_val_t a) { return counted_alloc(n, size_t(a)); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, size_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t) noexcept { counted_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { counted_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { counted_free(p); }

using namespace op1;

static void test_fract() {
  using namespace op1::bf;
  CHECK_EQ(sat16(40000), 32767);
  CHECK_EQ(sat16(-40000), -32768);
  CHECK_EQ(mult_fr1x16(-32768, -32768), 32767);  // the one overflowing product saturates
  CHECK_EQ(mult_fr1x16(16384, 16384), 8192);     // 0.5 * 0.5
  CHECK_EQ(mult_fr1x16(3, 16384), 1);            // truncates 1.5 -> 1
  CHECK_EQ(multr_fr1x16(3, 16384), 2);           // biased rounding 1.5 -> 2
  CHECK_EQ(multr_fr1x16_unbiased(3, 16384), 2);  // half to even: 1.5 -> 2
  CHECK_EQ(multr_fr1x16_unbiased(5, 16384), 2);  // 2.5 -> 2
  CHECK_EQ(add_fr1x32(INT32_MAX, 1), INT32_MAX);
  CHECK_EQ(sub_fr1x32(INT32_MIN, 1), INT32_MIN);
  CHECK_EQ(add_ns32(INT32_MAX, 1), INT32_MIN);   // NS ops wrap like the hardware
  CHECK_EQ(mult_fr1x32(-32768, -32768), INT32_MAX);
  CHECK_EQ(mult_fr1x32(16384, 16384), 1 << 29);
  CHECK_EQ(mul_q31_full(1 << 30, 1 << 30), 1 << 29);
  CHECK_EQ(shl_fr1x32(1 << 30, 2), INT32_MAX);
  CHECK_EQ(shl_fr1x32(-8, -2), -2);
  CHECK_EQ(norm_fr1x32(1), 30);
  CHECK_EQ(norm_fr1x32(1 << 30), 0);
  CHECK_EQ(norm_fr1x32(-1), 31);
  CHECK_EQ(round_fr1x32(0x00018000), 2);
  Acc40 a;
  for (int i = 0; i < 300; ++i) a.mac(-32768, -32768);  // 300 * 2^31 > 2^39: saturates at 40 bits
  CHECK_EQ(a.v, Acc40::kMax);
  CHECK_EQ(a.to32(), INT32_MAX);
}

static void test_spsc() {
  SpscQueue<int, 8> q;
  for (int i = 0; i < 8; ++i) CHECK(q.push(i));
  CHECK(!q.push(99));
  int v = -1;
  for (int i = 0; i < 8; ++i) {
    CHECK(q.pop(v));
    CHECK_EQ(v, i);
  }
  CHECK(!q.pop(v));
  for (int round = 0; round < 100; ++round) {  // wrap-around
    CHECK(q.push(round));
    CHECK(q.pop(v));
    CHECK_EQ(v, round);
  }

  struct Snap { int a, b; };
  TripleBuffer<Snap> tb;
  tb.back() = {1, 1};
  tb.publish();
  tb.back() = {2, 2};
  tb.publish();
  CHECK_EQ(tb.read().a, 2);  // reader gets the newest complete snapshot
  CHECK_EQ(tb.read().b, 2);
  tb.back() = {3, 3};
  CHECK_EQ(tb.read().a, 2);  // unpublished writes are invisible
  tb.publish();
  CHECK_EQ(tb.read().a, 3);
}

static void test_core_offline() {
  Core core;
  std::vector<Sample> il(256, 0), ir(256, 0), l(256, 0), r(256, 0);
  for (int i = 0; i < 16; ++i) core.process(il.data(), ir.data(), l.data(), r.data(), 32);  // warm up

  g_allocs = 0;
  g_frees = 0;
  g_counting = true;
  core.set_dac_time(2000);
  CHECK(core.post_midi(note_on(60, 100, Source::Midi, 1000)));
  int64_t frames = 0;
  int32_t peak = 0;
  // device periods of 32, 48 (crosses the 64-frame grid) and 128 (larger than a block)
  for (int period : {32, 48, 128}) {
    for (int i = 0; i < 300; ++i) {
      core.process(il.data(), ir.data(), l.data(), r.data(), period);
      frames += period;
      for (int k = 0; k < period; ++k) peak = std::max(peak, bf::abs_fr1x32(l[size_t(k)]));
    }
  }
  Command sw;
  sw.domain = Domain::Synth;
  sw.op = uint16_t(InstrumentOp::SetType);
  sw.value = int32_t(InstrumentType::DSynth);
  CHECK(core.post_ui(sw));  // engine switch happens on the audio thread
  for (int i = 0; i < 100; ++i) core.process(il.data(), ir.data(), l.data(), r.data(), 64);
  CHECK(core.post_midi(note_off(60, Source::Midi, 3000)));
  for (int i = 0; i < 100; ++i) core.process(il.data(), ir.data(), l.data(), r.data(), 64);
  g_counting = false;

  CHECK_EQ(g_allocs.load(), 0);
  CHECK_EQ(g_frees.load(), 0);
  CHECK(peak > (1 << 24));  // the placeholder engine sounded
  const CoreSnapshot& s = core.snapshot();
  CHECK(s.synth.type == InstrumentType::DSynth);
  CHECK_EQ(s.frame % kBlock, 0);
  CHECK(int64_t(s.frame) >= frames);
  NoteTiming t;
  CHECK(core.pop_note_timing(t));
  CHECK_EQ(t.received_ns, 1000);
  CHECK(t.dac_ns >= 2000);
  CHECK(t.src == Source::Midi);
}

// A slot's sample must survive an engine switch: sampler -> fm -> sampler plays the same as a sampler
// that just got the sample (t4's report: the re-created sampler used to stay silent).
static int64_t sampler_energy(const SampleData* smp, bool switch_away_and_back) {
  Core core;
  auto cmd = [&](InstrumentOp op, int32_t value, void* ptr = nullptr) {
    Command c;
    c.domain = Domain::Synth;
    c.op = uint16_t(op);
    c.value = value;
    c.ptr = ptr;
    CHECK(core.post_ui(c));
  };
  std::vector<Sample> l(64), r(64);
  cmd(InstrumentOp::SetType, int32_t(InstrumentType::Sampler));
  cmd(InstrumentOp::SetSample, 0, const_cast<SampleData*>(smp));
  core.process(nullptr, nullptr, l.data(), r.data(), 64);
  if (switch_away_and_back) {
    cmd(InstrumentOp::SetType, int32_t(InstrumentType::Fm));
    core.process(nullptr, nullptr, l.data(), r.data(), 64);
    cmd(InstrumentOp::SetType, int32_t(InstrumentType::Sampler));
    core.process(nullptr, nullptr, l.data(), r.data(), 64);
  }
  CHECK(core.post_midi(note_on(60, 100, Source::Midi, 1)));
  int64_t energy = 0;
  for (int i = 0; i < 700; ++i) {  // ~1 s
    core.process(nullptr, nullptr, l.data(), r.data(), 64);
    for (Sample s : l) energy += std::abs(int64_t(s) >> 16);
  }
  return energy;
}

static void test_sample_survives_engine_switch() {
  std::vector<int16_t> frames(44100);
  for (size_t i = 0; i < frames.size(); ++i) frames[i] = (i / 50) % 2 ? 12000 : -12000;  // 441 Hz square
  SampleData smp;
  smp.frames = frames.data();
  smp.length = int32_t(frames.size());
  const int64_t fresh = sampler_energy(&smp, false);
  const int64_t after_switch = sampler_energy(&smp, true);
  CHECK(fresh > 0);
  CHECK_EQ(after_switch, fresh);
}

int main() {
  test_fract();
  test_spsc();
  test_core_offline();
  test_sample_survives_engine_switch();
  return check_result("core");
}
