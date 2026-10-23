// drum (id 8, DrumSampler.cpp): one kit sample sliced across the 24 keys. The kit audio comes from the user's
// firmware image (the worker loads the factory kits); nothing from Teenage Engineering ships.
//
// Per-key values as the firmware keeps and plays them (t5, 2026-10-23, decoded from DrumSampler/DrumController,
// t5 notes decode-drums.md; F = seen in the code or run in the bfin simulator, I = this port's reading):
//   - F start / end: Q31 fractions of the 12 s kit buffer (529,200 frames): frame = value / 4058.03. 0 <= start <=
//     end <= 0x7FFFFFFE.
//   - F playmode: mode = value >> 13: 0 (4096..8191) gate: stops at key release; 1 (8192..16383, the factory 8192
//     and 9216) one-shot: plays to the slice end; 2 (16384..20480) loop: loops start..end while held, stops at
//     release (note-off 0x01978898, render 0x019789EC). (Before t5: thresholds 6144/10240, port choices.)
//   - F reverse: value >= 16384 plays end -> start (8192 = forward). (Before: > 12288.)
//   - F volume: gain = (volume / 8192)^2, 8192 = 0 dB, 6806 = -3.2 dB (render 0x01978A9E). (Before: linear.)
//   - F a released gate/loop voice stops (state 0); here with a 3 ms fade so it does not click (I).
//   - pitch (F: -24320..24832, 194 per encoder detent; the unit sits in the L1 voice code, not readable in the
//     dump). I: 6208 raw = one octave (the factory value 6208 = 32 detents, and dbox's chromatic bass kits climb
//     ~6100 raw per 12 keys), so 194 = 3/8 semitone and the range is +-3.9 octaves.
//   - dyna_env (F: 8 slots, the envelope page's encoders edit 0..4: s0 -32767..32767, s1 0..8192, s2 -32767..32767,
//     s3 0..32767, s4 0..32767; every factory kit stores s1 = 8192 = its maximum). Its sound is in the L1 code (open),
//     so this port reads its two signed slots as a level envelope over each hit's slice (I): s0 = attack, the
//     first |s0|/32768 of the slice fades in (s0 < 0: a convex curve); s2 = decay, the last |s2|/32768 fades out
//     (s2 < 0: convex). s1, s3, s4 are kept and shown but have no effect here (open). s0 = s2 = 0 is neutral.
//     The drum's InstrumentOp::SetAdsr sets dyna_env (the drum has no ADSR), and InstrumentView::adsr shows it.
// Without a patch the sample is cut into 24 equal slices. Keys map from notes as (note - 53) mod 24 (key 0 = F2).
//
// Engine knobs (kit-wide offsets for LFOs and MIDI, 0 = none, signed): 0 tune (+-12 semitones), 1 start
// offset (+-10 % of each slice), 2 end offset (+-10 %), 3 level (gain 0 .. 2x). The UI edits the keys instead.
// SetDrumKey: slot = field (0 start, 1 end, 2 pitch, 3 playmode, 4 reverse, 5 volume), index = key 0..23,
// value = the stored value as the firmware keeps it.
//
// display: [0..7] key of drum voice 0..7 (-1 idle), [16..39] level of key 0..23 (Q15), [40] sample length
// (frames, 0 = no sample), [41] last key played, [42] keys from a patch (1) or equal slices (0),
// t5: [43] the key the encoders edit (the last played, 0 before any), its values [44] start, [45] end (Q31 as
// stored), [46] pitch, [47] playmode, [48] reverse, [49] volume, [50] its mode 0 gate / 1 one-shot / 2 loop,
// [51] reversed (0/1), [52] slice start frame, [53] slice end frame.
#include <new>

#include "instrument_base.h"

namespace op1::dsp {
namespace {

constexpr int kDrumVoices = 8;  // kDrumVoiceCount is not settled statically; 8 overlapping hits
constexpr s64 kQ31PerFrame = 4058;  // 2^31 / 529,200 (a 12 s kit)

class DrumSampler final : public InstrumentBase {
 public:
  DrumSampler() noexcept : InstrumentBase(InstrumentType::Drum) {
    for (int k = 0; k < kDrumKeys; ++k) {
      vol_[k] = 8192;
      pm_[k] = 8192;
      rev_[k] = 8192;
    }
    dyna_[1] = 8192;
    dyna_[3] = 8192;
  }

