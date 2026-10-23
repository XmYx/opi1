// The 4-track tape (owner: t5). Replaces the t3 placeholder. Behaviour and value meanings: tape_engine.h.
//
// Memory: 4 x kTapeLength 16-bit samples plus a 4-track clipboard, mapped and prefaulted in create_tape()
// (main thread). Audio thread: process() reads and writes that memory only; long copies (lift, drop,
// clear) are cut into kJobSlice pieces, one piece per period.
#include <sys/mman.h>

#include <cstring>
#include <new>

#include "clips.h"
#include "op1/fract.h"
#include "op1/tape.h"
#include "tape_engine.h"

namespace op1 {
namespace {

using namespace tape;

// Anonymous memory, populated (write-faulted) now so the audio thread never page-faults on first touch.
int16_t* map_prefaulted(size_t samples, size_t& bytes) {
  bytes = samples * sizeof(int16_t);
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  volatile char* c = static_cast<volatile char*>(p);
  for (size_t off = 0; off < bytes; off += 4096) c[off] = 0;  // in case populate was partial
  return static_cast<int16_t*>(p);
}

class TapeImpl final : public Tape {
 public:
  TapeImpl() {
    mem_ = map_prefaulted(size_t(kTapeTracks) * kTapeLength, mem_bytes_);
    clip_ = map_prefaulted(size_t(kTapeTracks) * kClipboardMaxLength, clip_bytes_);
  }
  ~TapeImpl() override {
    munmap(mem_, mem_bytes_);
    munmap(clip_, clip_bytes_);
  }
  TapeImpl(const TapeImpl&) = delete;
  TapeImpl& operator=(const TapeImpl&) = delete;

  void command(const Command& c) noexcept override {
    switch (TapeOp(c.op)) {
      case TapeOp::Play: playing_ = true; break;
      case TapeOp::Stop: playing_ = false; finish_recording(); break;
      case TapeOp::Record:
        rec_ = c.value != 0;
        if (!rec_) finish_recording();
        break;
      case TapeOp::SelectTrack:
        finish_recording();
        track_ = uint8_t(c.value & 3);
        break;
      case TapeOp::Seek: finish_recording(); pos_ = int64_t(clamp_pos(c.value)) << 16; break;
      case TapeOp::SetLoop: loop_ = c.value != 0; break;
      case TapeOp::SetLoopIn: loop_in_ = clamp_pos(c.value); break;
      case TapeOp::SetLoopOut: loop_out_ = c.value < 0 ? 0 : (c.value > kTapeLength ? kTapeLength : c.value); break;
      case TapeOp::SetSpeed: speed_ = c.value < 0 ? 0 : (c.value > kSpeedMax ? kSpeedMax : c.value); break;
      case TapeOp::SetReverse: finish_recording(); reverse_ = c.value != 0; break;
      case TapeOp::Lift: lift(c.index == 1); break;
      case TapeOp::Drop: drop(); break;
      case TapeOp::SetInputRouting: routing_ = c.value; break;
      case TapeOp::SetRecordMode: overdub_ = c.value == kOverdub; break;
      case TapeOp::ClearTrack: clear_track(c.slot & 3); break;
      case TapeOp::Split:
        finish_recording();
        if (c.index == 1) clips_.join(track_, int32_t(pos_ >> 16));
        else if (!clips_.split(track_, int32_t(pos_ >> 16))) ++clips_refused_;
        break;
    }
  }

  void transport(const Event& e, int16_t bpm_tenths) noexcept override {
    switch (e.type) {
      case EventType::Start: finish_recording(); pos_ = 0; playing_ = true; break;
      case EventType::Continue: playing_ = true; break;
      case EventType::Stop: playing_ = false; finish_recording(); break;
      case EventType::SongPosition: {
        if (playing_ || bpm_tenths <= 0) break;
        // float, in the firmware's order (0x019E3560-0x019E3600)
        const float bpm = float(bpm_tenths) / 10.0f;
        const float s = float(e.value < 0 ? 0 : e.value) * 10584000.0f / bpm / 16.0f + 0.5f;
        if (s >= float(kTapeLength)) break;  // past the tape: ignored
        finish_recording();
        pos_ = int64_t(int32_t(s)) << 16;
        break;
      }
      default: break;
    }
  }

