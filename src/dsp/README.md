# src/dsp: engines, effects and LFOs (track t4)

Everything the OG plays: the 14 instrument engines (12 synth + drum + dbox), the 8 effects (mono for
instruments, stereo for the master), the 7 LFOs, the amplitude envelope, play modes, portamento and voice
allocation. Public contract: `include/op1/dsp.h`. Internal contract: `dsp_internal.h`.

## How the sound was made

The firmware's DSP is hand-written Blackfin code in L1 that has not been decoded statically
(og-behaviour-details.md §7: "t4 has to design these by ear, using the ranges and defaults"). So every
engine, effect and LFO here is **designed by ear on the OG facts**: firmware ids and factory tables, the type
defaults from `op1_factory.db` (`defaults.cpp`), which slots are signed, the A/D/R and portamento ranges, the
four play-mode bands, mono instruments with mono effects, 6 synth voices, 64-frame control rate. The in-house
reference for each design is the Pi app (`app/src/engines|fx|lfo`), translated to integer DSP. Bit-exact parity
with the firmware is open (see "Open" below).

All audio is Q1.31 integer (`dsp_util.h`): uint32 phase accumulators, table-based sine/tanh/exp/tan built once
in `dsp_init()`, saturating arithmetic, no signed overflow. Nothing allocates after construction; every module is
placement-constructed into storage the caller owns, so the core can switch engines on the audio thread.

## Files

| File | What |
|---|---|
| `factory.cpp` | `create_instrument`, `create_effect`, `dsp_init`, `create_lfo` (dispatch, firmware table order) |
| `dsp_util.h`, `tables.cpp` | fixed-point toolkit and its tables |
| `dsp_internal.h` | LFO interface (`Lfo`, `ModOut`, `LfoInputs`, targets), ADSR, per-type makers, storage sizes |
| `defaults.cpp` | type defaults (db `default_params` rows; t4 values for sampler, iter, filter) |
| `instrument_base.*` | shared instrument host: patch values, own mono effect + LFO, note offsets, control tick, view |
| `synth_instrument.h` | 6-voice synth: voice allocation, ADSR, poly/mono/legato/unison, portamento, unison detune |
| `eng_<name>.cpp` | the 12 synth engines (fm, digital, drwave, string, cluster, pulse, sampler, phase, dna, dsynth, voltage, iter) |
| `drum.cpp`, `dbox.cpp` | the drum sampler (user kit, 24 slices) and the dbox drum synth (24 keys x 8 values) |
| `fx_base.h`, `fx_<name>.cpp` | the 8 effects (delay, grid, punch, phone, spring, filter, nitro, cwo), each mono + stereo |
| `lfo_common.cpp`, `lfo_<name>.cpp` | the 7 LFOs (tremolo, value, random, element, midi, bend, crank) |

Each `eng_`, `fx_`, `lfo_` file starts with a comment giving the firmware class, the knob mapping and the
meaning of its screen data. That comment is the reference for the UI (t6).

## Screen data (`InstrumentView`)

- `display[0..15]`: common slots, `instrument_base.h` (voice notes, voice envelopes, play mode, portamento,
  peak, LFO pitch). Drums use `display[0..7]` for the key of each voice, see `drum.cpp` / `dbox.cpp`.
- `display[16..63]`: engine-specific, documented at the top of each `eng_<name>.cpp` / `drum.cpp` / `dbox.cpp`
  (the engine's `view()` writes `d[0]` = `display[16]`).
- `fx_display[0..7]`: the effect's eight parameters as applied; `[8..15]` per effect (`fx_<name>.cpp`).
- `lfo_display[0..15]`: per LFO (`lfo_<name>.cpp`).

## Commands (`InstrumentOp`)

All ops of `dsp.h`. Notes on the ones with t4-defined payloads:

- `SetAdsr` index 4 = play mode (bands of 4096: poly, mono, legato, unison), index 5 = portamento
  (<= 64 off, else 10 ms .. 2 s).
- `SetSample` (`ptr` = `SampleData*`, user audio only): synth sampler and drum sampler. The instrument keeps
  the pointer; the core retires the previous one.
- `SetDrumKey`: `slot` = field, `index` = key 0..23, `value` = stored value. Drum sampler: slot 0 start,
  1 end (sample index x 4058), 2 pitch, 3 playmode, 4 reverse, 5 volume. dbox: slots 8..15 = key value 0..7.
- `LoadDrumPatch` reads `DrumPatch::playmode16/reverse16/dbox_params/dbox_keys` (appended to `patch.h` by t4,
  `docs/interface-changes.md`), not the older `uint8_t` fields.
- Velocity is ignored: the OG keyboard has none (controller mode sends a fixed 100).

## Tests

`tests/dsp/test_dsp.cpp` (ctest `dsp/test_dsp`) renders every engine, effect (mono + stereo) and LFO offline,
checks sound / bounds / parameter ranges / play modes / LFO effect, counts heap allocations (must be 0) and
prints the CPU cost per engine. `OP1_RENDER_DIR=<dir>` writes one WAV per engine, effect and LFO.

## Open

| Item | Why |
|---|---|
| `ls` instrument (id 10) | Unknown what it is (REA unknown `unk_b431a856…`). `create_instrument` returns nullptr, so the core keeps the current engine. |
| Bit-exact parity (`tools/parity/`) | Needs the firmware's L1 DSP routines identified per engine/effect; REA cannot decode Blackfin and no routine has been mapped yet. The integer design keeps parity possible once a routine is known. |
| Knob curves, drum `pitch` unit, `dyna_env` slots 2..7, drum playmode/reverse thresholds | Not settled statically (og-behaviour-details.md §7); the readings chosen are stated in `drum.cpp`. |
| Render block size | `kBlock` = 64 (DMA period); whether the firmware renders in larger blocks is REA unknown `unk_fba82bc8…`. |