 protected:
  void on_note(int note, bool on) noexcept override {
    const int key = ((note - 53) % kDrumKeys + kDrumKeys) % kDrumKeys;
    if (on) {
      V* pick = nullptr;
      for (auto& v : v_)
        if (v.on && v.key == key) pick = &v;  // the same key again restarts it
      if (!pick)
        for (auto& v : v_)
          if (!v.on && (!pick || v.age < pick->age)) pick = &v;
      if (!pick)
        for (auto& v : v_)
          if (!pick || v.age < pick->age) pick = &v;
      start(*pick, key);
      last_key_ = key;
    } else {
      for (auto& v : v_)
        if (v.on && v.key == key && v.held) {
          v.held = false;
          if (keys_[key].mode != 1) v.releasing = true;  // gate and loop keys stop on release
        }
    }
  }
  void on_all_off() noexcept override {
    for (auto& v : v_) {
      v.held = false;
      if (v.on) v.releasing = true;
    }
  }

  void block_control(const ProcessContext& ctx) noexcept override {
    if (dirty_ || eff_knob_[1] != last_off_[0] || eff_knob_[2] != last_off_[1]) build_keys();
    const s32 tune = s32(s64(s15(eff_knob_[0])) * 3072 / 32767);
    const s32 shift = tune + bend_q8(ctx) + mod_.pitch_q8;
    level15_ = s32(32768 + (s64(s15(eff_knob_[3])) * 32768) / 32767);
    const u64 sr = u64(smp_ && smp_->sample_rate > 0 ? smp_->sample_rate : kSampleRate);
    for (auto& v : v_) {
      if (!v.on) continue;
      const u64 ratio = semis_ratio_q16(keys_[v.key].pitch_q8 + shift);
      v.step = ((ratio << 16) * sr) / kSampleRate;
    }
    // dyna_env (see the header): attack / decay fractions of the slice (Q15)
    att15_ = s32(s64(dyna_[0] < 0 ? -s32(dyna_[0]) : dyna_[0]));
    dec15_ = s32(s64(dyna_[2] < 0 ? -s32(dyna_[2]) : dyna_[2]));
    att_convex_ = dyna_[0] < 0;
    dec_convex_ = dyna_[2] < 0;
  }

  void render(const ProcessContext&, s32* out, int n) noexcept override {
    for (int i = 0; i < n; ++i) out[i] = 0;
    if (!smp_ || !len_) {
      for (auto& v : v_) v.on = false;
      return;
    }
    for (auto& v : v_) {
      if (!v.on) continue;
      const Key& k = keys_[v.key];
      const u64 lo = u64(k.lo) << 32, hi = u64(k.hi) << 32;
      const u64 span = hi > lo ? hi - lo : 1;
      const s64 g = (s64(k.vol15) * level15_) >> 15;
      for (int i = 0; i < n; ++i) {
        if (v.releasing) {
          v.rel -= kRelStep;
          if (v.rel <= 0) { v.on = false; break; }
        }
        // position in the slice 0..32767 from the start of play (reverse: from the end)
        const u64 done = k.rev ? hi - (v.pos < hi ? v.pos : hi) : (v.pos > lo ? v.pos - lo : 0);
        const s32 p15 = s32(((done >> 16) * 32767) / (span >> 16 ? span >> 16 : 1));
        s32 amp = dyna_amp(p15 > 32767 ? 32767 : p15);
        const u64 left = k.rev ? (v.pos > lo ? v.pos - lo : 0) : (hi > v.pos ? hi - v.pos : 0);
        const bool loop = k.mode == 2 && v.held;
        const s32 edge = loop ? 32767 : s32(clampv<u64>((left >> 32) * 1024, 0, 32767));  // 32 frames to the edge
        if (edge < amp) amp = edge;
        if (v.age_s < 32) amp = s32((s64(amp) * v.age_s) >> 5);  // 0.7 ms de-click at the hit
        ++v.age_s;
        amp = s32((s64(amp) * v.rel) >> 15);
        v.lvl = amp;
        const s32 y = read(v.pos);
        out[i] = add(out[i], s32((s64(mul15(y, amp)) * g) >> 16));  // kit gain 0.5 per voice at unity
        if (k.rev) {
          if (v.pos <= lo + v.step) {
            if (loop) v.pos = hi;
            else { v.on = false; break; }
          } else {
            v.pos -= v.step;
          }
        } else {
          v.pos += v.step;
          if (v.pos >= hi) {
            if (loop) v.pos = lo + (v.pos - hi);
            else { v.on = false; break; }
          }
        }
      }
    }
  }

  s32 newest_envelope() const noexcept override {
    const V* best = nullptr;
    for (const auto& v : v_)
      if (v.on && (!best || v.age > best->age)) best = &v;
    return best ? s32(s64(best->lvl) << 16) : 0;  // Q15 -> Q31
  }

