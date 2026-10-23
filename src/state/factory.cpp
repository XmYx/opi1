#include "factory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "json_read.h"
#include "sqlite_read.h"

namespace op1::state {
namespace {

Param to_param(double v) { return Param(std::clamp<long>(std::lround(v), -32768, 32767)); }

template <size_t N>
void read_params(const Json* a, Param (&out)[N]) {
  if (!a || a->kind != Json::Array) return;
  for (size_t i = 0; i < N && i < a->arr.size(); ++i)
    if (a->arr[i].kind == Json::Number) out[i] = to_param(a->arr[i].num);
}
template <class T, size_t N>
void read_ints(const Json* a, T (&out)[N]) {
  if (!a || a->kind != Json::Array) return;
  for (size_t i = 0; i < N && i < a->arr.size(); ++i)
    if (a->arr[i].kind == Json::Number) out[i] = T(std::llround(a->arr[i].num));
}
double num(const Json& o, const char* key, double fallback) {
  const Json* v = o.get(key);
  return v && v->kind == Json::Number ? v->num : fallback;
}
bool flag(const Json& o, const char* key) {
  const Json* v = o.get(key);
  return v && ((v->kind == Json::Bool && v->b) || (v->kind == Json::Number && v->num != 0));
}
std::string text(const Json& o, const char* key) {
  const Json* v = o.get(key);
  return v && v->kind == Json::String ? v->str : std::string();
}

void common_fx_lfo(const Json& o, FxType& fx, bool& fx_on, Param (&fxp)[kParams], LfoType& lfo, bool& lfo_on,
                   Param (&lfop)[kParams]) {
  fx_type_from_name(text(o, "fx_type"), fx);
  fx_on = flag(o, "fx_active");
  read_params(o.get("fx_params"), fxp);
  lfo_type_from_name(text(o, "lfo_type"), lfo);
  lfo_on = flag(o, "lfo_active");
  read_params(o.get("lfo_params"), lfop);
}

}  // namespace

bool instrument_type_from_name(const std::string& n, InstrumentType& t) {
  static const std::pair<const char*, InstrumentType> k[] = {
      {"fm", InstrumentType::Fm},           {"digital", InstrumentType::Digital}, {"drwave", InstrumentType::DrWave},
      {"string", InstrumentType::String},   {"cluster", InstrumentType::Cluster}, {"pulse", InstrumentType::Pulse},
      {"sampler", InstrumentType::Sampler}, {"phase", InstrumentType::Phase},     {"drum", InstrumentType::Drum},
      {"dbox", InstrumentType::Dbox},       {"dna", InstrumentType::Dna},         {"dsynth", InstrumentType::DSynth},
      {"voltage", InstrumentType::Voltage}, {"iter", InstrumentType::Iter},
  };
  for (const auto& e : k)
    if (n == e.first) { t = e.second; return true; }
  return false;
}
bool fx_type_from_name(const std::string& n, FxType& t) {
  static const char* k[] = {"delay", "grid", "punch", "phone", "spring", "filter", "nitro", "cwo"};
  for (int i = 0; i < int(FxType::Count); ++i)
    if (n == k[i]) { t = FxType(i); return true; }
  return false;
}
bool lfo_type_from_name(const std::string& n, LfoType& t) {
  static const char* k[] = {"tremolo", "value", "random", "element", "midi", "bend", "crank"};
  for (int i = 0; i < int(LfoType::Count); ++i)
    if (n == k[i]) { t = LfoType(i); return true; }
  return false;
}

bool FactoryLibrary::open(const AssetProvider& assets, std::string& err) {
  std::vector<uint8_t> db;
  if (!assets.read("op1_factory.db", db)) { err = "op1_factory.db not in the firmware image"; return false; }
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<SqlRow> rows;
    if (!sqlite_table(db, pass ? "drum_presets" : "synth_presets", rows, err)) return false;
    auto& out = pass ? drum_ : synth_;
    out.clear();
    for (const SqlRow& r : rows) {
      if (r.cols.size() < 2 || r.cols[1].kind != SqlValue::Text) continue;
      FactoryEntry e;
      e.id = int(r.cols[0].kind == SqlValue::Int ? r.cols[0].i : r.rowid);
      e.json = r.cols[1].s;
      if (r.cols.size() > 2) e.folder = r.cols[2].s;
      Json j;
      std::string jerr;
      if (!json_parse(e.json, j, jerr)) continue;
      e.name = text(j, "name");
      e.type = text(j, "type");
      out.push_back(std::move(e));
    }
  }
  if (synth_.empty() && drum_.empty()) { err = "op1_factory.db has no presets"; return false; }
  return true;
}

