#include "lzma_alone.h"

#include <cstring>

namespace op1 {
namespace {

constexpr int kNumBitModelTotalBits = 11, kNumMoveBits = 5;
constexpr uint16_t kProbInit = 1 << (kNumBitModelTotalBits - 1);
constexpr uint32_t kTopValue = 1u << 24;

struct RangeDecoder {
  const uint8_t* p;
  const uint8_t* end;
  uint32_t range = 0xFFFFFFFFu, code = 0;
  bool bad = false;

  uint8_t byte() {
    if (p >= end) { bad = true; return 0; }
    return *p++;
  }
  bool init() {
    if (byte() != 0) return false;
    for (int i = 0; i < 4; ++i) code = (code << 8) | byte();
    return !bad && code != range;
  }
  void normalize() {
    if (range < kTopValue) { range <<= 8; code = (code << 8) | byte(); }
  }
  uint32_t direct(int n) {
    uint32_t r = 0;
    while (n--) {
      range >>= 1;
      code -= range;
      const uint32_t t = 0u - (code >> 31);
      code += range & t;
      r = (r << 1) + (t + 1);
      normalize();
    }
    return r;
  }
  uint32_t bit(uint16_t* prob) {
    const uint32_t v = *prob, bound = (range >> kNumBitModelTotalBits) * v;
    uint32_t sym;
    if (code < bound) {
      *prob = uint16_t(v + (((1u << kNumBitModelTotalBits) - v) >> kNumMoveBits));
      range = bound;
      sym = 0;
    } else {
      *prob = uint16_t(v - (v >> kNumMoveBits));
      code -= bound;
      range -= bound;
      sym = 1;
    }
    normalize();
    return sym;
  }
  uint32_t tree(uint16_t* probs, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; ++i) m = (m << 1) + bit(probs + m);
    return m - (1u << bits);
  }
  uint32_t tree_rev(uint16_t* probs, int bits) {
    uint32_t m = 1, sym = 0;
    for (int i = 0; i < bits; ++i) {
      const uint32_t b = bit(probs + m);
      m = (m << 1) + b;
      sym |= b << i;
    }
    return sym;
  }
};

constexpr int kLenLow = 3, kLenMid = 3, kLenHigh = 8;
struct LenDecoder {
  uint16_t choice, choice2;
  uint16_t low[16 << kLenLow], mid[16 << kLenMid], high[1 << kLenHigh];
  void init() {
    choice = choice2 = kProbInit;
    for (auto& v : low) v = kProbInit;
    for (auto& v : mid) v = kProbInit;
    for (auto& v : high) v = kProbInit;
  }
  uint32_t decode(RangeDecoder& rc, unsigned pos_state) {
    if (!rc.bit(&choice)) return rc.tree(low + (pos_state << kLenLow), kLenLow);
    if (!rc.bit(&choice2)) return 8 + rc.tree(mid + (pos_state << kLenMid), kLenMid);
    return 16 + rc.tree(high, kLenHigh);
  }
};

}  // namespace