  bool own_command(const Command& c) noexcept override {
    switch (InstrumentOp(c.op)) {
      case InstrumentOp::SetAdsr:  // the drum's envelope page edits dyna_env (see the header)
        dyna_[c.index & 7] = Param(clampv<s32>(c.value, -32768, 32767));
        return true;
      case InstrumentOp::SetSample:
        smp_ = static_cast<const SampleData*>(c.ptr);
        len_ = (smp_ && smp_->frames && smp_->length > 1) ? u32(smp_->length) : 0;
        for (auto& v : v_) v.on = false;  // positions refer to the old sample
        dirty_ = true;
        return true;
      case InstrumentOp::SetDrumKey: {
        const int k = c.index;
        if (k < 0 || k >= kDrumKeys) return true;
        if ((c.slot == 0 || c.slot == 1) && !has_patch_) materialise_slices();
        switch (c.slot) {
          case 0: start_[k] = clampv<int32_t>(c.value, 0, 0x7FFFFFFE); break;
          case 1: end_[k] = clampv<int32_t>(c.value, 0, 0x7FFFFFFE); break;
          case 2: pitch_[k] = int16_t(clampv<s32>(c.value, -32768, 32767)); break;
          case 3: pm_[k] = int16_t(clampv<s32>(c.value, -32768, 32767)); break;
          case 4: rev_[k] = int16_t(clampv<s32>(c.value, -32768, 32767)); break;
          case 5: vol_[k] = int16_t(clampv<s32>(c.value, -32768, 32767)); break;
          default: break;
        }
        dirty_ = true;
        return true;
      }
      default: return false;
    }
  }

  void load_drum_patch(const DrumPatch& p) noexcept override {
    for (auto& v : v_) v.on = false;
    for (int k = 0; k < kDrumKeys; ++k) {
      start_[k] = p.start[k];
      end_[k] = p.end[k];
      pitch_[k] = p.pitch[k];
      vol_[k] = p.volume[k];
      pm_[k] = p.playmode16[k];
      rev_[k] = p.reverse16[k];
    }
    for (int i = 0; i < kParams; ++i) dyna_[i] = p.dyna_env[i];
    has_patch_ = false;
    for (int k = 0; k < kDrumKeys; ++k)
      if (p.end[k] != 0) has_patch_ = true;  // an all-zero kit keeps the equal slices
    dirty_ = true;
  }

  void own_view(InstrumentView& out) const noexcept override {
    uint8_t mask = 0;
    for (int i = 0; i < kDrumVoices; ++i) {
      out.display[i] = v_[i].on ? v_[i].key : -1;
      if (v_[i].on) {
        mask |= uint8_t(1u << i);
        if (v_[i].lvl > out.display[16 + v_[i].key]) out.display[16 + v_[i].key] = v_[i].lvl;
      }
    }
    out.voices_active = mask;
    out.display[40] = int32_t(len_);
    out.display[41] = last_key_;
    out.display[42] = has_patch_ ? 1 : 0;
    const int e = last_key_ < 0 ? 0 : last_key_;
    const Key& k = keys_[e];
    out.display[43] = e;
    out.display[44] = has_patch_ ? start_[e] : int32_t(s64(k.lo) * kQ31PerFrame);
    out.display[45] = has_patch_ ? end_[e] : int32_t(s64(k.hi) * kQ31PerFrame);
    out.display[46] = pitch_[e];
    out.display[47] = pm_[e];
    out.display[48] = rev_[e];
    out.display[49] = vol_[e];
    out.display[50] = k.mode;
    out.display[51] = k.rev ? 1 : 0;
    out.display[52] = int32_t(k.lo);
    out.display[53] = int32_t(k.hi);
    for (int i = 0; i < kParams; ++i) out.adsr[i] = dyna_[i];  // the envelope page shows dyna_env
    out.sample = len_ ? smp_ : nullptr;  // the kit waveform for drum2 (the worker frees retired samples late)
  }

 private:
  struct Key {
    u32 lo = 0, hi = 0;
    s32 pitch_q8 = 0, vol15 = 32768;
    uint8_t mode = 1;
    bool rev = false;
  };
  struct V {
    bool on = false, held = false, releasing = false;
    int key = 0;
    u64 pos = 0, step = u64(1) << 32, age = 0;
    s32 rel = 32767, lvl = 0;
    u32 age_s = 0;
  };
  static constexpr s32 kRelStep = 32767 / 132 + 1;  // 3 ms release fade

  // dyna_env level at slice position p15 (0..32767), Q15 (see the header)
  s32 dyna_amp(s32 p15) const noexcept {
    s32 a = 32767;
    if (att15_ > 0 && p15 < att15_) {
      s32 r = s32((s64(p15) * 32767) / att15_);
      if (att_convex_) r = s32((s64(r) * r) >> 15);
      a = s32((s64(a) * r) >> 15);
    }
    const s32 from_end = 32767 - p15;
    if (dec15_ > 0 && from_end < dec15_) {
      s32 r = s32((s64(from_end) * 32767) / dec15_);
      if (dec_convex_) r = s32((s64(r) * r) >> 15);
      a = s32((s64(a) * r) >> 15);
    }
    return a;
  }

