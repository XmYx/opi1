// Type defaults: the values a fresh engine, effect or LFO starts from. Rows marked "db" are the
// default_params column of op1_factory.db as tabled in og-behaviour-details.md §4 (eight numbers per type,
// no preset data). Types without a db row get values chosen here (marked "t4").
#include "dsp_internal.h"

namespace op1::dsp {
namespace {

constexpr Param kKnobs[int(InstrumentType::Count)][kParams] = {
    {20000, 4500, 3000, 18000, 15000, 0, 100, 1500},  // fm        db
    {2000, 5000, -1000, 2000, 0, 0, 0, 0},            // digital   db
    {24000, 16000, 8200, 24000, 32000, 0, 0, 0},      // drwave    db
    {1516, 16704, 0, 15168, 0, 0, 0, 0},              // string    db
    {4000, 4000, 8000, 100, 0, 0, 0, 0},              // cluster   db
    {20000, 16000, 4000, 3000, 0, 0, 0, 0},           // pulse     db
    {0, 0, 32767, 32767, 12000, 0, 0, 13551},         // sampler   no db row: the first factory sampler preset (id 161)
    {8000, 0, 0, 0, 0, 0, 0, 0},                      // phase     db
    {0, 0, 0, 0, 0, 0, 0, 0},                         // drum      t4: kit-wide offsets, 0 = none
    {0, 0, 2917, 974, 7167, 0, 2592, 4607},           // dbox      db (also the per-key default sound)
    {0, 0, 0, 0, 0, 0, 0, 0},                         // ls        open (no engine)
    {-16000, 5000, 0, 1024, 0, 0, 0, 0},              // dna       db
    {20664, 0, 8528, 6225, 328, 0, 12130, 11465},     // dsynth    db
    {1516, 16704, 0, 15168, 0, 0, 0, 0},              // voltage   db (identical to string: factory quirk)
    {1516, 16704, 0, 15168, 0, 0, 0, 0},              // iter      t4: the community-mod row, = string's
};

constexpr Param kFx[int(FxType::Count)][kParams] = {
    {8000, 8000, 8000, 8000, 0, 0, 0, 0},                  // delay   db
    {8000, 8000, 18000, 18000, 8000, 8000, 8000, 8000},    // grid    db
    {6000, 15000, 20000, 28000, 8000, 8000, 8000, 8000},   // punch   db
    {8000, 8000, 8000, 16000, 8000, 8000, 8000, 8000},     // phone   db
    {8000, 15000, 2000, 20000, 8000, 8000, 8000, 8000},    // spring  db
    {23000, 10000, 0, 5000, 8000, 8000, 8000, 8000},       // filter  t4 (hidden, no db row): open LP24, mild res
    {500, 0, 10500, 16000, 0, 0, 0, 0},                    // nitro   db
    {5000, 8192, 16384, 32767, 0, 0, 0, 0},                // cwo     db
};

constexpr Param kLfo[int(LfoType::Count)][kParams] = {
    {16000, 0, 0, 16000, 0, 0, 0, 0},           // tremolo db
    {1000, 4000, 2000, 2000, 0, 0, 0, 0},       // value   db
    {2000, 4000, 2000, 24000, 0, 0, 0, 0},      // random  db
    {2000, 4000, 2000, 2000, 0, 0, 0, 0},       // element db
    {2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000},  // midi db
    {7200, 7100, 32767, 0, 0, 0, 0, 2048},      // bend    db
    {0, 16384, 2000, 2000, 0, 0, 0, 0},         // crank   db
};

constexpr Param kZero[kParams] = {};

}  // namespace

const Param* default_knobs(InstrumentType t) noexcept { return t < InstrumentType::Count ? kKnobs[int(t)] : kZero; }
const Param* default_fx_params(FxType t) noexcept { return t < FxType::Count ? kFx[int(t)] : kZero; }
const Param* default_lfo_params(LfoType t) noexcept { return t < LfoType::Count ? kLfo[int(t)] : kZero; }

}  // namespace op1::dsp

const op1::Param* op1::fx_type_defaults(FxType t) noexcept { return dsp::default_fx_params(t); }