int FactoryLibrary::first_of(bool drum, const std::string& type) const {
  const auto& v = drum ? drum_ : synth_;
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i].type == type) return int(i);
  return -1;
}

bool FactoryLibrary::parse_synth(const std::string& json, SynthPatch& p, std::string& err) {
  Json j;
  if (!json_parse(json, j, err)) return false;
  p = SynthPatch{};
  const std::string type = text(j, "type");
  if (!instrument_type_from_name(type, p.type) || is_drum(p.type)) { err = "unknown synth type '" + type + "'"; return false; }
  const std::string name = text(j, "name");
  std::strncpy(p.name, name.c_str(), sizeof p.name - 1);
  p.octave = int8_t(std::clamp<double>(num(j, "octave", 0), -4, 4));
  p.synth_version = int16_t(num(j, "synth_version", 0));
  read_params(j.get("knobs"), p.knobs);
  read_params(j.get("adsr"), p.adsr);
  common_fx_lfo(j, p.fx_type, p.fx_active, p.fx_params, p.lfo_type, p.lfo_active, p.lfo_params);
  p.base_freq = float(num(j, "base_freq", 0));
  return true;
}

bool FactoryLibrary::parse_drum(const std::string& json, DrumPatch& p, std::string& err) {
  Json j;
  if (!json_parse(json, j, err)) return false;
  p = DrumPatch{};
  const std::string type = text(j, "type");
  if (!instrument_type_from_name(type, p.type) || !is_drum(p.type)) { err = "unknown drum type '" + type + "'"; return false; }
  p.drum_version = int16_t(num(j, "drum_version", num(j, "synth_version", 0)));
  p.octave = int8_t(std::clamp<double>(num(j, "octave", 0), -4, 4));
  read_params(j.get("dyna_env"), p.dyna_env);
  common_fx_lfo(j, p.fx_type, p.fx_active, p.fx_params, p.lfo_type, p.lfo_active, p.lfo_params);
  read_ints(j.get("start"), p.start);
  read_ints(j.get("end"), p.end);
  read_params(j.get("pitch"), p.pitch);
  read_params(j.get("volume"), p.volume);
  read_params(j.get("playmode"), p.playmode16);
  read_params(j.get("reverse"), p.reverse16);
  for (int k = 0; k < kDrumKeys; ++k) {  // the legacy 8-bit fields (t4 reads the 16-bit ones)
    p.playmode[k] = uint8_t(std::clamp(p.playmode16[k] >> 7, 0, 255));
    p.reverse[k] = uint8_t(std::clamp(p.reverse16[k] >> 7, 0, 255));
  }
  if (const Json* d = j.get("dbox_data"); d && d->kind == Json::Array) {
    int k = 0;
    for (const Json& key : d->arr) {
      if (k >= kDrumKeys) break;
      read_params(&key, p.dbox_params[k]);
      ++k;
    }
    p.dbox_keys = uint8_t(k);
  }
  return true;
}

SampleData* FactoryLibrary::load_raw(const AssetProvider& assets, const std::string& rel, float root_hz) {
  std::vector<uint8_t> bytes;
  if (!assets.read("audio/" + rel, bytes) || bytes.size() < 4) return nullptr;
  const size_t n = std::min<size_t>(bytes.size() / 2, size_t(kSampleRate) * 20);  // the OG's kits are <= 12 s
  auto* frames = new int16_t[n];
  for (size_t i = 0; i < n; ++i) frames[i] = int16_t(uint16_t(bytes[2 * i]) | (uint16_t(bytes[2 * i + 1]) << 8));
  auto* s = new SampleData;
  s->frames = frames;
  s->length = int32_t(n);
  s->channels = 1;
  s->sample_rate = kSampleRate;
  s->root_hz = root_hz;
  return s;
}

}  // namespace op1::state
