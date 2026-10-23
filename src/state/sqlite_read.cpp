// Read-only SQLite 3 table scan (file format: https://www.sqlite.org/fileformat2.html). Every offset is
// bounds-checked: the input is a database from the user's firmware image and is treated as untrusted.
#include "sqlite_read.h"

#include <algorithm>
#include <cstring>

namespace op1::state {
namespace {

class Reader {
 public:
  Reader(const std::vector<uint8_t>& db, std::string& err) : d_(db), err_(err) {}

  bool init() {
    if (d_.size() < 100 || std::memcmp(d_.data(), "SQLite format 3", 16) != 0) return fail("not an SQLite 3 file");
    page_size_ = (uint32_t(d_[16]) << 8) | d_[17];
    if (page_size_ == 1) page_size_ = 65536;
    if (page_size_ < 512 || (page_size_ & (page_size_ - 1))) return fail("bad page size");
    usable_ = page_size_ - d_[20];
    if (usable_ < 480) return fail("bad reserved space");
    pages_ = uint32_t(d_.size() / page_size_);
    return true;
  }

  // Rows of the table b-tree rooted at `root` (1-based page number).
  bool scan(uint32_t root, std::vector<SqlRow>& rows) {
    rows.clear();
    return walk(root, rows, 0);
  }

  bool fail(const char* what) {
    err_ = std::string("sqlite: ") + what;
    return false;
  }

 private:
  bool walk(uint32_t page, std::vector<SqlRow>& rows, int depth) {
    if (depth > 32) return fail("b-tree too deep");
    if (page == 0 || page > pages_) return fail("page number out of range");
    const size_t base = size_t(page - 1) * page_size_;
    const size_t hdr = base + (page == 1 ? 100 : 0);
    if (hdr + 12 > d_.size()) return fail("short page");
    const uint8_t type = d_[hdr];
    const uint32_t ncells = be16(hdr + 3);
    if (type == 0x0D) {  // table leaf
      for (uint32_t c = 0; c < ncells; ++c) {
        const size_t ptr = hdr + 8 + 2 * size_t(c);
        if (ptr + 2 > base + page_size_) return fail("cell pointer out of page");
        size_t p = base + be16(ptr);
        uint64_t payload = 0, rowid = 0;
        if (!varint(p, payload) || !varint(p, rowid)) return false;
        std::vector<uint8_t> rec;
        if (!read_payload(p, payload, base, rec)) return false;
        SqlRow row;
        row.rowid = int64_t(rowid);
        if (!record(rec, row)) return false;
        rows.push_back(std::move(row));
      }
      return true;
    }
    if (type == 0x05) {  // table interior: left children in order, then the right-most child
      for (uint32_t c = 0; c < ncells; ++c) {
        const size_t ptr = hdr + 12 + 2 * size_t(c);
        if (ptr + 2 > base + page_size_) return fail("cell pointer out of page");
        const size_t p = base + be16(ptr);
        if (p + 4 > d_.size()) return fail("cell out of file");
        if (!walk(be32(p), rows, depth + 1)) return false;
      }
      return walk(be32(hdr + 8), rows, depth + 1);
    }
    return fail("not a table b-tree page");
  }

  bool read_payload(size_t p, uint64_t payload, size_t page_base, std::vector<uint8_t>& out) {
    if (payload > (uint64_t(64) << 20)) return fail("payload too large");
    const uint64_t x = usable_ - 35;
    uint64_t local = payload;
    if (payload > x) {
      const uint64_t m = ((uint64_t(usable_) - 12) * 32 / 255) - 23;
      local = m + (payload - m) % (usable_ - 4);
      if (local > x) local = m;
    }
    if (p + local > page_base + page_size_ || p + local > d_.size()) return fail("payload out of page");
    out.assign(d_.begin() + long(p), d_.begin() + long(p + local));
    if (local == payload) return true;
    if (p + local + 4 > d_.size()) return fail("overflow pointer out of file");
    uint32_t next = be32(p + local);
    int guard = 0;
    while (out.size() < payload) {
      if (next == 0 || next > pages_ || ++guard > 100000) return fail("bad overflow chain");
      const size_t ob = size_t(next - 1) * page_size_;
      next = be32(ob);
      const size_t take = size_t(std::min<uint64_t>(payload - out.size(), usable_ - 4));
      out.insert(out.end(), d_.begin() + long(ob + 4), d_.begin() + long(ob + 4 + take));
    }
    return true;
  }

