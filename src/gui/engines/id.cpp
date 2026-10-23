// Synth engine screen "id" (dna). Decoded from the firmware by t3 (SPEC-id.md in the t3 work folder):
// screen object 0x01DD4B54 (ctor 0x019B4F2C, vtable 0x019102D4), init 0x019B4E90, per-frame update 0x019B502C.
//
// What the firmware does each frame (all decoded unless marked):
//   * The 26 tiles s0..s25 are authored stacked in the bottom-left corner. Exactly ONE is visible:
//       a = int(12 p1 + 4.5), b = int((0.375 p1 + 0.140625) p2 32), c = int(15.99988 p2),
//       N = (a + b + c - 1) mod 26   (p1 = green, p2 = white, parameters 0..1);
//     every other tile gets its visible byte cleared (node+0x50 = 0).
//   * Tile N's stroke colour is written into its linestyle as R,G,B,A = 255-8a, 16b, 128+8c, 255 (bytes).
//   * Each tile has a transform: scale s = 2 (p0 + 0.2) (p0 = blue) and a rotation by an angle a_i (radians),
//     both about the screen constant pivot (40, 120) (fixed point 22.10, this+0xE0 / this+0xE4).
//     a_i is smoothed toward a target every frame: a_i += (target_i - a_i) * min(dt, 512) / 1024, with
//     target_i = ((seed >> i) & 15) and seed = (w << 16) + w, w = the last output sample (int16) of the newest
//     dna voice, 0 when that voice is idle (DSP 0x019B48D8 writes it to 0x01DCEB00). Silent: tiles settle
//     back to their authored orientation; playing: they spin to random angles 0..15 rad.
//   * After the SVG is rendered the firmware post-processes the frame buffer (column-major, 256 entries per
//     column, x mirrored): a kaleidoscope. An 80 x 80 patch at x = off1.., y = 80+off2.. (off1 = 40 p1,
//     off2 = 40 p2) is copied, mirrored four ways, around (159.5, 80); then x 0..79 mirror x 80..159 and
//     x 239..318 mirror x 159..238. Each of the four kaleidoscope writes is skipped when an LCG
//     (x 0x19660D + 0x3C6EF35F) draws <= int(8000 p3 - 32768) (p3 = orange): up to ~12 % dropouts.
// ASSUMED: parameter p0 is normalised as (raw + 32768) / 65535 (the firmware divides by a run-time range table
// that was not read); dt is taken as milliseconds (the firmware passes a tick count whose rate was not
// decoded); the dna voice's last sample is approximated by the last mixed output sample (REQUEST in the
// report); framebuffer x is mirrored (inferred from where the source patch must lie to contain the tile).
#include "engine_common.h"

