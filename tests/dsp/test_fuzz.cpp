// Randomised sweep of the engine track (t4): every instrument with random stored values on all slots (the full
// int16 range, not only the documented one), random notes, random effect and LFO types switched on and off,
// random MIDI controls and odd chunk sizes. Deterministic seeds. It checks that nothing crashes or stalls and
// that output stays finite-width (an int32 stuck at a rail for a whole second fails); its main use is under
// ./build.sh sanitize, where UBSan turns any signed overflow or bad shift on these paths into a report.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "check.h"
#include "op1/dsp.h"

using namespace op1;

namespace {

struct Lcg {
  uint32_t s;
  uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
  int range(int lo, int hi) { return lo + int(next() % uint32_t(hi - lo + 1)); }
  int16_t any16() { return int16_t(next() >> 16); }
};

Command cmd(InstrumentOp op, int index, int value, int slot = 0) {
  Command c;
  c.domain = Domain::Synth;
  c.op = uint16_t(op);
  c.index = index;
  c.value = value;
  c.slot = uint8_t(slot);
  return c;
}

}  // namespace

int main() {
  dsp_init();
  auto* mem = static_cast<std::byte*>(::operator new(kInstrumentStorage, std::align_val_t(64)));
  std::memset(mem, 0, kInstrumentStorage);
  // user-style audio for the samplers: a noisy decaying tone
  std::vector<int16_t> audio(size_t(kSampleRate) * 12);
  Lcg g{7};
  for (size_t i = 0; i < audio.size(); ++i)
    audio[i] = int16_t((int32_t(g.next() >> 17) - 16384) * int32_t(1 + (i % 22050) / 400) / 64);
  SampleData smp;
  smp.frames = audio.data();
  smp.length = int32_t(audio.size());
  ControlInputs ctl;
  int32_t zero[kBlock] = {}, l[kBlock], r[kBlock];
  long stuck_fail = 0, renders = 0;

  for (int ti = 0; ti < int(InstrumentType::Count); ++ti) {
    for (int round = 0; round < 6; ++round) {
      Instrument* inst = create_instrument(InstrumentType(ti), mem, kInstrumentStorage);
      if (!inst) continue;  // ls is open (nullptr)
      Lcg rng{uint32_t(1000 + ti * 77 + round)};
      if (ti == int(InstrumentType::Sampler) || ti == int(InstrumentType::Drum))
        inst->command(cmd(InstrumentOp::SetSample, 0, 0));  // first with no sample at all
      int64_t frame = 0;
      int32_t last = 0;
      long same = 0;
      for (int step = 0; step < 400; ++step) {
        switch (rng.range(0, 11)) {
          case 0: inst->command(cmd(InstrumentOp::SetKnob, rng.range(0, 7), rng.any16())); break;
          case 1: inst->command(cmd(InstrumentOp::SetAdsr, rng.range(0, 7), rng.any16())); break;
          case 2: inst->command(cmd(InstrumentOp::SetFxParam, rng.range(0, 7), rng.any16())); break;
          case 3: inst->command(cmd(InstrumentOp::SetLfoParam, rng.range(0, 7), rng.any16())); break;
          case 4: inst->command(cmd(InstrumentOp::SetFxType, 0, rng.range(0, 8))); break;    // 8 = invalid
          case 5: inst->command(cmd(InstrumentOp::SetLfoType, 0, rng.range(0, 7))); break;   // 7 = invalid
          case 6: inst->command(cmd(InstrumentOp::SetFxOn, 0, rng.range(0, 1))); break;
          case 7: inst->command(cmd(InstrumentOp::SetLfoOn, 0, rng.range(0, 1))); break;
          case 8: inst->command(cmd(InstrumentOp::SetOctave, 0, rng.range(-6, 6))); break;
          case 9: inst->command(cmd(InstrumentOp::SetDrumKey, rng.range(-1, 24), rng.next(), rng.range(0, 16))); break;
          case 10: {
            Command c = cmd(InstrumentOp::SetSample, 0, 0);
            c.ptr = &smp;
            inst->command(c);
            break;
          }
          default: break;
        }
        for (int k = 0; k < 3; ++k) {
          const int note = rng.range(0, 127);
          if (rng.range(0, 2)) inst->note_on(note, rng.range(1, 127), rng.range(-1, 70));
          else inst->note_off(note, rng.range(-1, 70));
        }
        if (rng.range(0, 40) == 0) inst->all_notes_off();
        ctl.cc[rng.range(0, 127)] = int16_t(rng.range(0, 127));
        ctl.pitch_bend = int16_t(rng.range(-8192, 8191));
        ctl.accel[0] = rng.any16();
        ctl.input_level = int32_t(rng.next() >> 1);
        // a few chunks of odd sizes on the 64-frame grid
        for (int c = 0; c < 4; ++c) {
          const int n = std::min<int>(rng.range(1, kBlock), kBlock - int(frame % kBlock));
          ProcessContext ctx;
          ctx.frame = frame;
          ctx.frames = n;
          ctx.block_start = frame % kBlock == 0;
          ctx.bpm_tenths = int16_t(rng.range(400, 2200));
          ctx.in_l = zero;
          ctx.in_r = zero;
          ctx.controls = rng.range(0, 9) ? &ctl : nullptr;
          inst->process(ctx, l, r);
          for (int i = 0; i < n; ++i) {
            const bool rail = l[i] >= INT32_MAX - 1 || l[i] <= INT32_MIN + 1;
            same = (rail && l[i] == last) ? same + 1 : 0;
            last = l[i];
            if (same > kSampleRate) ++stuck_fail;
          }
          frame += n;
          ++renders;
        }
        InstrumentView v;
        inst->view(v);
      }
      inst->~Instrument();
    }
  }
  // the stereo (master) effects with random parameters, as t5's mixer uses them
  auto* fxmem = static_cast<std::byte*>(::operator new(kEffectStorage, std::align_val_t(64)));
  std::vector<int32_t> fl(kBlock), fr(kBlock);
  for (int ti = 0; ti < int(FxType::Count); ++ti) {
    std::memset(fxmem, 0, kEffectStorage);
    Effect* fx = create_effect(FxType(ti), true, fxmem, kEffectStorage);
    CHECK(fx != nullptr);
    if (!fx) continue;
    Lcg rng{uint32_t(5000 + ti)};
    for (int step = 0; step < 3000; ++step) {
      if (step % 16 == 0) fx->set_param(rng.range(0, 7), rng.any16());
      if (rng.range(0, 500) == 0) fx->reset();
      for (int i = 0; i < kBlock; ++i) {
        fl[size_t(i)] = int32_t(rng.next());
        fr[size_t(i)] = int32_t(rng.next()) >> rng.range(0, 8);
      }
      ProcessContext ctx;
      ctx.frame = int64_t(step) * kBlock;
      ctx.frames = kBlock;
      ctx.block_start = true;
      fx->process(ctx, fl.data(), rng.range(0, 9) ? fr.data() : nullptr);
      ++renders;
    }
    int32_t d[16];
    fx->view(d);
    fx->~Effect();
  }
  ::operator delete(fxmem, std::align_val_t(64));
  std::printf("fuzz: %ld chunks rendered, %ld stuck-at-rail seconds\n", renders, stuck_fail);
  CHECK_EQ(stuck_fail, 0);
  ::operator delete(mem, std::align_val_t(64));
  return check_result("dsp fuzz");
}