  void process(const ProcessContext& ctx, const Sample* rec, Sample* const tracks[kTapeTracks]) noexcept override {
    job_step();
    const int n = ctx.frames;
    int32_t peak[kTapeTracks] = {};
    if (!playing_) {
      for (int t = 0; t < kTapeTracks; ++t) std::memset(tracks[t], 0, sizeof(Sample) * size_t(n));
      if (ctx.block_start) for (int t = 0; t < kTapeTracks; ++t) level_[t] = 0;
      return;
    }
    const int64_t step = (int64_t(speed_) << 16) / kSpeedNormal;
    const bool writing = rec_ && job_ != kClearing;
    for (int i = 0; i < n; ++i) {
      const int32_t p = int32_t(pos_ >> 16);
      const uint32_t fr = uint32_t(pos_ & 0xFFFF);
      for (int t = 0; t < kTapeTracks; ++t) {
        Sample out = 0;
        if (!(writing && !overdub_ && t == track_)) out = read(t, p, fr);
        tracks[t][i] = out;
        const int32_t a = bf::abs_fr1x32(out);
        if (a > peak[t]) peak[t] = a;
      }
      // advance, then write the positions the head passed over
      int64_t np = reverse_ ? pos_ - step : pos_ + step;
      const int32_t q = int32_t(np >> 16);
      if (writing) {
        const int16_t s = to_tape(rec[i]);
        if (q == p) put(p, s);
        else if (q > p) for (int32_t k = p; k < q; ++k) put(k, s);
        else for (int32_t k = p; k > q; --k) put(k, s);
      }
      if (loop_ && loop_out_ > loop_in_) {
        const int64_t len = int64_t(loop_out_ - loop_in_) << 16;
        if (!reverse_ && p < loop_out_ && q >= loop_out_) np -= len;
        else if (reverse_ && p >= loop_in_ && q < loop_in_) np += len;
      }
      if (np < 0 || (np >> 16) >= kTapeLength) {  // the end (or the start, in reverse) stops the tape
        np = np < 0 ? 0 : int64_t(kTapeLength - 1) << 16;
        pos_ = np;
        playing_ = false;
        finish_recording();
        for (int k = i + 1; k < n; ++k)
          for (int t = 0; t < kTapeTracks; ++t) tracks[t][k] = 0;
        break;
      }
      pos_ = np;
    }
    for (int t = 0; t < kTapeTracks; ++t) {
      const int16_t lv = int16_t(peak[t] >> 16);
      if (ctx.block_start || lv > level_[t]) level_[t] = lv;
    }
  }

  void view(TapeView& v) const noexcept override {
    v.position = int32_t(pos_ >> 16);
    v.playing = playing_;
    v.recording = rec_ && playing_;
    v.armed = rec_;
    v.loop = loop_;
    v.reverse = reverse_;
    v.track = track_;
    v.loop_in = loop_in_;
    v.loop_out = loop_out_;
    v.speed = int16_t(speed_);
    for (int t = 0; t < kTapeTracks; ++t) v.level[t] = level_[t];
    for (int32_t& d : v.display) d = 0;
    v.display[0] = job_ != kIdle ? job_ : (clip_len_ ? kHolding : kIdle);
    v.display[1] = clip_len_;
    v.display[2] = clip_all_;
    v.display[3] = clips_.count();
    v.display[4] = overdub_ ? kOverdub : kReplace;
    v.display[5] = speed_;
    const int32_t p = int32_t(pos_ >> 16);
    for (int t = 0; t < kTapeTracks; ++t) {
      const int c = clips_.find(t, p);
      v.display[6 + 2 * t] = c < 0 ? -1 : clips_.at(c).start;
      v.display[7 + 2 * t] = c < 0 ? -1 : clips_.at(c).stop;
    }
    // the region being written, and the clips near the head (nearest first, insertion sort over <= 48)
    v.rec_track = w_track_;
    v.rec_start = recording_ ? w_lo_ : 0;
    v.rec_stop = recording_ ? w_hi_ + 1 : 0;
    v.nclips = 0;
    int32_t dist[TapeView::kMaxViewClips];
    for (int i = 0; i < clips_.count(); ++i) {
      const Clip& c = clips_.at(i);
      if (c.stop < p - TapeView::kTapeViewSpan || c.start > p + TapeView::kTapeViewSpan) continue;
      const int32_t d = p < c.start ? c.start - p : (p > c.stop ? p - c.stop : 0);
      int n = v.nclips;
      if (n == TapeView::kMaxViewClips) {
        if (d >= dist[n - 1]) continue;
        --n;
      } else {
        ++v.nclips;
      }
      while (n > 0 && dist[n - 1] > d) { dist[n] = dist[n - 1]; v.clips[n] = v.clips[n - 1]; --n; }
      dist[n] = d;
      v.clips[n] = {c.start, c.stop, c.ch};
    }
    v.display[14] = routing_;
    v.display[15] = int32_t(clips_refused_);
    v.display[16] = int32_t(jobs_refused_);
  }

 private:
  static int32_t clamp_pos(int32_t v) noexcept { return v < 0 ? 0 : (v >= kTapeLength ? kTapeLength - 1 : v); }

  int16_t* track_mem(int t) const noexcept { return mem_ + size_t(t) * kTapeLength; }
  int16_t* clip_mem(int row) const noexcept { return clip_ + size_t(row) * kClipboardMaxLength; }

  // Linear interpolation between p and p + 1 (fr = 1/65536 frame), Q1.31 out.
  Sample read(int t, int32_t p, uint32_t fr) const noexcept {
    const int16_t* m = track_mem(t);
    const int32_t s0 = m[p];
    const int32_t s1 = p + 1 < kTapeLength ? m[p + 1] : s0;
    return bf::sat32((int64_t(s0) << 16) + int64_t(s1 - s0) * int64_t(fr));
  }

