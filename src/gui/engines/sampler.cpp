// Synth engine screen "sampler" (sampler.svg). Decoded from the firmware's SamplerScreen.cpp (t3, 2026-10-23;
// full pseudo-code with addresses in SPEC-sampler.md). Firmware routines: element init 0x01A34C64, per-frame
// draw 0x01A34FB0 (vtable 0x01911424 +0x1c), column peaks 0x01A343F4, 3-column grouping 0x01A36490, gain
// clamp 0x01A365B8, wave polyline 0x01A361E4, TU constants 0x01A360D8.
//
// What the firmware does each frame (all numbers decoded unless marked):
//   p[i] = stored knob i / 32768 (fract16 -> float, 0x01AA7050).
//   s = p0; li = max(s, p1 + p5/32768); lo = max(li, p2 + p6/32768); e = max(lo, p3)   (k5/k6 = loop fine)
//   gain dB = 20*log10(16*p7^2) printed "%+.0f" into the "gain" text node; linear gain 16*p7^2.
//   play_fwd visible when p4 < 0.5, play_rev otherwise; "shifted" visible while shift is held.
//   Without shift: the overview strip = 290 columns over the whole sample (no gain), darkblue with the
//   start..end columns blue. Always: the magnified view = the window [s - 0.1(e-s), e + 0.1(e-s)] clamped to
//   0..1, one column per pixel, peaks grouped in threes (max; min for the first group), times the gain,
//   clamped to full height; start..end blue, the rest darkblue. The four markers (dot + ring group and the
//   dashed line) are translated to their column in the magnified view and lifted onto the wave's top.
//   Line and ring alpha = 255 while that marker's encoder is being turned (blue/orange only without shift),
//   0 otherwise (animator rise 1000/tick, fall instant).
// The waveform needs the sample's frames, which the snapshot does not carry yet: sampler_sample() is a
// stub (REQUEST in SPEC-sampler.md); without data the firmware's reading of an empty buffer gives flat
// lines on the baseline, which is what is drawn then.
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

// ---- data accessor: the sampler's loaded sample (InstrumentView::sample, filled by the engine's view();
// immutable while published; null when none or when the engine does not fill it yet).
const SampleData* sampler_sample(const BindCtx& c) { return c.inst().sample; }

// palette entries of colors.svg used by the screen (Colors.cpp table at 0x01BAAF50: darkblue = entry 10,
// blue = entry 15)
constexpr uint32_t kPalDarkBlue = 0x383572, kPalBlue = 0x698EFF;
constexpr int kMaxCols = 290;          // 0x122, asserted by the peak routine
constexpr int kQ = 1024;               // the screen's fixed point (Q10, 1024 = 1.0 = 1 pixel)

// One mono sample (stereo is mixed; the OG slot is mono).
int sample_at(const SampleData& d, int64_t i) {
  if (i < 0 || i >= d.length) return 0;
  if (d.channels == 2) return (int(d.frames[2 * i]) + int(d.frames[2 * i + 1])) / 2;
  return d.frames[i];
}

// 0x01A343F4: column peaks of [lo, hi] (fractions of the sample) into out[0..n), each 0..1023.
void column_peaks(const SampleData* d, float lo, float hi, int n, int32_t* out) {
  const int32_t len = (d && d->frames) ? d->length : 0;
  const float L = float(len);
  const auto a = uint32_t(lo * L), b = uint32_t(hi * L);
  const int32_t frames = int32_t(b - a);
  const float step = n > 0 ? float(frames) / float(n) : 0.f;
  if (frames > 40000) {
    // coarse table: per 256-frame block the largest sample (SampleBuffer load, 0x01A34B60), one block per column
    for (int i = 0; i < n; ++i) {
      const auto blk = int64_t(uint32_t((float(i) * step + float(a)) / 256.f));
      int m = 0;
      for (int k = 0; k < 256; ++k) m = std::max(m, sample_at(*d, blk * 256 + k));
      out[i] = m >> 5;
    }
    return;
  }
  const int cnt = std::max(1, int(uint32_t(std::max(step, 0.f))));
  for (int i = 0; i < n; ++i) {
    int m = 0;
    if (len > 0) {
      const auto st = int64_t(uint32_t(float(i) * step + float(a)));
      for (int k = 0; k < cnt; ++k) m = std::max(m, sample_at(*d, st + k));
    }
    out[i] = m >> 5;
  }
}

