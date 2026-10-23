// Tape screen (tape.svg; firmware TapeScreen.cpp). Element names the firmware asks for are listed next to
// "Atape.svg" in the binary's strings: track_inactive, track_active, clips, grid, track1..4, trackno,
// minute/second/frame _msd/_lsd, tapespeed, step, "%i%%", tapecurrspeed, reelleftmid, reelrightmid,
// reelleft, reelright, whitetapeparts, reclevel, mixervol, loop_fwd, loop_rev, play_fwd, play_rev,
// seek_fwd, seek_rev, synth, drum, centerline, loopin, loopout, loop_line, ghost_line; the asserts
// TapeScreen.cpp:336 m_bpm / :338 bar_length tie the clip grid to the tempo. See docs/ui-bindings.md.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "bind.h"
#include "op1/tape.h"
#include "../tape/tape_engine.h"

namespace op1 {
namespace {

// Geometry of the template (user units = display pixels).
constexpr float kHeadX = 159.91f;                                      // centerline: the play head
constexpr float kLaneY[4] = {128.359f, 132.396f, 136.434f, 140.471f};  // the four clip lanes
constexpr float kBarPx = 55.363f;                                      // spacing of the grid ticks = one bar
constexpr float kSpeedBoxHalf = 21.333f;                               // tapecurrspeedbox: 138.667 .. 181.334
constexpr float kVolTop = 4.451f, kVolBottom = 110.129f;               // mixervol line

float bar_frames(const CoreSnapshot& s) {
  return float(kSampleRate) * 60.f * 4.f / (float(std::max<int>(400, s.tempo.bpm_tenths)) / 10.f);
}
float px_per_frame(const CoreSnapshot& s) { return kBarPx / bar_frames(s); }

void bind_tape(BindCtx& c) {
  const TapeView& tv = c.s.tape;
  // ---- counter: minutes : seconds : hundredths (the dim leading zero is authored as #2B2338)
  const int64_t pos = tv.position;
  const int cs = int(pos * 100 / kSampleRate);
  const int mm = cs / 6000, ss = cs / 100 % 60, ff = cs % 100;
  char t[16];
  std::snprintf(t, sizeof t, "%d%d%02d%02d", mm / 10 % 10, mm % 10, ss, ff);
  digits(c, {"minute_msd", "minute_lsd", "second_msd", "second_lsd", "frame_msd", "frame_lsd"}, t);
  tint(c.n("minute_msd"), mm >= 10 ? kColWhite : kColDark);

  // ---- reels turn with the tape; tape speed and direction come with the position
  const float sec = float(pos) / float(kSampleRate);
  const float deg = sec * 120.f;  // one turn per 3 s of tape
  const svg::Box lm = c.box("reelleftmid"), rm = c.box("reelrightmid");
  rotate(c, c.n("reelleft"), deg, lm.cx(), lm.cy());
  rotate(c, c.n("reelright"), deg, rm.cx(), rm.cy());

  // ---- track number box: one digit glyph per track
  one_of(c, {"track1", "track2", "track3", "track4"}, tv.track & 3);

  // ---- transport icon (centre): record, loop, play, or nothing when stopped
  const bool rev = tv.reverse;
  int icon = -1;  // 0 rec, 1 loop_fwd, 2 loop_rev, 3 play_fwd, 4 play_rev
  if (tv.armed && (tv.playing || std::fmod(c.t, 1.0) < 0.5)) icon = 0;  // armed and stopped: blinks
  else if (tv.playing && tv.loop) icon = rev ? 2 : 1;
  else if (tv.playing) icon = rev ? 4 : 3;
  one_of(c, {"rec", "loop_fwd", "loop_rev", "play_fwd", "play_rev"}, icon);
  // overdub armed (shift + record; t6 review): the record icon turns orange (port choice, no firmware element)
  if (tv.display[4] == 1) tint(c.n("rec"), kColOrange);
  // seek arrows while the blue encoder moves the tape
  const bool seeking = since_enc(c, 0) < 0.4;
  show(c.n("seek_fwd"), seeking && !rev && icon < 0);
  show(c.n("seek_rev"), seeking && rev && icon < 0);

  // ---- tape speed: the needle over the scale, and the percent read-out while the white encoder turns
  const int speed = tv.display[5] ? tv.display[5] : (tv.speed ? tv.speed : tape::kSpeedNormal);
  move(c.n("tapecurrspeed"), (float(speed - tape::kSpeedNormal) / float(tape::kSpeedNormal)) * kSpeedBoxHalf, 0);
  const bool speed_pop = since_enc(c, 2) < 1.5;
  svg::Node* sp = c.n("tapespeed");
  show(sp, speed_pop);
  if (speed_pop) {
    char pct[4];
    std::snprintf(pct, sizeof pct, "%02d", std::clamp(speed / 20, 0, 99));  // 50 = normal speed (the authored value)
    text_into(c, slots_lr(c, sp), pct);
  }

  // ---- level: mixer volume of the selected track (line), and its playback/record level (dot)
  const int trk = tv.track & 3;
  const float vol = q15(c.s.mixer.volume[trk]);
  const float vy = kVolBottom - vol * (kVolBottom - kVolTop);
  if (svg::Node* mv = c.n("mixervol")) set_line(mv, 305.346f, kVolBottom, 305.346f, vy);
  const float lvl = clamp01(float(std::max<int>(tv.level[trk], c.s.mixer.meter_l)) / 32767.f);
  move(c.n("reclevel"), 0, -lvl * (kVolBottom - kVolTop));
  tint(c.n("reclevel"), tv.recording ? kColRed : kColWhite);

  // ---- source icons: which instrument plays into the tape, its effect and sequencer
  const bool drum = c.ui.inst_mode == Mode::Drum;
  show(c.n("synth"), !drum);
  show(c.n("drum"), drum);
  const InstrumentView& iv = drum ? c.s.drum : c.s.synth;
  tint(c.n("fx"), iv.fx_on ? kColWhite : kColDark);
  const int32_t* sd = c.s.seq.display + (drum ? 32 : 0);
  show(c.n("seq"), sd[0] != 0);

  // ---- clips, grid and loop are drawn by code (draw_tape); hide the authored sample clips
  show(c.n("clips"), false);
  const float ppf = px_per_frame(c.s);
  const float bar = bar_frames(c.s);
  move(c.n("grid"), -std::fmod(float(pos), bar) * ppf, 0);
  const bool loop = tv.loop && tv.loop_out > tv.loop_in;
  show(c.n("loop_line"), loop);
  show(c.n("loopin"), loop);
  show(c.n("loopout"), loop);
  if (loop) {
    move(c.n("loopin"), float(tv.loop_in - pos) * ppf, 0);
    move(c.n("loopout"), float(tv.loop_out - pos) * ppf, 0);
  }
}

void draw_tape(BindCtx& c, svg::Image& img) {
  const TapeView& tv = c.s.tape;
  const float ppf = px_per_frame(c.s);
  const int64_t pos = tv.position;
  for (int t = 0; t < kTapeTracks; ++t) {
    const float y = kLaneY[t];
    const bool sel = t == (tv.track & 3);
    // the lane
    svg::stroke(img, {{5.5f, y}, {314.5f, y}}, 1.5f, kColDim, false, 1.f, false);
    // every finished clip near the head (TapeView::clips), then the region being recorded right now
    const uint32_t col = sel ? kColBlue : kColRed;
    auto span = [&](int32_t from, int32_t to, uint32_t color, float w) {
      const float x0 = std::max(5.5f, kHeadX + float(from - pos) * ppf), x1 = std::min(314.5f, kHeadX + float(to - pos) * ppf);
      if (x1 > x0) svg::stroke(img, {{x0, y}, {x1, y}}, w, color, false, 1.f, false);
    };
    for (int i = 0; i < tv.nclips && i < TapeView::kMaxViewClips; ++i)
      if (tv.clips[i].ch == t && tv.clips[i].stop > tv.clips[i].start) span(tv.clips[i].start, tv.clips[i].stop, col, 1.5f);
    // the new audio grows behind the head while recording; its thickness follows the input level
    if (tv.recording && tv.rec_track == t && tv.rec_stop > tv.rec_start)
      span(tv.rec_start, std::max(tv.rec_stop, int32_t(pos)), tv.display[4] == 1 ? kColOrange : kColRed, 1.5f + clamp01(float(tv.level[t]) / 16384.f));
  }
  // lift/drop clipboard state: a short bar under the head (tape_engine.h display[0] LiftState)
  if (tv.display[0] == tape::kHolding)
    svg::stroke(img, {{kHeadX - 6, 146.f}, {kHeadX + 6, 146.f}}, 1.5f, kColGreen, false, 1.f, false);
}

}  // namespace

OP1_SCREEN("tape", bind_tape, draw_tape);

}  // namespace op1