uint32_t crc32_ieee(const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

bool lzma_alone_decode(const uint8_t* in, size_t in_size, size_t max_out, std::vector<uint8_t>& out,
                       std::string& err) {
  if (in_size < 13 + 5) { err = "lzma: stream too short"; return false; }
  const unsigned d = in[0];
  if (d >= 9 * 5 * 5) { err = "lzma: bad properties byte"; return false; }
  const unsigned lc = d % 9, lp = (d / 9) % 5, pb = d / 45;
  uint64_t size = 0;
  for (int i = 0; i < 8; ++i) size |= uint64_t(in[5 + i]) << (8 * i);
  const bool unknown = size == ~uint64_t(0);  // then the stream ends with an end marker
  if (!unknown && size > max_out) { err = "lzma: uncompressed size exceeds the limit"; return false; }
  out.assign(unknown ? size_t(1) << 20 : size_t(size), 0);

  RangeDecoder rc{in + 13, in + in_size};
  if (!rc.init()) { err = "lzma: bad range coder header"; return false; }

  std::vector<uint16_t> lit(size_t(0x300) << (lc + lp), kProbInit);
  uint16_t pos_slot[4][1 << 6], align[1 << 4], spec_pos[1 + 114];
  uint16_t is_match[12 << 4], is_rep[12], is_rep_g0[12], is_rep_g1[12], is_rep_g2[12], is_rep0_long[12 << 4];
  auto fill = [](auto& a) { for (auto& v : a) v = kProbInit; };
  for (auto& a : pos_slot) fill(a);
  fill(align); fill(spec_pos); fill(is_match); fill(is_rep); fill(is_rep_g0); fill(is_rep_g1); fill(is_rep_g2);
  fill(is_rep0_long);
  LenDecoder len_dec, rep_len_dec;
  len_dec.init();
  rep_len_dec.init();

  size_t pos = 0;
  bool ended = false;
  // Unknown size: keep room for one maximal match (273 bytes) and double when it runs out.
  auto room = [&]() {
    if (!unknown || pos + 280 <= out.size()) return true;
    if (out.size() * 2 > max_out) return false;
    out.resize(out.size() * 2);
    return true;
  };
  uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
  unsigned state = 0;
  while (unknown || pos < out.size()) {
    if (!room()) { err = "lzma: output exceeds the limit"; return false; }
    uint8_t* o = out.data();
    const size_t total = out.size();
    const unsigned pos_state = unsigned(pos) & ((1u << pb) - 1);
    if (!rc.bit(&is_match[(state << 4) + pos_state])) {
      const uint8_t prev = pos ? o[pos - 1] : 0;
      uint16_t* probs = &lit[size_t(0x300) * (((unsigned(pos) & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc)))];
      unsigned sym = 1;
      if (state >= 7) {
        if (rep0 >= pos) { err = "lzma: corrupt (literal match beyond output)"; return false; }
        unsigned match = o[pos - rep0 - 1];
        do {
          const unsigned mb = (match >> 7) & 1;
          match <<= 1;
          const unsigned b = rc.bit(&probs[((1 + mb) << 8) + sym]);
          sym = (sym << 1) | b;
          if (mb != b) break;
        } while (sym < 0x100);
      }
      while (sym < 0x100) sym = (sym << 1) | rc.bit(&probs[sym]);
      o[pos++] = uint8_t(sym);
      state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
    } else {
      uint32_t len;
      if (rc.bit(&is_rep[state])) {
        if (pos == 0) { err = "lzma: corrupt (rep at start)"; return false; }
        if (!rc.bit(&is_rep_g0[state])) {
          if (!rc.bit(&is_rep0_long[(state << 4) + pos_state])) {
            if (rep0 >= pos) { err = "lzma: corrupt (short rep beyond output)"; return false; }
            state = state < 7 ? 9 : 11;
            o[pos] = o[pos - rep0 - 1];
            ++pos;
            continue;
          }
        } else {
          uint32_t dist;
          if (!rc.bit(&is_rep_g1[state])) dist = rep1;
          else {
            if (!rc.bit(&is_rep_g2[state])) dist = rep2;
            else { dist = rep3; rep3 = rep2; }
            rep2 = rep1;
          }
          rep1 = rep0;
          rep0 = dist;
        }
        len = rep_len_dec.decode(rc, pos_state);
        state = state < 7 ? 8 : 11;
      } else {
        rep3 = rep2; rep2 = rep1; rep1 = rep0;
        len = len_dec.decode(rc, pos_state);
        state = state < 7 ? 7 : 10;
        const unsigned lstate = len < 4 ? len : 3;
        const uint32_t slot = rc.tree(pos_slot[lstate], 6);
        if (slot < 4) rep0 = slot;
        else {
          const unsigned nd = (slot >> 1) - 1;
          uint32_t dist = (2 | (slot & 1)) << nd;
          if (slot < 14) dist += rc.tree_rev(spec_pos + dist - slot, int(nd));
          else {
            dist += rc.direct(int(nd) - 4) << 4;
            dist += rc.tree_rev(align, 4);
          }
          rep0 = dist;
          if (rep0 == 0xFFFFFFFFu) { ended = true; break; }  // end marker
        }
      }
      len += 2;
      if (rep0 >= pos) { err = "lzma: corrupt (distance beyond output)"; return false; }
      for (uint32_t i = 0; i < len && pos < total; ++i, ++pos) o[pos] = o[pos - rep0 - 1];
    }
    if (rc.bad) { err = "lzma: truncated input"; return false; }
  }
  if (unknown) {
    if (!ended) { err = "lzma: missing end marker"; return false; }
    out.resize(pos);
  } else if (pos != out.size()) {
    err = "lzma: stream ended early";
    return false;
  }
  return true;
}

}  // namespace op1