// 0x01A36490 (called with ZERO, ONE): groups of three columns take their max, the group holding column 0
// its min; the columns after the last whole group repeat it.
void group_threes(int32_t* pk, int n) {
  if (n - 2 <= 0) return;
  const int count = ((2 * (n - 3)) / 3) / 2 + 1;
  for (int k = 0; k < count; ++k) {
    int32_t* g = pk + 3 * k;
    const int32_t v = k == 0 ? std::min({g[0], g[1], g[2]}) : std::max({g[0], g[1], g[2]});
    g[0] = g[1] = g[2] = v;
  }
  for (int i = 3 * count; i < n; ++i) pk[i] = pk[3 * count - 1];
}

struct Q10Box {  // a node's bounds in Q10 pixels, as the firmware stores them
  int64_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  bool ok = false;
};
Q10Box q10_box(const svg::Box& b) {
  Q10Box q;
  if (b.empty()) return q;
  q.x0 = std::llround(b.x0 * kQ), q.x1 = std::llround(b.x1 * kQ);
  q.y0 = std::llround(b.y0 * kQ), q.y1 = std::llround(b.y1 * kQ);
  q.ok = true;
  return q;
}

// 0x01A361E4: the wave as a polyline, one point per column on the box's bottom edge minus the column's
// height; a segment takes the colour of the column it ends in (blue inside [r0, r1] Q10 fractions).
// Written into the group's path children (one per colour run) so the template's later elements (the
// markers) stay on top, as on the device.
void wave_into(svg::Node* g, const Q10Box& bx, const int32_t* pk, int n, int64_t r0, int64_t r1) {
  if (!g) return;
  std::vector<svg::Node*> paths;
  for (svg::Node& k : g->kids)
    if (k.tag == "path") paths.push_back(&k);
  for (svg::Node* p : paths) show(p, false);
  if (!bx.ok || n <= 0 || paths.empty()) return;
  const int64_t h = bx.y1 - bx.y0;
  auto ypx = [&](int32_t v) { return float(bx.y1 - ((h * v) >> 10)) / kQ; };
  // a range starting exactly at 0 starts the line on the baseline (decoded; odd but harmless)
  float px = float(bx.x0) / kQ, py = r0 == 0 ? float(bx.y1) / kQ : ypx(pk[0]);
  size_t run = 0;
  svg::Points pts{{px, py}};
  int col_prev = -1;
  for (int i = 0; i < n; ++i) {
    const int64_t xi = int64_t(i) * kQ;
    const int col = ((int64_t(n) * r0) <= xi && xi <= (int64_t(n) * r1)) ? 1 : 0;
    const float x = float(bx.x0 + xi) / kQ, y = ypx(pk[i]);
    if (col_prev >= 0 && col != col_prev) {  // close the run, start the next at the shared point
      if (run < paths.size()) {
        set_path(paths[run], pts);
        stroke_col(paths[run], col_prev ? kPalBlue : kPalDarkBlue);
        show(paths[run], true);
      }
      ++run;
      pts = {pts.back()};
    }
    pts.emplace_back(x, y);
    col_prev = col;
  }
  if (run < paths.size() && pts.size() > 1) {
    set_path(paths[run], pts);
    stroke_col(paths[run], col_prev ? kPalBlue : kPalDarkBlue);
    show(paths[run], true);
  }
}

struct SmpState {
  float s, li, lo, e, p4, p7;
};
SmpState smp_state(const InstrumentView& iv) {
  auto p = [&](int i) { return float(iv.knobs[i]) / 32768.f; };
  SmpState st;
  st.s = p(0);
  st.li = std::max(st.s, p(1) + p(5) / 32768.f);
  st.lo = std::max(st.li, p(2) + p(6) / 32768.f);
  st.e = std::max(st.lo, p(3));
  st.p4 = p(4);
  st.p7 = p(7);
  return st;
}

// Seconds an encoder counts as "being turned" (the firmware's per-frame turned flag; ASSUMED window, the
// port's events are not frame-synchronous).
constexpr double kTurnWindow = 0.1;

