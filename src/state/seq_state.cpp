// op1.db sequencer states -> Domain::Seq commands (t5, 2026-10-23). See seq_state.h.
#include "seq_state.h"

#include <cmath>

#include "json_read.h"
#include "op1/patch.h"
#include "op1/sequencer.h"
#include "sqlite_read.h"

namespace op1::state {
namespace {

constexpr int16_t kRestStep = -32768;  // seq_engines.h kRest

Command seq_cmd(SeqOp op, int side, int32_t index, int32_t value) {
  Command c;
  c.domain = Domain::Seq;
  c.slot = uint8_t(side);
  c.op = uint16_t(op);
  c.index = index;
  c.value = value;
  return c;
}
// SetParam / Edit addressed to one type, selected or not (SeqOp::SetParamOf / EditOf)
Command param_of(SeqType t, int side, int index, int32_t value) {
  return seq_cmd(SeqOp::SetParamOf, side, (int32_t(t) << 16) | (index & 0xFFFF), value);
}
Command edit_of(SeqType t, int side, int index, int32_t value) {
  return seq_cmd(SeqOp::EditOf, side, (int32_t(t) << 16) | (index & 0xFFFF), value);
}

int num_or(const Json* j, int dflt) {
  return j && j->kind == Json::Number ? int(std::lround(j->num)) : dflt;
}

// Rows of (is_synth, data JSON) -> (side, parsed data). side: is_synth 1 = synth (0), else drum (1).
bool rows_json(const std::vector<uint8_t>& db, const char* table, std::vector<std::pair<int, Json>>& out,
               std::string& err) {
  std::vector<SqlRow> rows;
  std::string e;
  if (!sqlite_table(db, table, rows, e)) {
    err += std::string(err.empty() ? "" : "; ") + table + ": " + e;
    return false;
  }
  for (const SqlRow& r : rows) {
    if (r.cols.size() < 2 || r.cols[1].kind != SqlValue::Text) continue;
    Json j;
    if (!json_parse(r.cols[1].s, j, e) || j.kind != Json::Object) {
      err += std::string(err.empty() ? "" : "; ") + table + ": " + e;
      continue;
    }
    const SqlValue& k = r.cols[0];
    const bool synth = k.kind == SqlValue::Int ? k.i != 0 : (k.kind == SqlValue::Text && k.s == "1");
    out.emplace_back(synth ? 0 : 1, std::move(j));
  }
  return true;
}

void finger(const Json& d, int side, std::vector<Command>& out) {
  out.push_back(param_of(SeqType::Finger, side, 0, num_or(d.get("pm"), 1)));
  out.push_back(param_of(SeqType::Finger, side, 1, num_or(d.get("swing"), 50)));
  if (const Json* len = d.get("length"); len && len->kind == Json::Array)
    for (size_t i = 0; i < len->arr.size() && i < 14; ++i)
      out.push_back(param_of(SeqType::Finger, side, int(2 + i), num_or(&len->arr[i], 16)));
  // notes: 14 slots x 32 steps x 2 voices, slot-major, the two voices of a step next to each other (the layout
  // of FingerSeq::edit's index = slot * 64 + step * 2 + voice)
  if (const Json* n = d.get("notes"); n && n->kind == Json::Array)
    for (size_t i = 0; i < n->arr.size() && i < 14 * 64; ++i)
      out.push_back(edit_of(SeqType::Finger, side, int(i), num_or(&n->arr[i], -1)));
}

void pattern(const Json& d, int side, std::vector<Command>& out) {
  out.push_back(param_of(SeqType::Pattern, side, 0, num_or(d.get("length"), 16)));
  out.push_back(param_of(SeqType::Pattern, side, 1, num_or(d.get("pm"), 2)));
  out.push_back(param_of(SeqType::Pattern, side, 2, num_or(d.get("swing"), 50)));
  out.push_back(param_of(SeqType::Pattern, side, 3, num_or(d.get("start"), 0)));
  // notes[i] = bit mask of the keys on step i (bit k = keyboard key k = note 53 + k)
  if (const Json* n = d.get("notes"); n && n->kind == Json::Array)
    for (size_t i = 0; i < n->arr.size() && i < 16; ++i)
      out.push_back(edit_of(SeqType::Pattern, side, int(256 + i), num_or(&n->arr[i], 0)));
}

void endless(const Json& d, int side, std::vector<Command>& out) {
  out.push_back(param_of(SeqType::Endless, side, 5, 0));  // not recording
  out.push_back(param_of(SeqType::Endless, side, 0, num_or(d.get("timing"), 24)));
  out.push_back(param_of(SeqType::Endless, side, 1, num_or(d.get("playmode"), 2)));
  out.push_back(param_of(SeqType::Endless, side, 2, num_or(d.get("euclidean"), 0)));
  out.push_back(param_of(SeqType::Endless, side, 3, num_or(d.get("euclidean_offset"), 0)));
  out.push_back(param_of(SeqType::Endless, side, 4, num_or(d.get("swing"), 50)));
  out.push_back(edit_of(SeqType::Endless, side, 1, 0));  // clear
  // steps: [key, length, key, length, ...] (a chord of up to 4 keyboard keys, lengths in steps) or null (a rest)
  if (const Json* s = d.get("steps"); s && s->kind == Json::Array)
    for (size_t i = 0; i < s->arr.size() && i < 128; ++i) {
      const Json& st = s->arr[i];
      if (st.kind != Json::Array || st.arr.size() < 2) {
        out.push_back(edit_of(SeqType::Endless, side, 0, kRestStep));
        continue;
      }
      for (size_t k = 0; k + 1 < st.arr.size() && k < 8; k += 2) {
        const int key = num_or(&st.arr[k], 0), len = num_or(&st.arr[k + 1], 1);
        out.push_back(edit_of(SeqType::Endless, side, k == 0 ? 2 : 3, (key & 0xFF) | ((len & 0x7F) << 8)));
      }
    }
}

int seq_type_of(const std::string& name) {
  static const char* names[] = {"pattern", "endless", "tombola", "finger", "sketch", "arpeggio"};
  for (int i = 0; i < 6; ++i)
    if (name == names[i]) return i;
  return -1;
}

}  // namespace

bool sequencer_state_commands(const std::vector<uint8_t>& db, std::vector<Command>& out, std::string& err) {
  std::vector<std::pair<int, Json>> rows;
  if (rows_json(db, "okseq", rows, err))
    for (auto& [side, d] : rows) finger(d, side, out);
  rows.clear();
  if (rows_json(db, "drumseq", rows, err))
    for (auto& [side, d] : rows) pattern(d, side, out);
  rows.clear();
  if (rows_json(db, "endless", rows, err))
    for (auto& [side, d] : rows) endless(d, side, out);

  // tombola physics (knobs: {"value": float}), the same for both sides
  std::vector<SqlRow> knobs;
  std::string e;
  if (sqlite_table(db, "knobs", knobs, e)) {
    for (const SqlRow& r : knobs) {
      if (r.cols.size() < 2 || r.cols[0].kind != SqlValue::Text || r.cols[1].kind != SqlValue::Text) continue;
      Json j;
      if (!json_parse(r.cols[1].s, j, e)) continue;
      const Json* v = j.get("value");
      if (!v || v->kind != Json::Number) continue;
      int index = -1;
      if (r.cols[0].s == "tombolagravity") index = 0;
      else if (r.cols[0].s == "tombolarestitution") index = 1;
      if (index < 0) continue;  // tombolaangle (0.0) is the start angle; the engine starts at 0 already
      for (int side = 0; side < 2; ++side)
        out.push_back(param_of(SeqType::Tombola, side, index, int32_t(std::lround(v->num * 1000.0))));
    }
  } else {
    err += std::string(err.empty() ? "" : "; ") + "knobs: " + e;
  }

  // the selected type per side
  std::vector<SqlRow> sel;
  if (sqlite_table(db, "sequencer", sel, e)) {
    for (const SqlRow& r : sel) {
      if (r.cols.size() < 2 || r.cols[0].kind != SqlValue::Text || r.cols[1].kind != SqlValue::Text) continue;
      const int side = r.cols[0].s == "synth_sequencer" ? 0 : (r.cols[0].s == "drum_sequencer" ? 1 : -1);
      const int t = seq_type_of(r.cols[1].s);
      if (side >= 0 && t >= 0) out.push_back(seq_cmd(SeqOp::Select, side, 0, t));
    }
  }
  return true;
}

bool sequencer_state_commands(const AssetProvider& assets, std::vector<Command>& out, std::string& err) {
  std::vector<uint8_t> db;
  if (!assets.read("op1.db", db)) {
    err = "op1.db not in the firmware image";
    return false;
  }
  return sequencer_state_commands(db, out, err);
}

}  // namespace op1::state
