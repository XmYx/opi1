// Factory presets and their audio from the user's firmware image (t2, 2026-10-23). The OG keeps its presets in
// op1_factory.db (tables synth_presets / drum_presets: id, patch JSON, folder) and their audio as raw 16-bit
// little-endian mono 44.1 kHz files: the sampler's in content/audio/factory_synth/<name>.raw, the drum kits'
// in content/audio/factory_drum/<name>.raw (firmware string 0x018C79F4 "%s/content/audio/factory_drum/%s.raw").
// Everything is read from memory; nothing is written anywhere.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "op1/patch.h"
#include "op1/ui.h"

namespace op1::state {

struct FactoryEntry {
  int id = 0;
  std::string folder, name, type;  // type as stored: "fm", "sampler", "drum", "dbox", ...
  std::string json;                // the patch column
};

class FactoryLibrary {
 public:
  // Reads op1_factory.db from the image. false + err if the image has none or it cannot be read.
  bool open(const AssetProvider& assets, std::string& err);

  const std::vector<FactoryEntry>& synths() const { return synth_; }
  const std::vector<FactoryEntry>& drums() const { return drum_; }
  // First entry of that type ("sampler", "drum", "dbox", ...), or -1.
  int first_of(bool drum, const std::string& type) const;

  // Patch JSON -> struct. false + err on a type this port does not know or malformed JSON.
  static bool parse_synth(const std::string& json, SynthPatch& p, std::string& err);
  static bool parse_drum(const std::string& json, DrumPatch& p, std::string& err);

  // content/audio/<rel> from the image as a SampleData owned by the caller (frames allocated with new[],
  // freed by the worker when the core retires it). nullptr if the file is missing or empty.
  static SampleData* load_raw(const AssetProvider& assets, const std::string& rel, float root_hz);

 private:
  std::vector<FactoryEntry> synth_, drum_;
};

// Synth/fx/lfo type names as op1_factory.db spells them -> the port's enums. false if unknown.
bool instrument_type_from_name(const std::string& name, InstrumentType& t);
bool fx_type_from_name(const std::string& name, FxType& t);
bool lfo_type_from_name(const std::string& name, LfoType& t);

}  // namespace op1::state
