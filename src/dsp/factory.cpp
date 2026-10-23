// Factories of the engine track (t4): the public entry points of dsp.h dispatch to the per-type makers
// (dsp_internal.h), mirroring the firmware's factory tables (synth 0x018F5A04, stereo fx 0x018F5910,
// mono fx 0x018F59AC, LFO 0x018E13AC). Each maker constructs with placement new into the caller's
// storage and never allocates, so the core can switch engines on the audio thread.
#include "dsp_internal.h"

namespace op1 {

void dsp_init() noexcept {
  dsp::init_tables();
  dsp::init_dna_tables();
}

Instrument* create_instrument(InstrumentType type, void* storage, size_t size) noexcept {
  using namespace dsp;
  if (!storage) return nullptr;
  switch (type) {
    case InstrumentType::Fm: return make_fm(storage, size);
    case InstrumentType::Digital: return make_digital(storage, size);
    case InstrumentType::DrWave: return make_drwave(storage, size);
    case InstrumentType::String: return make_string(storage, size);
    case InstrumentType::Cluster: return make_cluster(storage, size);
    case InstrumentType::Pulse: return make_pulse(storage, size);
    case InstrumentType::Sampler: return make_sampler(storage, size);
    case InstrumentType::Phase: return make_phase(storage, size);
    case InstrumentType::Drum: return make_drum(storage, size);
    case InstrumentType::Dbox: return make_dbox(storage, size);
    case InstrumentType::Dna: return make_dna(storage, size);
    case InstrumentType::DSynth: return make_dsynth(storage, size);
    case InstrumentType::Voltage: return make_voltage(storage, size);
    case InstrumentType::Iter: return make_iter(storage, size);
    // ls (id 10) is open: the firmware builds it through the synth factory, but what it is has not been
    // resolved (REA unknown unk_b431a856..., firmware-behaviour.md §11). The core keeps the current engine.
    case InstrumentType::Ls:
    case InstrumentType::Count: break;
  }
  return nullptr;
}

Effect* create_effect(FxType type, bool stereo, void* storage, size_t size) noexcept {
  using namespace dsp;
  if (!storage) return nullptr;
  switch (type) {
    case FxType::Delay: return make_fx_delay(stereo, storage, size);
    case FxType::Grid: return make_fx_grid(stereo, storage, size);
    case FxType::Punch: return make_fx_punch(stereo, storage, size);
    case FxType::Phone: return make_fx_phone(stereo, storage, size);
    case FxType::Spring: return make_fx_spring(stereo, storage, size);
    case FxType::Filter: return make_fx_filter(stereo, storage, size);
    case FxType::Nitro: return make_fx_nitro(stereo, storage, size);
    case FxType::Cwo: return make_fx_cwo(stereo, storage, size);
    case FxType::Count: break;
  }
  return nullptr;
}

namespace dsp {

Lfo* create_lfo(LfoType type, void* storage, size_t size) noexcept {
  if (!storage) return nullptr;
  switch (type) {
    case LfoType::Tremolo: return make_lfo_tremolo(storage, size);
    case LfoType::Value: return make_lfo_value(storage, size);
    case LfoType::Random: return make_lfo_random(storage, size);
    case LfoType::Element: return make_lfo_element(storage, size);
    case LfoType::Midi: return make_lfo_midi(storage, size);
    case LfoType::Bend: return make_lfo_bend(storage, size);
    case LfoType::Crank: return make_lfo_crank(storage, size);
    case LfoType::Count: break;
  }
  return nullptr;
}

}  // namespace dsp
}  // namespace op1