namespace op1 {
namespace {
using namespace eng;

constexpr int kIdTiles = 26;
constexpr float kIdPivotX = 40.f, kIdPivotY = 120.f;  // this+0xE0 / +0xE4 = 0xA000 / 0x1E000 (22.10)

struct IdState {
  float ang[kIdTiles] = {};  // this+0xE8: smoothed angle per tile (radians)
  double last_t = -1;
  uint32_t lcg = 1;          // 0xFF802BC8 in the firmware (shared with the DSP there)
};
IdState g_id;

struct IdParams {
  float p0, p1, p2, p3;
  int a, b, c, n;
};

IdParams id_params(const InstrumentView& iv) {
  IdParams p{};
  p.p0 = clamp01((float(iv.knobs[0]) + 32768.f) / 65535.f);  // ASSUMED range of the bipolar slot
  p.p1 = knob(iv, 1);
  p.p2 = knob(iv, 2);
  p.p3 = knob(iv, 3);
  p.a = int(12.f * p.p1 + 4.5f);
  p.b = int((0.375f * p.p1 + 0.140625f) * p.p2 * 32.f);
  p.c = int(p.p2 * 15.99988f);
  int n = (p.a + p.b + p.c - 1) % kIdTiles;
  if (n < 0) n += kIdTiles;
  p.n = n;
  return p;
}

void bind_id(BindCtx& c) {
  const InstrumentView& iv = c.inst();
  const IdParams p = id_params(iv);

  // seed: last output sample of the newest voice (0 when idle), widened as (w << 16) + w
  int16_t w = 0;
  if (iv.voices_active) w = int16_t(c.s.scope[kDisplayW - 1] >> 16);
  const int32_t seed = int32_t((uint32_t(uint16_t(w)) << 16) + uint32_t(int32_t(w)));
  double dt_ms = g_id.last_t < 0 ? 0.0 : (c.t - g_id.last_t) * 1000.0;
  g_id.last_t = c.t;
  const float k = float(std::clamp(dt_ms, 0.0, 512.0)) / 1024.f;
  for (int i = 0; i < kIdTiles; ++i) {
    const float target = float((seed >> i) & 15);
    g_id.ang[i] = g_id.ang[i] * (1.f - k) + target * k;
  }

  const float s = 2.f * (p.p0 + 0.2f);
  const uint32_t col = (uint32_t((255 - 8 * p.a) & 255) << 16) | (uint32_t((16 * p.b) & 255) << 8) |
                       uint32_t((128 + 8 * p.c) & 255);
  for (int i = 0; i < kIdTiles; ++i) {
    svg::Node* n = c.n("s" + std::to_string(i));
    if (!n) continue;
    if (i != p.n) {
      show(n, false);
      continue;
    }
    show(n, true);
    n->xf = svg::Affine::scale(s, s, kIdPivotX, kIdPivotY)
                .then(svg::Affine::rotate(g_id.ang[i] * 180.f / kPi, kIdPivotX, kIdPivotY));
    tint_tree(n, col);
  }
}

// The kaleidoscope post-process (0x019B5A22 .. 0x019B5CC2), in display units (an image of S x the display
// size is handled by copying S x S blocks).
void draw_id(BindCtx& c, svg::Image& img) {
  const int S = std::max(1, img.w / kDisplayW);
  if (img.w < kDisplayW * S || img.h < kDisplayH * S) return;
  const IdParams p = id_params(c.inst());
  const int off1 = (int(1024.f * p.p1) * 40) >> 10;
  const int off2 = (int(1024.f * p.p2) * 40) >> 10;
  const int thr = int(8000.f * p.p3 - 32768.f);
  const svg::Image src = img;
  auto get = [&](int x, int y) -> uint32_t {
    if (x < 0 || x >= kDisplayW || y < 0 || y >= kDisplayH) return 0xFF000000u;  // wrapped / padding: black
    return src.px[size_t(y * S) * size_t(img.w) + size_t(x * S)];
  };
  auto put = [&](int x, int y, uint32_t v) {
    if (x < 0 || x >= kDisplayW || y < 0 || y >= kDisplayH) return;
    for (int j = 0; j < S; ++j)
      for (int i = 0; i < S; ++i) img.at(x * S + i, y * S + j) = v;
  };
  auto roll = [&]() {
    g_id.lcg = g_id.lcg * 0x19660Du + 0x3C6EF35Fu;
    return thr < (int32_t(g_id.lcg) >> 16);
  };
  constexpr int H = kDisplayW / 4;  // 80 ([0xFF8017C8] = width / 4)
  for (int yy = 0; yy < H; ++yy) {
    for (int xx = 0; xx < H; ++xx) {
      const uint32_t v = get(H - 1 + off1 - yy, H + off2 + xx);
      if (roll()) put(2 * H - 1 - yy, H + xx, v);
      if (roll()) put(2 * H - 1 - yy, H - xx, v);
      if (roll()) put(2 * H - 1 + yy, H + xx, v);
      if (roll()) put(2 * H - 1 + yy, H - xx, v);
    }
  }
  auto copy_col = [&](int from, int to) {
    for (int y = 0; y < kDisplayH * S; ++y)
      for (int i = 0; i < S; ++i) img.at(to * S + i, y) = img.at(from * S + i, y);
  };
  for (int r = 0; r < H; ++r) copy_col(2 * H - 1 - r, r);          // x 0..79   <- x 159..80
  for (int r = 0; r < H; ++r) copy_col(3 * H - 2 - r, 3 * H - 1 + r);  // x 239..318 <- x 238..159
}

OP1_SCREEN("id", bind_id, draw_id);

}  // namespace
}  // namespace op1