  bool record(const std::vector<uint8_t>& r, SqlRow& row) {
    size_t p = 0;
    uint64_t hsize = 0;
    if (!varint_in(r, p, hsize) || hsize > r.size()) return fail("bad record header");
    std::vector<uint64_t> types;
    while (p < hsize) {
      uint64_t t = 0;
      if (!varint_in(r, p, t)) return fail("bad serial type");
      types.push_back(t);
    }
    size_t q = size_t(hsize);
    for (uint64_t t : types) {
      SqlValue v;
      auto need = [&](size_t n) { return q + n <= r.size(); };
      auto sint = [&](size_t n) {
        int64_t x = (r[q] & 0x80) ? -1 : 0;
        for (size_t k = 0; k < n; ++k) x = int64_t((uint64_t(x) << 8) | r[q + k]);
        return x;
      };
      static constexpr size_t kIntLen[7] = {0, 1, 2, 3, 4, 6, 8};
      if (t == 0) {
        v.kind = SqlValue::Null;
      } else if (t <= 6) {
        if (!need(kIntLen[t])) return fail("record runs past the payload");
        v.kind = SqlValue::Int;
        v.i = sint(kIntLen[t]);
        q += kIntLen[t];
      } else if (t == 7) {
        if (!need(8)) return fail("record runs past the payload");
        uint64_t bits = 0;
        for (int k = 0; k < 8; ++k) bits = (bits << 8) | r[q + size_t(k)];
        std::memcpy(&v.r, &bits, 8);
        v.kind = SqlValue::Real;
        q += 8;
      } else if (t == 8 || t == 9) {
        v.kind = SqlValue::Int;
        v.i = t == 9;
      } else if (t >= 12) {
        const size_t n = size_t((t - (t & 1 ? 13 : 12)) / 2);
        if (!need(n)) return fail("record runs past the payload");
        v.kind = (t & 1) ? SqlValue::Text : SqlValue::Blob;
        v.s.assign(reinterpret_cast<const char*>(r.data() + q), n);
        q += n;
      } else {
        return fail("reserved serial type");
      }
      row.cols.push_back(std::move(v));
    }
    return true;
  }

  bool varint(size_t& p, uint64_t& v) { return varint_in(d_, p, v) || fail("varint out of file"); }
  static bool varint_in(const std::vector<uint8_t>& b, size_t& p, uint64_t& v) {
    v = 0;
    for (int k = 0; k < 9; ++k) {
      if (p >= b.size()) return false;
      const uint8_t c = b[p++];
      if (k == 8) { v = (v << 8) | c; return true; }
      v = (v << 7) | (c & 0x7F);
      if (!(c & 0x80)) return true;
    }
    return true;
  }
  uint32_t be16(size_t p) const { return p + 2 <= d_.size() ? (uint32_t(d_[p]) << 8) | d_[p + 1] : 0; }
  uint32_t be32(size_t p) const {
    return p + 4 <= d_.size()
               ? (uint32_t(d_[p]) << 24) | (uint32_t(d_[p + 1]) << 16) | (uint32_t(d_[p + 2]) << 8) | d_[p + 3]
               : 0;
  }

  const std::vector<uint8_t>& d_;
  std::string& err_;
  uint32_t page_size_ = 0, usable_ = 0, pages_ = 0;
};

}  // namespace

bool sqlite_table(const std::vector<uint8_t>& db, const std::string& table, std::vector<SqlRow>& rows,
                  std::string& err) {
  Reader rd(db, err);
  if (!rd.init()) return false;
  std::vector<SqlRow> master;
  if (!rd.scan(1, master)) return false;
  for (const SqlRow& m : master) {
    // sqlite_master: type, name, tbl_name, rootpage, sql
    if (m.cols.size() < 5 || m.cols[0].s != "table" || m.cols[1].s != table || m.cols[3].kind != SqlValue::Int) continue;
    if (!rd.scan(uint32_t(m.cols[3].i), rows)) return false;
    // An INTEGER PRIMARY KEY column is stored as NULL and reads as the rowid.
    if (m.cols[4].s.find("INTEGER PRIMARY KEY") != std::string::npos)
      for (SqlRow& r : rows)
        if (!r.cols.empty() && r.cols[0].kind == SqlValue::Null) {
          r.cols[0].kind = SqlValue::Int;
          r.cols[0].i = r.rowid;
        }
    return true;
  }
  err = "sqlite: no table " + table;
  return false;
}

}  // namespace op1::state