  void put(int32_t q, int16_t s) noexcept {
    if (q < 0 || q >= kTapeLength || q == last_put_) return;
    int16_t& m = track_mem(track_)[q];
    m = overdub_ ? bf::sat16(int32_t(m) + s) : s;
    last_put_ = q;
    if (!recording_) {
      recording_ = true;
      w_track_ = track_;
      w_lo_ = w_hi_ = q;
    }
    if (q < w_lo_) w_lo_ = q;
    if (q > w_hi_) w_hi_ = q;
  }

  void finish_recording() noexcept {
    if (recording_ && !clips_.add(w_track_, w_lo_, w_hi_ + 1)) ++clips_refused_;
    recording_ = false;
    last_put_ = -1;
  }

  // ---- lift / drop / clear: a copy job in slices
  void lift(bool all) noexcept {
    if (job_ != kIdle) { ++jobs_refused_; return; }
    finish_recording();
    int32_t a, b;
    if (loop_ && loop_out_ > loop_in_) {
      a = loop_in_;
      b = loop_out_;
    } else {
      const int c = clips_.find(track_, int32_t(pos_ >> 16));
      if (c < 0) return;  // nothing under the head
      a = clips_.at(c).start;
      b = clips_.at(c).stop;
    }
    if (b - a >= kClipboardMaxLength) b = a + kClipboardMaxLength - 1;  // a lift must be shorter than 90 s
    clip_len_ = b - a;
    clip_all_ = all;
    job_mask_ = all ? 0xF : uint8_t(1u << track_);
    for (int t = 0; t < kTapeTracks; ++t)
      if (job_mask_ & (1u << t)) clips_.erase(t, a, b);
    start_job(kLifting, a, clip_len_);
  }
  void drop() noexcept {
    if (job_ != kIdle) { ++jobs_refused_; return; }
    if (clip_len_ == 0) return;
    finish_recording();
    const int32_t at = int32_t(pos_ >> 16);
    int32_t len = clip_len_;
    if (at + len > kTapeLength) len = kTapeLength - at;
    job_mask_ = clip_all_ ? 0xF : uint8_t(1u << track_);
    start_job(kDropping, at, len);
  }
  void clear_track(int t) noexcept {
    if (job_ != kIdle) { ++jobs_refused_; return; }
    finish_recording();
    clips_.clear(t);
    job_mask_ = uint8_t(1u << t);
    start_job(kClearing, 0, kTapeLength);
  }
  void start_job(LiftState j, int32_t at, int32_t len) noexcept {
    job_ = j;
    job_at_ = at;
    job_len_ = len;
    job_done_ = 0;
  }
  void job_step() noexcept {
    if (job_ == kIdle) return;
    int32_t n = job_len_ - job_done_;
    if (n > kJobSlice) n = kJobSlice;
    const size_t bytes = size_t(n) * sizeof(int16_t);
    for (int t = 0; t < kTapeTracks; ++t) {
      if (!(job_mask_ & (1u << t))) continue;
      int16_t* tape = track_mem(t) + job_at_ + job_done_;
      int16_t* board = clip_mem(clip_all_ ? t : 0) + job_done_;
      switch (job_) {
        case kLifting: std::memcpy(board, tape, bytes); std::memset(tape, 0, bytes); break;
        case kDropping: std::memcpy(tape, board, bytes); break;
        case kClearing: std::memset(tape, 0, bytes); break;
        default: break;
      }
    }
    job_done_ += n;
    if (job_done_ < job_len_) return;
    if (job_ == kDropping)
      for (int t = 0; t < kTapeTracks; ++t)
        if ((job_mask_ & (1u << t)) && !clips_.add(t, job_at_, job_at_ + job_len_)) ++clips_refused_;
    job_ = kIdle;
  }

  int16_t* mem_ = nullptr;
  int16_t* clip_ = nullptr;
  size_t mem_bytes_ = 0, clip_bytes_ = 0;

  int64_t pos_ = 0;  // Q16 frames
  int speed_ = kSpeedNormal;
  bool playing_ = false, rec_ = false, overdub_ = false, reverse_ = false, loop_ = false;
  uint8_t track_ = 0;
  int32_t loop_in_ = 0, loop_out_ = 0;
  int32_t routing_ = 0;
  int16_t level_[kTapeTracks] = {};

  Clips clips_;
  bool recording_ = false;
  uint8_t w_track_ = 0;
  int32_t w_lo_ = 0, w_hi_ = 0, last_put_ = -1;
  uint32_t clips_refused_ = 0, jobs_refused_ = 0;

  int32_t clip_len_ = 0;
  bool clip_all_ = false;
  LiftState job_ = kIdle;
  uint8_t job_mask_ = 0;
  int32_t job_at_ = 0, job_len_ = 0, job_done_ = 0;
};

}  // namespace

std::unique_ptr<Tape> create_tape() { return std::make_unique<TapeImpl>(); }

}  // namespace op1