  void start(V& v, int key) noexcept {
    if (dirty_) build_keys();
    const Key& k = keys_[key];
    v.on = len_ > 0 && k.hi > k.lo;
    v.held = true;
    v.releasing = false;
    v.key = key;
    v.pos = u64(k.rev ? k.hi : k.lo) << 32;
    v.rel = 32767;
    v.age_s = 0;
    v.lvl = 0;
    v.age = ++clock_;
    const u64 sr = u64(smp_ && smp_->sample_rate > 0 ? smp_->sample_rate : kSampleRate);
    v.step = ((semis_ratio_q16(k.pitch_q8 + mod_.pitch_q8) << 16) * sr) / kSampleRate;
  }

  // The equal slices as stored start/end values, so a first per-key edit of a kit without a patch keeps the
  // other keys where they were.
  void materialise_slices() noexcept {
    const u32 slice = len_ / kDrumKeys;
    for (int k = 0; k < kDrumKeys; ++k) {
      const u32 a = u32(k) * slice, b = slice ? a + slice - 1 : a;
      start_[k] = int32_t(clampv<s64>(s64(a) * kQ31PerFrame, 0, 0x7FFFFFFE));
      end_[k] = int32_t(clampv<s64>(s64(b) * kQ31PerFrame, 0, 0x7FFFFFFE));
    }
    has_patch_ = true;
  }

  void build_keys() noexcept {
    dirty_ = false;
    last_off_[0] = eff_knob_[1];
    last_off_[1] = eff_knob_[2];
    for (int k = 0; k < kDrumKeys; ++k) {
      Key& key = keys_[k];
      u32 a, b;
      if (has_patch_) {
        a = u32(start_[k] < 0 ? 0 : start_[k]) / 4058;
        b = u32(end_[k] < 0 ? 0 : end_[k]) / 4058;
      } else {
        const u32 slice = len_ / kDrumKeys;
        a = u32(k) * slice;
        b = slice ? a + slice - 1 : a;
      }
      key.rev = rev_[k] >= 16384;
      if (a > b) {
        const u32 t = a;
        a = b;
        b = t;
        key.rev = !key.rev;
      }
      // kit-wide start/end offsets (LFO/MIDI): +-10 % of the slice
      const s64 span = s64(b) - a;
      s64 lo = s64(a) + span * s15(eff_knob_[1]) / 327670;
      s64 hi = s64(b) + span * s15(eff_knob_[2]) / 327670;
      const s64 top = len_ ? s64(len_) - 2 : 0;
      lo = clampv<s64>(lo, 0, top);
      hi = clampv<s64>(hi, 0, top);
      if (hi < lo) hi = lo;
      key.lo = u32(lo);
      key.hi = u32(hi);
      key.pitch_q8 = s32(s64(pitch_[k]) * 3072 / 6208);  // 6208 raw = one octave (see the header)
      const s64 vol = clampv<s32>(vol_[k], 0, 32767);
      key.vol15 = s32((vol * vol) >> 11);  // (vol / 8192)^2 in Q15: 8192 -> 32768
      key.mode = uint8_t(clampv<s32>(pm_[k] >> 13, 0, 2));
    }
  }

  s32 frame_at(u32 i) const noexcept {
    if (i >= len_) return 0;
    if (smp_->channels == 2) return (s32(smp_->frames[2 * i]) + smp_->frames[2 * i + 1]) * 32768;
    return s32(smp_->frames[i]) * 65536;
  }
  s32 read(u64 pos) const noexcept {
    const u32 i = u32(pos >> 32);
    const s32 f = s32((pos >> 16) & 0xffff);
    const s32 a = frame_at(i), b = frame_at(i + 1);
    return interp16(a, b, f);
  }

  const SampleData* smp_ = nullptr;
  u32 len_ = 0;
  int32_t start_[kDrumKeys] = {}, end_[kDrumKeys] = {};
  int16_t pitch_[kDrumKeys] = {}, vol_[kDrumKeys] = {}, pm_[kDrumKeys] = {}, rev_[kDrumKeys] = {};
  Param dyna_[kParams] = {};
  bool has_patch_ = false, dirty_ = true;
  s32 last_off_[2] = {};
  Key keys_[kDrumKeys];
  V v_[kDrumVoices];
  s32 level15_ = 32768;
  s32 att15_ = 0, dec15_ = 0;
  bool att_convex_ = false, dec_convex_ = false;
  u64 clock_ = 0;
  int last_key_ = -1;
};

}  // namespace

Instrument* make_drum(void* mem, size_t size) noexcept {
  return sizeof(DrumSampler) <= size ? new (mem) DrumSampler() : nullptr;
}

}  // namespace op1::dsp