void bind_sampler(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const SmpState st = smp_state(iv);
  const SampleData* sd = sampler_sample(c);
  const bool sh = c.ui.shift;

  show(c.n("shifted"), sh);
  show(c.n("play_fwd"), uint32_t(0.5f + st.p4) == 0);
  show(c.n("play_rev"), uint32_t(0.5f + st.p4) != 0);
  show(c.n("gain"), false);  // a text node on the device: drawn with the font in draw_sampler

  // bounds first (overrides were reset before bind, so these are the authored ones)
  const Q10Box ob = q10_box(c.box("overview")), mb = q10_box(c.box("magnified"));
  int32_t pk[kMaxCols + 3] = {};

  // overview (only without shift; the shifted group takes its place)
  if (svg::Node* ov = c.n("overview")) {
    if (!sh && ob.ok) {
      const int n = std::clamp(int((ob.x1 - ob.x0) >> 10), 0, kMaxCols);
      column_peaks(sd, 0.f, 1.f, n, pk);
      wave_into(ov, ob, pk, n, int64_t(uint32_t(st.s * kQ)), int64_t(uint32_t(st.e * kQ)));
    } else {
      wave_into(ov, ob, pk, 0, 0, 0);
    }
  }

  // magnified window
  const float m = (st.e - st.s) * 0.1f;
  float wlo = std::max(0.f, st.s - m), whi = std::min(1.f, st.e + m);
  if (wlo == whi) wlo = 0.f, whi = 1.f;
  const int n = mb.ok ? std::clamp(int((mb.x1 - mb.x0) >> 10), 0, kMaxCols) : 0;
  std::fill(pk, pk + kMaxCols + 3, 0);
  column_peaks(sd, wlo, whi, n, pk);
  group_threes(pk, n);
  const auto gq = int64_t(uint32_t(16384.f * st.p7 * st.p7));  // linear gain 16*p7^2 in Q10
  for (int i = 0; i < n; ++i) pk[i] = int32_t(std::min<int64_t>((int64_t(pk[i]) * gq) >> 10, kQ));
  auto frac = [&](float v) { return int64_t(uint32_t(std::max(0.f, (v - wlo) / (whi - wlo)) * kQ)); };
  wave_into(c.n("magnified"), mb, pk, n, frac(st.s), frac(st.e));

  // markers: x along the magnified view, lifted onto the wave's top at that column
  static const char* groups[4] = {"start", "loopin", "loopout", "end"};
  static const char* lines[4] = {"startline", "loopinline", "loopoutline", "endline"};
  static const char* rings[4] = {"startring", "loopinring", "loopoutring", "endring"};
  const float mk[4] = {st.s, st.li, st.lo, st.e};
  for (int i = 0; i < 4; ++i) {
    float dx = 0, dy = 0;
    if (mb.ok && n > 0) {
      const int64_t xo = ((mb.x1 - mb.x0 - kQ) * frac(mk[i])) >> 10;
      int64_t yo = 0;
      if (xo != 0) {
        const int col = std::clamp(int(xo >> 10), 0, n - 1);
        yo = (-int64_t(pk[col]) * (mb.y1 - mb.y0)) >> 10;
      }
      dx = float(xo) / kQ, dy = float(yo) / kQ;
    }
    move(c.n(groups[i]), dx, dy);
    move(c.n(lines[i]), dx, dy);
    // turned flag: green/white always, blue/orange only without shift (they drive direction/gain then)
    const bool turned = (sh && (i == 0 || i == 3)) ? false : since_enc(c, i) >= 0 && since_enc(c, i) < kTurnWindow;
    if (svg::Node* l = c.n(lines[i])) l->alpha = turned ? 1.f : 0.f;
    if (svg::Node* r = c.n(rings[i])) r->alpha = turned ? 1.f : 0.f;
  }
}

void draw_sampler(BindCtx& c, svg::Image& img) {
  // the "gain" text node: "%+.0f" of 20*log10(16*p7^2); only visible inside "shifted"
  if (!c.ui.shift || !c.font.ok()) return;
  const SmpState st = smp_state(c.inst());
  char b[16];
  std::snprintf(b, sizeof b, "%+.0f", 20.0 * std::log10(16.0 * double(st.p7) * double(st.p7)));
  // ASSUMED placement: right-aligned on the authored placeholder glyph (right edge 292.4, cap top 7.6, 20 high)
  text_right(c.font, img, 292.4f, 7.6f, 20.f, b, kColRed);
}

OP1_SCREEN("sampler", bind_sampler, draw_sampler);

}  // namespace
}  // namespace op1
