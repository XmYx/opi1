// A small read-only SQLite 3 reader (t2, 2026-10-23): enough to list the rows of one table from a database
// file held in memory, as the factory/state databases inside the user's firmware image are. No writes, no
// SQL, no indexes: it walks the table's b-tree in rowid order. Used by the worker (factory presets).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace op1::state {

struct SqlValue {
  enum Kind : uint8_t { Null, Int, Real, Text, Blob } kind = Null;
  int64_t i = 0;
  double r = 0;
  std::string s;  // text or blob bytes
};

struct SqlRow {
  int64_t rowid = 0;
  std::vector<SqlValue> cols;  // an INTEGER PRIMARY KEY column (stored as NULL) is filled with the rowid
};

// Every row of `table`, in rowid order. false + err if the file is not SQLite 3, is malformed, or has no
// such table.
bool sqlite_table(const std::vector<uint8_t>& db, const std::string& table, std::vector<SqlRow>& rows,
                  std::string& err);

}  // namespace op1::state
