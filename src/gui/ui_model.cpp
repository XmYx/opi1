// The OP-1 UI model (owner: t6). See ui_model.h for the page/key scheme and bind.h for the screen bindings.
#include "ui_model.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include "../state/factory.h"
#include "bind.h"
#include "bitmap_text.h"
#include "op1/core.h"
#include "op1/dsp.h"
#include "op1/io.h"
#include "op1/sequencer.h"
#include "op1/tape.h"
#include "op1/ui.h"

namespace op1 {

namespace {

int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

Command cmd(Domain d, uint16_t op, int32_t index, int32_t value, uint8_t slot = 0) {
  Command c;
  c.domain = d;
  c.op = op;
  c.index = index;
  c.value = value;
  c.slot = slot;
  return c;
}

const char* engine_screen(InstrumentType t) {
  switch (t) {
    case InstrumentType::Fm: return "fm";
    case InstrumentType::Digital: return "t10";
    case InstrumentType::DrWave: return "drw";
    case InstrumentType::String: return "st";
    case InstrumentType::Cluster: return "cls";
    case InstrumentType::Pulse: return "pls";
    case InstrumentType::Sampler: return "sampler";
    case InstrumentType::Phase: return "pd";
    case InstrumentType::Drum: return "drum2";
    case InstrumentType::Dbox: return "dbox";
    case InstrumentType::Ls: return nullptr;  // no screen in the firmware (§11)
    case InstrumentType::Dna: return "id";
    case InstrumentType::DSynth: return "dsynth";
    case InstrumentType::Voltage: return "slump";
    case InstrumentType::Iter: return "iter";
    default: return nullptr;
  }
}

}  // namespace

// firmware-behaviour.md §5 (class -> screen file): punch = Mllp, phone = Ptch, spring = Rymd, filter = Mmmf,
// nitro = Ftwo, cwo = Bode.
const char* fx_screen(FxType t) {
  static const char* n[] = {"delay", "grid", "mllp", "ptch", "rymd", "mmmf", "ftwo", "bode"};
  return n[size_t(t) % 8];
}
// §6: tremolo = LfoDual, value = LfoSingle, random = LfoRnd, element = LfoReRoute, midi, bend, crank.
const char* lfo_screen(LfoType t) {
  static const char* n[] = {"duallfo", "singlelfo", "rndlfo", "reroutelfo", "midilfo", "bendlfo", "cranklfo"};
  return n[size_t(t) % 7];
}
// §8: finger is "ok" internally. Arpeggio: "simple" (its strings: staff, notes, hold; t10 system notes).
const char* seq_screen(SeqType t) {
  static const char* n[] = {"pattern", "endless", "tombola", "ok", "sketch", "simple"};
  return n[size_t(t) % 6];
}

namespace {

const char* mode_name(Mode m) {
  static const char* n[] = {"SYNTH", "DRUM", "TAPE", "MIXER", "TEMPO", "ALBUM", "BROWSER", "COM", "CLOCK", "LANDER",
                            "TUNE", "BATTERY", "DEBUG", "CLASS-D", "CODEC", "GAIN", "SIGNAL FLOW"};
  return n[size_t(m) % 17];
}

constexpr uint32_t kRedPx = 0xFFFF3A5D, kWhitePx = 0xFFF0F0F0, kDimPx = 0xFF606070, kOrangePx = 0xFFFF6A1F;

void draw_fallback(svg::Image& im, const CoreSnapshot& s) {
  using namespace bitmap;
  rect(im, 0, 0, kDisplayW, kDisplayH, 0xFF000000u);
  rect(im, 0, 0, int(int64_t(s.tape.position) * kDisplayW / kTapeLength), 2, s.tape.recording ? kRedPx : kDimPx);
  for (int i = 0; i < kSynthVoices; ++i) rect(im, 8 + i * 12, 8, 8, 8, (s.synth.voices_active >> i) & 1 ? kOrangePx : 0xFF303030u);
  draw_scope(im, s, 0, 20, kDisplayW, 100, 0xE0E0E0);
  text(im, 8, 126, "NO FIRMWARE IMAGE: SCREENS NOT DRAWN", kRedPx);
  text(im, 8, 140, "RUN WITH --FIRMWARE PATH OR OP1_OG_FIRMWARE", kWhitePx);
}

void draw_missing(svg::Image& im, const std::string& what) {
  using namespace bitmap;
  rect(im, 0, 0, kDisplayW, kDisplayH, 0xFF000000u);
  const std::string b = "HAS NO SCREEN TEMPLATE IN THE FIRMWARE";
  text(im, (kDisplayW - text_w(what)) / 2, 66, what, kWhitePx);
  text(im, (kDisplayW - text_w(b)) / 2, 80, b, kDimPx);
}

// Hash of every run-time override in a document: the raster is rebuilt only when this changes.
void hash_overrides(const svg::Node& n, uint64_t& h) {
  auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
  auto mixf = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); mix(u); };
  if (n.visible >= 0 || n.dx != 0 || n.dy != 0 || n.tinted || n.alpha != 1 || !n.xf.identity() || !n.over.empty() || n.glyph) {
    mix(reinterpret_cast<uintptr_t>(&n));
    mix(uint64_t(n.visible + 2));
    mixf(n.dx); mixf(n.dy);
    mix(n.tinted ? n.tint + 1 : 0);
    mixf(n.alpha);
    mixf(n.xf.a); mixf(n.xf.b); mixf(n.xf.c); mixf(n.xf.d); mixf(n.xf.e); mixf(n.xf.f);
    for (const auto& kv : n.over) {
      for (char c : kv.first) mix(uint8_t(c));
      for (char c : kv.second) mix(uint8_t(c));
    }
    mix(reinterpret_cast<uintptr_t>(n.glyph));
    if (n.glyph) { mixf(n.glyph_xf.a); mixf(n.glyph_xf.e); mixf(n.glyph_xf.f); }
  }
  for (const svg::Node& k : n.kids) hash_overrides(k, h);
}

// t5 (2026-10-23): effect slots the firmware reads as signed (-32768..32767): the encoder walks the full range
// there; elsewhere 0..32767 (PARITY-AUDIT.md §4.2 gap 6: before, nitro's negative follow could not be reached).
bool fx_slot_signed(FxType t, int slot) {
  switch (t) {
    case FxType::Nitro: return slot == 1;  // follow, cubed with its sign (Ftwo 0x019A6308); presets go to -31744
    // every other slot is read unsigned (logical shifts, squares, max(s, 33)); cwo's slot 0 is an offset from a
    // centre value but stored 0..32767 (decode-fx.md Q1)
    default: return false;
  }
}

// t5 (2026-10-23): the drum sampler's encoders edit the last played key (DrumController 0x01977430 -> 0x01A1E57C,
// decode-drums.md Q1, F): word = encoder + 4 x shift; one detent: pitch +-194 (-24320..24832), start / end coarse
// +-(end - start) / 32 (at least 32 high units), playmode +-1024 (4096..20480), reverse +-512 (8192..24576), start
// / end fine +-8192 (2 frames), volume +-154 (0..32767); 0 <= start <= end <= 0x7FFFFFFE. The key's values come
// from the drum view (drum.cpp display[43..49]); SetDrumKey slots 0 start, 1 end, 2 pitch, 3 playmode, 4 reverse,
// 5 volume.
template <typename Impl>
void drum_key_edit(Impl& I, const InstrumentView& iv, int word, int det) {
  const int key = iv.display[43];
  const int64_t start = iv.display[44], end = iv.display[45];
  auto send = [&](int slot, int64_t v) {
    Command c;
    c.domain = Domain::Drum;
    c.op = uint16_t(InstrumentOp::SetDrumKey);
    c.slot = uint8_t(slot);
    c.index = key;
    c.value = int32_t(v);
    I.post(c);
  };
  const int64_t span_hi = std::max<int64_t>((end >> 16) - (start >> 16), 32);
  const int64_t coarse = std::max<int64_t>(1, (1024 * span_hi) >> 15) << 16;
  switch (word) {
    case 0: send(2, std::clamp<int64_t>(iv.display[46] + det * 194, -24320, 24832)); break;
    case 1: send(0, std::clamp<int64_t>(start + det * coarse, 0, end)); break;
    case 2: send(1, std::clamp<int64_t>(end + det * coarse, start, 0x7FFFFFFE)); break;
    case 3: send(3, std::clamp<int64_t>(iv.display[47] + det * 1024, 4096, 20480)); break;
    case 4: send(4, std::clamp<int64_t>(iv.display[48] + det * 512, 8192, 24576)); break;
    case 5: send(0, std::clamp<int64_t>(start + det * 8192, 0, end)); break;
    case 6: send(1, std::clamp<int64_t>(end + det * 8192, start, 0x7FFFFFFE)); break;
    default: send(5, std::clamp<int64_t>(iv.display[49] + det * 154, 0, 32767)); break;
  }
}

// t5 (2026-10-23): the effect after `t` in the firmware's menu. The OG lists the fx_types rows in row order
// (SELECT id, type FROM fx_types, no ORDER BY; decode-fx.md Q2): grid 3, punch 4, delay 5, phone 7, spring 8,
// cwo 9, nitro 10. The hidden filter has no row, so it is never offered (before, T3 stepped the enum order and
// reached it). From the filter (only a loaded patch can hold it) the next is the first of the list.
FxType next_fx(FxType t) {
  static const FxType order[] = {FxType::Grid, FxType::Punch, FxType::Delay, FxType::Phone,
                                 FxType::Spring, FxType::Cwo, FxType::Nitro};
  constexpr int n = int(sizeof order / sizeof order[0]);
  for (int i = 0; i < n; ++i)
    if (order[i] == t) return order[(i + 1) % n];
  return order[0];
}

}  // namespace

struct UiModel::Impl {
  Core& core;
  AssetProvider& assets;
  UiState ui;
  std::string shown;
  std::vector<std::string> names;
  std::map<std::string, std::unique_ptr<svg::Document>> docs;
  svg::Font font;
  int browse = -1;
  int sounding[24];
  bool entry[24] = {};             // t5: keys held for sequencer step entry (shift + key on the seq page)
  double seq_t[2] = {-1e9, -1e9};  // when a sequencer encoder last turned, per side
  int master_fx[8];                // master effect parameters as last sent
  svg::Image raster{kDisplayW, kDisplayH};
  uint64_t raster_key = 0;
  const svg::Document* raster_doc = nullptr;
  bool env_screen_done = false;
  int eng_sent = 0;                // synth engine last asked for, and when
  double eng_t = -1e9;
  int pm_sent = 0;                 // play mode last sent, and when
  double pm_t = -1e9;
  // keys task: encoder push = back to the value the parameter had when its engine/effect/LFO was loaded
  // (captured from the snapshot when the type changes), per side (synth, drum)
  int def_type[2][3] = {{-1, -1, -1}, {-1, -1, -1}};  // instrument, fx and lfo type the values belong to
  Param def_knobs[2][kParams] = {}, def_adsr[2][kParams] = {}, def_fx[2][kParams] = {}, def_lfo[2][kParams] = {};
  Worker* worker = nullptr;
  bool break_resume = false;       // tape trick 4 (break): play again when the key is released
  // preset browser (t6 review): the factory library read once from the image, the presets per folder as
  // indices into the side's table (the worker's LoadFactoryPreset index), and what each sound slot holds
  state::FactoryLibrary lib;
  bool lib_ok = false;
  std::vector<std::string> pb_folders[2];
  std::vector<std::vector<int>> pb_index[2];
  int slot_preset[2][8];           // -1: the slot's default (factory preset n of that side)

  void build_browser_lists() {
    std::string err;
    lib_ok = assets.available() && lib.open(assets, err);
    if (!lib_ok) return;
    for (int side = 0; side < 2; ++side) {
      const auto& v = side ? lib.drums() : lib.synths();
      for (size_t i = 0; i < v.size(); ++i) {
        InstrumentType t;  // only presets the worker can build (a known type)
        if (!state::instrument_type_from_name(v[i].type, t)) continue;
        const std::string& f = v[i].folder.empty() ? v[i].type : v[i].folder;
        auto it = std::find(pb_folders[side].begin(), pb_folders[side].end(), f);
        const size_t k = size_t(it - pb_folders[side].begin());
        if (it == pb_folders[side].end()) { pb_folders[side].push_back(f); pb_index[side].emplace_back(); }
        pb_index[side][k].push_back(int(i));
      }
    }
  }
  const state::FactoryEntry* preset_entry(int side, int index) const {
    const auto& v = side ? lib.drums() : lib.synths();
    return index >= 0 && size_t(index) < v.size() ? &v[size_t(index)] : nullptr;
  }
  // record arm as last sent (the snapshot lags a key press by a few ms: record + note in one go)
  bool armed_sent = false;
  double arm_t = -1e9;
  void arm(bool on) {
    post(cmd(Domain::Tape, uint16_t(TapeOp::Record), 0, on ? 1 : 0));
    armed_sent = on;
    arm_t = ui.t;
  }
  bool armed(const CoreSnapshot& s) const { return ui.t - arm_t < 0.2 ? armed_sent : s.tape.armed; }
  // the factory preset a sound slot plays: what was picked in the browser, else factory preset n
  int slot_index(int side, int n) const { return slot_preset[side][n & 7] >= 0 ? slot_preset[side][n & 7] : n; }
  int cur_preset[2] = {-1, -1};    // factory preset last loaded per side (-1: the boot sound, no preset)
  // browsing previews (Sir): the preset under the cursors is loaded once the cursors rest, so the keyboard and
  // the sequencer play it; the push keeps it on the slot, leaving the browser without a push restores the slot
  bool pb_dirty = false, pb_previewing = false;
  double pb_turn_t = -1e9;
  int pb_side() const { return ui.inst_mode == Mode::Drum ? 1 : 0; }
  int pb_cursor_index() const {
    const auto& idx = pb_index[pb_side()];
    if (ui.pb_folder < 0 || size_t(ui.pb_folder) >= idx.size()) return -1;
    const auto& f = idx[size_t(ui.pb_folder)];
    return ui.pb_preset >= 0 && size_t(ui.pb_preset) < f.size() ? f[size_t(ui.pb_preset)] : -1;
  }
  void load_preset(int side, int index) {
    if (index < 0) return;
    cur_preset[side] = index;
    if (worker) worker->request(cmd(side ? Domain::Drum : Domain::Synth, uint16_t(WorkerOp::LoadFactoryPreset), index, 0));
  }
  void leave_browser() {  // called before the mode changes away from the browser
    if (ui.mode != Mode::Browser) return;
    if (pb_previewing) {  // back to what played when the browser opened
      const int side = pb_side();
      if (prev_preset >= 0) load_preset(side, prev_preset);
      else post(cmd(side ? Domain::Drum : Domain::Synth, uint16_t(InstrumentOp::SetType), 0, prev_type));  // boot sound
      cur_preset[side] = prev_preset;
    }
    pb_previewing = pb_dirty = false;
  }
  int prev_preset = -1;            // what played when the browser opened (restored when leaving without a push)
  int prev_type = 0;               //  and its engine, for the boot sound that has no preset

  void capture_defaults(const CoreSnapshot& s) {
    for (int side = 0; side < 2; ++side) {
      const InstrumentView& iv = side ? s.drum : s.synth;
      int* k = def_type[side];
      if (k[0] != int(iv.type)) {
        k[0] = int(iv.type);
        std::memcpy(def_knobs[side], iv.knobs, sizeof iv.knobs);
        std::memcpy(def_adsr[side], iv.adsr, sizeof iv.adsr);
      }
      if (k[1] != int(iv.fx_type)) { k[1] = int(iv.fx_type); std::memcpy(def_fx[side], iv.fx, sizeof iv.fx); }
      if (k[2] != int(iv.lfo_type)) { k[2] = int(iv.lfo_type); std::memcpy(def_lfo[side], iv.lfo, sizeof iv.lfo); }
    }
  }

  Impl(Core& c, AssetProvider& a) : core(c), assets(a) {
    std::fill(std::begin(sounding), std::end(sounding), -1);
    std::fill(std::begin(master_fx), std::end(master_fx), 4000);  // mixer.cpp default: punch with 8 x 4000
    for (auto& s : slot_preset) std::fill(std::begin(s), std::end(s), -1);
    build_browser_lists();
    if (a.available()) names = a.list("svg");
    if (svg::Document* f = doc("opfont")) font.load(*f);
  }

  svg::Document* doc(const std::string& name) {
    auto it = docs.find(name);
    if (it != docs.end()) return it->second.get();
    std::vector<uint8_t> bytes;
    std::unique_ptr<svg::Document> d;
    if (assets.available() && assets.read(name, bytes)) {
      d = std::make_unique<svg::Document>();
      std::string err;
      if (!d->load(std::string(bytes.begin(), bytes.end()), err)) {
        std::fprintf(stderr, "op1-og: screen %s: %s\n", name.c_str(), err.c_str());
        d.reset();
      }
    }
    return docs.emplace(name, std::move(d)).first->second.get();
  }

  void post(const Command& c) { core.post_ui(c); }
  void set_core_mode(Mode m) { post(cmd(Domain::Core, uint16_t(CoreOp::SetMode), 0, int32_t(m))); }
  bool inst_mode() const { return ui.mode == Mode::Synth || ui.mode == Mode::Drum; }
  int side() const { return ui.mode == Mode::Drum || (!inst_mode() && ui.inst_mode == Mode::Drum) ? 1 : 0; }
  Domain inst_domain() const { return side() ? Domain::Drum : Domain::Synth; }
  void to_instrument() {  // T keys and the sequencer key outside synth/drum act on the last instrument
    if (inst_mode()) return;
    leave_browser();
    ui.mode = ui.inst_mode;
    set_core_mode(ui.mode);
  }

  // The template for what is on screen now; nullptr = no template.
  const char* want(const CoreSnapshot& s) const {
    if (ui.t - ui.octave_t < 0.9) return "octave";  // the octave pop-up
    const InstrumentView& iv = side() ? s.drum : s.synth;
    switch (ui.page) {
      case Page::Envelope: return side() ? "dynaenv" : "adsr";
      case Page::PlayMode: return "playmode";
      case Page::Fx: return fx_screen(ui.mode == Mode::Mixer ? s.mixer.master_fx : iv.fx_type);
      case Page::Lfo: return lfo_screen(iv.lfo_type);
      case Page::Seq: return seq_screen(side() ? s.seq.drum_seq : s.seq.synth_seq);
      case Page::Eq: return "eq";
      case Page::Master: return "master";
      case Page::TapeConfig: return "tapeconfig";
      case Page::Help: return "help";
      case Page::Input: return "in";
      case Page::MicLine: return "micline";
      case Page::Main: break;
    }
    switch (ui.mode) {
      case Mode::Synth: return engine_screen(s.synth.type);
      case Mode::Drum: return engine_screen(s.drum.type);
      case Mode::Tape: return "tape";
      case Mode::Mixer: return "mixer";
      case Mode::Tempo: return "tempo";
      case Mode::Album: return "album";
      case Mode::Browser: return "presetbrowser";
      case Mode::Communication: return "com";
      case Mode::Clock: return "clock";
      case Mode::Lander: return "lander";
      case Mode::Tune: return "tune";
      case Mode::SignalFlow: return "signalflow";
      default: return nullptr;  // battery, debug, class-d, codec, gain: service screens without a template
    }
  }

  void toggle_page(Page p) { ui.page = ui.page == p ? Page::Main : p; }
};

UiModel::UiModel(Core& core, AssetProvider& assets) : impl_(std::make_unique<Impl>(core, assets)) {}
UiModel::~UiModel() = default;

const std::string& UiModel::screen() const { return impl_->shown; }
const UiState& UiModel::state() const { return impl_->ui; }
bool UiModel::firmware() const { return impl_->assets.available(); }
std::vector<std::string> UiModel::screen_names() const { return impl_->names; }

void UiModel::set_mode(Mode m) {
  Impl& I = *impl_;
  UiState& ui = I.ui;
  if (ui.power_off) return;
  I.browse = -1;
  if (ui.shift) {  // shift + mode key: the mode's options
    if (m == Mode::Tape) { I.leave_browser(); ui.mode = m; ui.page = Page::TapeConfig; I.set_core_mode(m); return; }
    if (m == Mode::Tempo) {
      {
        const bool on = !I.core.snapshot().tempo.metronome;
        I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetMetronome), 0, on ? 1 : 0));
        I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetMetronome), 0, on ? ui.metronome_level : 0));  // t5: the click
      }
      return;
    }
    if (m == Mode::Mixer) m = Mode::SignalFlow;              // guide 14.3: shift + mixer = signal path
    else if (m == Mode::Communication) m = Mode::Album;      // the album/com key (ASSUMED: shift = album)
  }
  if (m != Mode::Browser) I.leave_browser();
  if (m == ui.mode && ui.page != Page::Main) ui.page = Page::Main;  // the mode key again: back to its main page
  else { ui.mode = m; ui.page = Page::Main; }
  if (m == Mode::Synth || m == Mode::Drum) ui.inst_mode = m;
  // the browser keeps the keyboard on the instrument being browsed (the core sends notes in other modes to
  // the synth), so a drum kit preview plays on the drum keys
  I.set_core_mode(m == Mode::Browser ? ui.inst_mode : m);
}

void UiModel::key(OpKey k, bool down) {
  Impl& I = *impl_;
  UiState& ui = I.ui;
  if (k == OpKey::Shift) {
    // t5: releasing shift on the sequencer page ends step entry (the cursor moves on, a recording session ends)
    if (!down && ui.shift && ui.page == Page::Seq && I.inst_mode())
      I.post(cmd(Domain::Seq, uint16_t(SeqOp::KeyEdit), -1, 0, uint8_t(I.side())));
    ui.shift = down;
    return;
  }
  auto tape = [&](TapeOp op, int32_t v, int32_t index = 0) { I.post(cmd(Domain::Tape, uint16_t(op), index, v)); };
  if (k == OpKey::Record) ui.rec_held = down;
  if (!down) {
    if (k == OpKey::S4 && I.break_resume) { tape(TapeOp::Play, 0); I.break_resume = false; }  // break released
    return;
  }
  if (ui.power_off) return;
  I.browse = -1;
  const CoreSnapshot& s = I.core.snapshot();
  const bool shift = ui.shift;
  if (k >= OpKey::S1 && k <= OpKey::S8) { sound_key(int(k) - int(OpKey::S1)); return; }
  switch (k) {
    case OpKey::T1: case OpKey::T2: case OpKey::T3: case OpKey::T4: {
      const int t = int(k) - int(OpKey::T1);
      if (ui.mode == Mode::Tape) { tape(TapeOp::SelectTrack, t); return; }
      if (ui.mode == Mode::Mixer) {
        if (t == 0) ui.page = Page::Main;
        else if (t == 1) ui.page = Page::Eq;
        else if (t == 3) ui.page = Page::Master;
        else if (shift) I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetMasterFxOn), 0, s.mixer.master_fx_on ? 0 : 1));
        else if (ui.page == Page::Fx)
          I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetMasterFx), 0, int(next_fx(s.mixer.master_fx))));
        else ui.page = Page::Fx;
        return;
      }
      I.to_instrument();
      const InstrumentView& iv = I.side() ? s.drum : s.synth;
      const Domain d = I.inst_domain();
      if (t == 0) {
        if (shift) engine_step(1);
        ui.page = Page::Main;
      } else if (t == 1) {
        ui.page = shift && !I.side() ? Page::PlayMode : Page::Envelope;
      } else if (t == 2) {
        if (shift) I.post(cmd(d, uint16_t(InstrumentOp::SetFxOn), 0, iv.fx_on ? 0 : 1));
        else if (ui.page == Page::Fx) I.post(cmd(d, uint16_t(InstrumentOp::SetFxType), 0, int(next_fx(iv.fx_type))));
        ui.page = Page::Fx;
      } else {
        if (shift) I.post(cmd(d, uint16_t(InstrumentOp::SetLfoOn), 0, iv.lfo_on ? 0 : 1));
        else if (ui.page == Page::Lfo) I.post(cmd(d, uint16_t(InstrumentOp::SetLfoType), 0, (int(iv.lfo_type) + 1) % int(LfoType::Count)));
        ui.page = Page::Lfo;
      }
      return;
    }
    case OpKey::Seq: {
      I.to_instrument();
      const uint8_t side = uint8_t(I.side());
      const SeqType cur = side ? s.seq.drum_seq : s.seq.synth_seq;
      if (shift) {  // next sequencer type
        I.post(cmd(Domain::Seq, uint16_t(SeqOp::Select), 0, (int(cur) + 1) % int(SeqType::Count), side));
        I.post(cmd(Domain::Seq, uint16_t(SeqOp::SetEnabled), 0, 1, side));
        ui.page = Page::Seq;
      } else if (ui.page == Page::Seq && s.seq.display[32 * side] != 0) {
        // t5: the sequencer key on its own page turns that side's sequencer off (notes pass straight through
        // again) and goes back to the instrument. Before, nothing sent SetEnabled 0, so once on a side stayed
        // sequenced until restart (PARITY-AUDIT.md §4.3).
        I.post(cmd(Domain::Seq, uint16_t(SeqOp::SetEnabled), 0, 0, side));
        ui.page = Page::Main;
      } else if (s.seq.display[32 * side] != 0) {  // on: show it
        ui.page = Page::Seq;
      } else {
        I.post(cmd(Domain::Seq, uint16_t(SeqOp::Select), 0, int(cur), side));
        I.post(cmd(Domain::Seq, uint16_t(SeqOp::SetEnabled), 0, 1, side));
        ui.page = Page::Seq;
      }
      return;
    }
    case OpKey::Help:
      if (shift) { set_mode(Mode::Clock); return; }  // guide 11.2: shift + help = tools (time and date)
      I.toggle_page(Page::Help);
      return;
    case OpKey::Input: I.toggle_page(shift ? Page::MicLine : Page::Input); return;
    case OpKey::Record:
      // record arms (it blinks while stopped); record + play or record + a note starts the tape (t6 review, Sir).
      // shift + record = overdub: the new audio is summed onto the track instead of replacing it (port choice;
      // the engine's TapeOp::SetRecordMode). Record again disarms; shift + record while armed switches the mode.
      if (shift) {
        tape(TapeOp::SetRecordMode, s.tape.display[4] == 1 && I.armed(s) ? 0 : 1);  // 1 = overdub, 0 = replace
        if (!I.armed(s)) I.arm(true);
      } else if (I.armed(s)) {
        I.arm(false);
      } else {
        tape(TapeOp::SetRecordMode, 0);
        I.arm(true);
      }
      return;
    case OpKey::Play:
      if (s.tape.playing) {
        tape(TapeOp::Stop, 0);
        if (I.armed(s)) I.arm(false);  // stopping disarms, as the Stop key does
      } else {
        tape(TapeOp::Play, 0);
      }
      return;
    case OpKey::Stop:
      tape(TapeOp::Stop, 0);
      if (I.armed(s)) I.arm(false);  // stop disarms, or the next Play records over the take
      return;
    case OpKey::Left: case OpKey::Right: {
      if (I.inst_mode() && ui.page == Page::Seq) {  // t5: << >> belong to the sequencer page (cursor, octave)
        I.post(cmd(Domain::Seq, uint16_t(SeqOp::Nav), k == OpKey::Left ? 0 : 1, shift ? 1 : 0, uint8_t(I.side())));
        return;
      }
      if (I.inst_mode()) { octave(k == OpKey::Left ? -1 : 1); return; }  // guide 3.2: the arrows shift the octave
      // one beat at the current tempo (shift: one bar)
      const int64_t beat = int64_t(kSampleRate) * 600 / std::max<int>(400, s.tempo.bpm_tenths);
      const int64_t step = (shift ? 4 : 1) * beat * (k == OpKey::Left ? -1 : 1);
      tape(TapeOp::Seek, int32_t(std::clamp<int64_t>(s.tape.position + step, 0, kTapeLength)));
      return;
    }
    case OpKey::Lift: case OpKey::Drop:
      if (I.inst_mode() && ui.page == Page::Seq) {  // t5: finger copies / pastes a slot
        I.post(cmd(Domain::Seq, uint16_t(SeqOp::Nav), k == OpKey::Lift ? 2 : 3, shift ? 1 : 0, uint8_t(I.side())));
        return;
      }
      if (k == OpKey::Lift) tape(TapeOp::Lift, 0, shift ? 1 : 0);
      else tape(TapeOp::Drop, 0, 0);
      return;
    case OpKey::Rewind: tape(TapeOp::Seek, 0); return;
    case OpKey::Split: tape(TapeOp::Split, 0, shift ? 1 : 0); return;  // guide 8.8: split, shift = join
    default: return;
  }
}

// Sound keys 1-8 (guide 5, 8.12). Synth/drum: the sound slot; this port keeps no per-slot sounds, so a slot
// loads factory preset n of that side (ASSUMED) through the worker when there is one; shift = the preset
// browser. Tape and mixer: the tape tricks.
void UiModel::sound_key(int n) {
  Impl& I = *impl_;
  UiState& ui = I.ui;
  const CoreSnapshot& s = I.core.snapshot();
  auto tape = [&](TapeOp op, int32_t v) { I.post(cmd(Domain::Tape, uint16_t(op), 0, v)); };
  if (ui.mode == Mode::Tape || ui.mode == Mode::Mixer) {
    const int t = s.tape.track & 3;
    switch (n) {
      case 0: tape(TapeOp::SetLoopIn, s.tape.position); return;                                // loop in
      case 1:                                                                                  // loop out
        tape(TapeOp::SetLoopOut, s.tape.position);
        if (s.tape.loop_in < s.tape.position) tape(TapeOp::SetLoop, 1);
        return;
      case 2:                                                                                  // loop toggle
        if (ui.shift && s.tape.display[6 + 2 * t] >= 0) {  // shift: loop the take under the head
          tape(TapeOp::SetLoopIn, s.tape.display[6 + 2 * t]);
          tape(TapeOp::SetLoopOut, s.tape.display[7 + 2 * t]);
          tape(TapeOp::SetLoop, 1);
        } else {
          tape(TapeOp::SetLoop, s.tape.loop ? 0 : 1);
        }
        return;
      case 3:                                                                                  // break
        if (s.tape.playing) { tape(TapeOp::Stop, 0); I.break_resume = true; }
        return;
      case 4: tape(TapeOp::SetReverse, s.tape.reverse ? 0 : 1); return;                       // reverse
      default: return;  // 6 chop, 7/8 memo: not in the port yet
    }
  }
  if (!I.inst_mode()) return;
  const int side = I.side();
  if (ui.shift) {  // guide 14.2: shift + sound key = the presets, for that slot
    // the browser opens on the preset the slot holds (t6 review: the lists come from op1_factory.db)
    ui.pb_slot = n;
    ui.pb_folders = I.pb_folders[side];
    ui.pb_names.clear();
    ui.pb_folder = ui.pb_preset = 0;
    // preselect what that slot plays: its browser pick, else (the slot sounding now) the preset last loaded,
    // else its default preset; the boot sound (no preset yet) selects the folder of the engine that plays
    const InstrumentType playing = side ? I.core.snapshot().drum.type : I.core.snapshot().synth.type;
    int target = I.slot_preset[side][n];
    if (target < 0 && ui.sound == n + 1) target = I.cur_preset[side];
    if (target < 0 && !(ui.sound == n + 1 && I.cur_preset[side] < 0)) target = n;
    bool found = false;
    for (size_t f = 0; f < I.pb_index[side].size(); ++f) {
      ui.pb_names.emplace_back();
      for (size_t k = 0; k < I.pb_index[side][f].size(); ++k) {
        const state::FactoryEntry* e = I.preset_entry(side, I.pb_index[side][f][k]);
        ui.pb_names.back().push_back(e ? e->name : std::string());
        if (!found && target >= 0 && I.pb_index[side][f][k] == target) { ui.pb_folder = int(f); ui.pb_preset = int(k); found = true; }
        InstrumentType t;
        if (!found && target < 0 && k == 0 && e && state::instrument_type_from_name(e->type, t) && t == playing) {
          ui.pb_folder = int(f);
          found = true;
        }
      }
    }
    I.prev_preset = I.cur_preset[side];
    I.prev_type = int(playing);
    I.pb_previewing = I.pb_dirty = false;
    set_mode(Mode::Browser);
    return;
  }
  ui.sound = n + 1;
  I.load_preset(side, I.slot_index(side, n));
}

// Encoder push (tap). Firmware evidence for a push exists only for the FM radio pop-up (fmpopup: push the blue
// knob), which the port does not have. The device guides say a tap "usually means confirm, return to default"
// (OP-1 field guide, encoder commands); the Field app binds "tap = reset that parameter". So: a push puts what
// that encoder turns on this page back to its default; where a page has a better use it is listed. KEYMAP.md.
void UiModel::push(int i) {
  Impl& I = *impl_;
  UiState& ui = I.ui;
  if (ui.power_off) return;
  i &= 3;
  I.browse = -1;
  ui.last_enc = i;
  ui.push_t[i] = ui.t;
  ++ui.pushes[i];
  const CoreSnapshot& s = I.core.snapshot();
  I.capture_defaults(s);
  const bool shift = ui.shift;
  const int p = i + (shift ? 4 : 0);
  if (I.inst_mode()) {
    const int side = I.side();
    const Domain d = I.inst_domain();
    switch (ui.page) {
      case Page::Main: I.post(cmd(d, uint16_t(InstrumentOp::SetKnob), p, I.def_knobs[side][p])); return;
      case Page::Envelope: I.post(cmd(d, uint16_t(InstrumentOp::SetAdsr), p, I.def_adsr[side][p])); return;
      case Page::PlayMode:
        if (i < 2) I.post(cmd(d, uint16_t(InstrumentOp::SetAdsr), 4 + i, I.def_adsr[side][4 + i]));
        return;
      case Page::Fx: I.post(cmd(d, uint16_t(InstrumentOp::SetFxParam), p, I.def_fx[side][p])); return;
      case Page::Lfo: I.post(cmd(d, uint16_t(InstrumentOp::SetLfoParam), p, I.def_lfo[side][p])); return;
      case Page::Seq: return;  // t5: the firmware's sequencer controllers read turns only (seq_engines.h)
      default: return;
    }
  }
  auto mixer = [&](MixerOp op, int idx, int v, uint8_t slot = 0) { I.post(cmd(Domain::Mixer, uint16_t(op), idx, v, slot)); };
  switch (ui.mode) {
    case Mode::Tape:
      if (i == 0) I.post(cmd(Domain::Tape, uint16_t(TapeOp::Seek), 0, 0));                 // blue: tape start
      else if (i == 1) I.post(cmd(Domain::Tape, uint16_t(TapeOp::SetLoop), 0, 0));        // green: loop off
      else if (i == 2) {                                                                  // white: normal speed, forward
        I.post(cmd(Domain::Tape, uint16_t(TapeOp::SetSpeed), 0, 1000));
        I.post(cmd(Domain::Tape, uint16_t(TapeOp::SetReverse), 0, 0));
      } else {                                                                            // orange: full track level
        const int t = s.tape.track & 3;
        mixer(MixerOp::SetVolume, t, 32767, uint8_t(t));
      }
      return;
    case Mode::Mixer:
      switch (ui.page) {
        case Page::Eq: if (i < 3) mixer(MixerOp::SetEq, i, 16383); return;                // op1.db master defaults
        case Page::Master: {
          static const int kMasterDefault[4] = {32767, 32767, 8192, 10240};
          mixer(MixerOp::SetMaster, i, kMasterDefault[i]);
          return;
        }
        case Page::Fx:
          I.master_fx[p] = 4000;
          ui.master_fx[p] = 4000;
          mixer(MixerOp::SetMasterFxParam, p, 4000);
          return;
        default:
          if (shift) mixer(MixerOp::SetPan, i, 0, uint8_t(i));
          else mixer(MixerOp::SetVolume, i, 32767, uint8_t(i));
          return;
      }
    case Mode::Tempo:
      if (i == 0) I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 0, 1200));
      else if (i == 1) { ui.sync = 0; I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetSync), 0, 0)); }
      else if (i == 2) { ui.sync_out = false; I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetSyncOut), 0, 0)); }
      else {  // "tap orange: metronome"
        I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetMetronome), 0, s.tempo.metronome ? 0 : 1));
        I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetMetronome), 0, s.tempo.metronome ? 0 : ui.metronome_level));
      }
      return;
    case Mode::Tune:
      if (i < 2) I.post(cmd(Domain::Synth, uint16_t(InstrumentOp::SetAdsr), 4 + i, I.def_adsr[0][4 + i]));
      return;
    case Mode::Communication: ui.com_page = 0; return;
    case Mode::Browser: {  // "tap the blue encoder to confirm your choice" (field guide): load it, back to the sound
      // t6 review (Sir): the push puts the preset under the cursors (its engine included) on the slot the
      // browser was opened for; the slot keeps it, so its sound key brings it back
      const int side = I.pb_side();
      if (const int n = I.pb_cursor_index(); n >= 0) {
        I.slot_preset[side][ui.pb_slot & 7] = n;
        ui.sound = (ui.pb_slot & 7) + 1;
        // already sounding as the preview: keep it (a reload would reset knobs turned while previewing)
        if (!(I.pb_previewing && !I.pb_dirty && I.cur_preset[side] == n)) I.load_preset(side, n);
      }
      I.pb_previewing = I.pb_dirty = false;  // kept: leaving now restores nothing
      ui.shift = false;  // the push may come with shift still held from opening the browser
      set_mode(ui.inst_mode);
      return;
    }
    default: return;
  }
}

void UiModel::volume(int dir) {
  UiState& ui = impl_->ui;
  if (ui.power_off || dir == 0) return;
  ui.volume = std::clamp(ui.volume + (dir > 0 ? 2048 : -2048), 0, 32767);
  if (dir > 0 && ui.volume > 32767 - 2048) ui.volume = 32767;
  ui.volume_t = ui.t;
  impl_->post(cmd(Domain::Core, uint16_t(CoreOp::SetOutputVolume), 0, ui.volume));
}

void UiModel::power() {
  Impl& I = *impl_;
  UiState& ui = I.ui;
  if (!ui.power_off) {  // off: notes off, tape stopped, screen black, keys ignored until the switch again
    release_all();
    I.post(cmd(Domain::Core, uint16_t(CoreOp::AllNotesOff), 0, 0));
    I.post(cmd(Domain::Tape, uint16_t(TapeOp::Stop), 0, 0));
    ui.shift = false;
    ui.power_off = true;
  } else {
    ui.power_off = false;
  }
}

void UiModel::set_worker(Worker* w) { impl_->worker = w; }

void UiModel::encoder(int i, int det) {
  Impl& I = *impl_;
  UiState& ui = I.ui;
  if (det == 0 || I.ui.power_off) return;
  i &= 3;
  ui.last_enc = i;
  ui.enc_t[i] = ui.t;
  ui.enc_acc[i] += det;
  const CoreSnapshot& s = I.core.snapshot();
  const bool shift = ui.shift;
  auto q = [&](int cur, int per = 328) { return std::clamp(cur + det * per, 0, 32767); };
  auto qs = [&](int cur, int per = 328) { return std::clamp(cur + det * per, -32768, 32767); };  // bipolar
  const int p = i + (shift ? 4 : 0);  // parameter slot: shift = the second page (§4)
  if (ui.mode == Mode::Browser && !ui.pb_folders.empty()) {  // t6 review: blue = folder, green = preset
    const int nf = int(ui.pb_folders.size());
    if (i == 0) {
      ui.pb_folder = std::clamp(ui.pb_folder + det, 0, nf - 1);
      ui.pb_preset = 0;
    } else if (i == 1) {
      const int np = int(ui.pb_names[size_t(std::clamp(ui.pb_folder, 0, nf - 1))].size());
      ui.pb_preset = std::clamp(ui.pb_preset + det, 0, std::max(0, np - 1));
    }
    I.pb_dirty = true;  // render() loads the preview once the cursors rest
    I.pb_turn_t = ui.t;
    return;
  }
  if (I.inst_mode()) {
    const InstrumentView& iv = I.side() ? s.drum : s.synth;
    const Domain d = I.inst_domain();
    switch (ui.page) {
      case Page::Envelope:
        if (is_drum(iv.type)) {  // t5: the drum envelope page edits dyna_env (DrumController page 1, F)
          // knobs 0..4 only; ranges and steps per detent from the firmware's range table (decode-drums.md Q3)
          static const int lo[5] = {-32767, 0, -32767, 0, 0}, hi[5] = {32767, 8192, 32767, 32767, 32767};
          static const int step[5] = {1024, 256, 1024, 256, 1024};
          if (p > 4) return;
          I.post(cmd(d, uint16_t(InstrumentOp::SetAdsr), p, std::clamp(iv.adsr[p] + det * step[p], lo[p], hi[p])));
          return;
        }
        // A, D and R use 0..16384 (adsr_rates; factory data 64..16320), S 0..32767: the same 100 detents walk
        // either range, so no part of the travel is dead (t2, 2026-10-23; before, A/D/R did nothing above half).
        if (p == 0 || p == 1 || p == 3)
          I.post(cmd(d, uint16_t(InstrumentOp::SetAdsr), p, std::clamp(iv.adsr[p] + det * 164, 0, 16384)));
        else
          I.post(cmd(d, uint16_t(InstrumentOp::SetAdsr), p, q(iv.adsr[p])));
        return;
      case Page::PlayMode:
        if (i == 0) {  // play mode: 4 steps of 4096 (synth_instrument.h play_mode_of)
          // step from what was last sent if the snapshot has not caught up yet (fast detents)
          const int base = ui.t - I.pm_t < 0.2 ? I.pm_sent : (iv.adsr[4] >> 12);
          const int m = std::clamp(base + (det > 0 ? 1 : -1), 0, 3);
          I.pm_sent = m;
          I.pm_t = ui.t;
          I.post(cmd(d, uint16_t(InstrumentOp::SetAdsr), 4, m * 4096 + 2048));
        } else if (i == 1) {
          I.post(cmd(d, uint16_t(InstrumentOp::SetAdsr), 5, std::clamp(iv.adsr[5] + det * 256, 0, 16384)));
        } else {  // white = G-BEND (slot 6), orange = DETUNE (slot 7) (t2: synth_instrument.h)
          I.post(cmd(d, uint16_t(InstrumentOp::SetAdsr), 4 + i, q(iv.adsr[4 + i])));
        }
        return;
      case Page::Fx:
        I.post(cmd(d, uint16_t(InstrumentOp::SetFxParam), p, fx_slot_signed(iv.fx_type, p) ? qs(iv.fx[p]) : q(iv.fx[p])));
        return;
      case Page::Lfo: {
        // signed depths (lfo_tremolo.cpp p[1], p[2]; lfo_value.cpp and lfo_element.cpp p[1]: s15 values)
        const LfoType lt = iv.lfo_type;
        const bool bip = (p == 1 && (lt == LfoType::Tremolo || lt == LfoType::Value || lt == LfoType::Element)) ||
                         (p == 2 && lt == LfoType::Tremolo);
        I.post(cmd(d, uint16_t(InstrumentOp::SetLfoParam), p, bip ? qs(iv.lfo[p]) : q(iv.lfo[p])));
        return;
      }
      case Page::Seq:
        // t5: the turn goes to the sequencer as the firmware's controllers read it (index 0..7, 4..7 = shift):
        // each sequencer maps it (cursor, swing, hold on/off by direction, crank, ...; seq_engines.h)
        I.post(cmd(Domain::Seq, uint16_t(SeqOp::Encoder), p, det, uint8_t(I.side())));
        I.seq_t[I.side()] = ui.t;
        return;
      default: {
        if (iv.type == InstrumentType::Drum) {  // t5: edit the last played key (DrumController page 0, F)
          drum_key_edit(I, iv, p, det);
          return;
        }
        if (iv.type == InstrumentType::Dbox) {  // t5: the same for dbox's 8 words
          static const int lo[8] = {-24320, 0, 0, 0, 0, 0, 0, 0};
          static const int hi[8] = {24832, 32767, 32767, 32767, 32767, 32767, 32767, 32767};
          static const int step[8] = {194, 328, 328, 328, 154, 328, 328, 256};
          const int key = iv.display[50];
          const int v = std::clamp(iv.display[42 + p] + det * step[p], lo[p], hi[p]);
          I.post(cmd(Domain::Drum, uint16_t(InstrumentOp::SetDrumKey), key, v, uint8_t(8 + p)));
          return;
        }
        // bipolar knobs: dna filter (eng_dna.cpp k[0]: below 0 low-pass, above high-pass, default -16000)
        // drum and dbox: every knob is a signed kit-wide offset, 0 = none (drum.cpp, dbox.cpp)
        // digital k2 and pulse k3 are signed in the factory data (-32768..32767, -9288..6141) and the DSP uses the
        // negative half (eng_digital.cpp, eng_pulse.cpp): t2, 2026-10-23
        const bool bip = (iv.type == InstrumentType::Dna && p == 0) || (iv.type == InstrumentType::Digital && p == 2) ||
                         (iv.type == InstrumentType::Pulse && p == 3) || iv.type == InstrumentType::Drum ||
                         iv.type == InstrumentType::Dbox;
        I.post(cmd(d, uint16_t(InstrumentOp::SetKnob), p, bip ? qs(iv.knobs[p]) : q(iv.knobs[p])));
        return;
      }
    }
  }
  switch (ui.mode) {
    case Mode::Tape: {
      if (i == 0) {  // blue: move the tape (jog), shift = coarse
        const int64_t step = int64_t(kSampleRate) / (shift ? 1 : 10);
        I.post(cmd(Domain::Tape, uint16_t(TapeOp::Seek), 0, int32_t(std::clamp<int64_t>(s.tape.position + det * step, 0, kTapeLength))));
      } else if (i == 1) {  // green: loop in (shift: loop out), in sixteenths
        const int64_t six = int64_t(kSampleRate) * 150 / std::max<int>(400, s.tempo.bpm_tenths);
        const int32_t cur = shift ? s.tape.loop_out : s.tape.loop_in;
        const int32_t base = cur > 0 ? cur : s.tape.position;
        I.post(cmd(Domain::Tape, uint16_t(shift ? TapeOp::SetLoopOut : TapeOp::SetLoopIn), 0,
                   int32_t(std::clamp<int64_t>(base + det * six, 0, kTapeLength))));
        I.post(cmd(Domain::Tape, uint16_t(TapeOp::SetLoop), 0, 1));
      } else if (i == 2) {  // white: tape speed (permille, tape_engine.h), shift = direction
        if (shift) I.post(cmd(Domain::Tape, uint16_t(TapeOp::SetReverse), 0, det > 0 ? 0 : 1));
        else I.post(cmd(Domain::Tape, uint16_t(TapeOp::SetSpeed), 0, std::clamp(int(s.tape.speed) + det * 20, 0, 2000)));
      } else {  // orange: level of the selected track
        const int t = s.tape.track & 3;
        I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetVolume), t, q(s.mixer.volume[t]), uint8_t(t)));
      }
      return;
    }
    case Mode::Mixer:
      switch (ui.page) {
        case Page::Eq:
          if (i < 3) I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetEq), i, q(s.mixer.eq[i])));
          return;
        case Page::Fx: {
          // t5: from the engine's value (MixerView, a type switch loads that type's defaults), signed where the
          // effect reads the slot signed (fx_slot_signed)
          const int cur = s.mixer.master_fx_params[p];
          I.master_fx[p] = fx_slot_signed(s.mixer.master_fx, p) ? qs(cur) : q(cur);
          ui.master_fx[p] = I.master_fx[p];
          I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetMasterFxParam), p, I.master_fx[p]));
          return;
        }
        case Page::Master: I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetMaster), i, q(s.mixer.master[i]))); return;
        default:
          if (shift) I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetPan), i, std::clamp(s.mixer.pan[i] + det * 655, -32768, 32767), uint8_t(i)));
          else I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetVolume), i, q(s.mixer.volume[i]), uint8_t(i)));
          return;
      }
    case Mode::Tempo:
      if (i == 0) {
        I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetBpmTenths), 0,
                   std::clamp(int(s.tempo.bpm_tenths) + det * (shift ? 1 : 10), kTempoMin * 10, kTempoMax * 10)));
      } else if (i == 1) {
        ui.sync = std::clamp(ui.sync + (det > 0 ? 1 : -1), 0, 1);
        I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetSync), 0, ui.sync));
      } else if (i == 2) {
        ui.sync_out = det > 0;
        I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetSyncOut), 0, ui.sync_out ? 1 : 0));
      } else {
        // t5: the firmware's level knob is 0..64, one per detent, sent x 511 (tempo controller 0x01A5EDDC)
        ui.metronome_level = std::clamp(ui.metronome_level / 511 + det, 0, 64) * 511;
        I.post(cmd(Domain::Tempo, uint16_t(TempoOp::SetMetronome), 0, ui.metronome_level > 0 ? 1 : 0));
        I.post(cmd(Domain::Mixer, uint16_t(MixerOp::SetMetronome), 0, ui.metronome_level));
      }
      return;
    case Mode::Communication: ui.com_page = std::clamp(ui.com_page + (det > 0 ? 1 : -1), 0, 3); return;
    case Mode::Tune: {  // the global tune page: blue play mode, green portamento (synth adsr[4], adsr[5])
      const InstrumentView& iv = s.synth;
      if (i == 0) {
        const int base = ui.t - I.pm_t < 0.2 ? I.pm_sent : (iv.adsr[4] >> 12);
        const int m = std::clamp(base + (det > 0 ? 1 : -1), 0, 3);
        I.pm_sent = m;
        I.pm_t = ui.t;
        I.post(cmd(Domain::Synth, uint16_t(InstrumentOp::SetAdsr), 4, m * 4096 + 2048));
      } else if (i == 1) {
        I.post(cmd(Domain::Synth, uint16_t(InstrumentOp::SetAdsr), 5, std::clamp(iv.adsr[5] + det * 256, 0, 16384)));
      }
      return;
    }
    default: return;
  }
}

void UiModel::note(int semi, bool down) {
  Impl& I = *impl_;
  UiState& ui = I.ui;
  if (semi < 0 || semi >= 24) return;
  if (down) {
    if (I.sounding[semi] >= 0 || ui.power_off) return;
    const int n = std::clamp(53 + 12 * ui.octave + semi, 0, 127);  // the OP-1 keyboard: F3..E5 at octave 0
    if (ui.shift && ui.page == Page::Seq && I.inst_mode()) {  // t5: shift + key enters a step (seq_engines.h)
      I.entry[semi] = true;
      I.post(cmd(Domain::Seq, uint16_t(SeqOp::KeyEdit), semi, 1, uint8_t(I.side())));
      return;
    }
    // record + note (held or armed, tape stopped): the tape starts recording with this note (t6 review, Sir)
    if (I.armed(I.core.snapshot()) && !I.core.snapshot().tape.playing)
      I.post(cmd(Domain::Tape, uint16_t(TapeOp::Play), 0, 0));
    I.sounding[semi] = n;
    ui.held[n] = true;
    ui.last_note = n;
    ui.key_t = ui.t;
    Event e = note_on(uint8_t(n), 100, Source::Keyboard, now_ns());
    e.value = int16_t(semi + 1);  // t5: the keyboard key (finger slots are the white keys, seq_engines.h)
    I.core.post_ui(e);
  } else if (I.entry[semi]) {
    I.entry[semi] = false;
    I.post(cmd(Domain::Seq, uint16_t(SeqOp::KeyEdit), semi, 0, uint8_t(I.side())));
  } else if (I.sounding[semi] >= 0) {
    ui.held[I.sounding[semi]] = false;
    Event e = note_off(uint8_t(I.sounding[semi]), Source::Keyboard, now_ns());
    e.value = int16_t(semi + 1);
    I.core.post_ui(e);
    I.sounding[semi] = -1;
  }
}

void UiModel::octave(int dir) {
  UiState& ui = impl_->ui;
  ui.octave = std::clamp(ui.octave + dir, -3, 3);
  ui.octave_t = ui.t;
}

void UiModel::engine_step(int dir) {
  Impl& I = *impl_;
  const CoreSnapshot& s = I.core.snapshot();
  if (I.side()) {
    const InstrumentType t = s.drum.type == InstrumentType::Drum ? InstrumentType::Dbox : InstrumentType::Drum;
    I.post(cmd(Domain::Drum, uint16_t(InstrumentOp::SetType), 0, int32_t(t)));
    return;
  }
  // the engines the synth factory builds, without the drum ids (8, 9) and ls (10: the factory returns
  // nothing for it, so stepping onto it would stick); iter (14) is missing from the firmware's menu yet
  // fully implemented (firmware-behaviour.md §11)
  static const int ids[] = {0, 1, 2, 3, 4, 5, 6, 7, 11, 12, 13, 14};
  const int n = int(sizeof ids / sizeof ids[0]);
  // step from the engine last asked for if the snapshot has not caught up yet (fast key presses)
  const int cur = I.ui.t - I.eng_t < 0.2 ? I.eng_sent : int(s.synth.type);
  int at = 0;
  for (int k = 0; k < n; ++k)
    if (ids[k] == cur) at = k;
  I.eng_sent = ids[(at + dir + n) % n];
  I.eng_t = I.ui.t;
  I.post(cmd(Domain::Synth, uint16_t(InstrumentOp::SetType), 0, I.eng_sent));
}

void UiModel::browse(int dir) {
  Impl& I = *impl_;
  const int n = int(I.names.size());
  if (!n) return;
  if (dir == 0) { I.browse = -1; return; }
  if (I.browse < 0) {
    const auto it = std::find(I.names.begin(), I.names.end(), I.shown);
    I.browse = it == I.names.end() ? 0 : int(it - I.names.begin());
  } else {
    I.browse = (I.browse + (dir > 0 ? 1 : n - 1)) % n;
  }
}

bool UiModel::browse_to(const std::string& name) {
  Impl& I = *impl_;
  const auto it = std::find(I.names.begin(), I.names.end(), name);
  if (it == I.names.end()) return false;
  I.browse = int(it - I.names.begin());
  return true;
}

void UiModel::release_all() {
  for (int i = 0; i < 24; ++i) note(i, false);
}

const std::string& UiModel::render(svg::Image& dst, double t) {
  Impl& I = *impl_;
  UiState& ui = I.ui;
  ui.t = t;
  const CoreSnapshot& s = I.core.snapshot();
  // t5: the sequencer screens read UiState::seq_param; it follows the engines (SeqView::params) except right
  // after an encoder turn the snapshot has not caught up with
  for (int side = 0; side < 2; ++side)
    for (int k = 0; k < 16; ++k) ui.seq_param[side][k] = s.seq.params[side][k];
  // preset browser preview: 120 ms after the last turn, so fast scrolling does not queue a load per detent
  if (ui.mode == Mode::Browser && I.pb_dirty && ui.t - I.pb_turn_t >= 0.12) {
    I.pb_dirty = false;
    const int n = I.pb_cursor_index();
    if (n >= 0 && n != I.cur_preset[I.pb_side()]) {
      I.load_preset(I.pb_side(), n);
      I.pb_previewing = true;
    }
  }
  if (!I.env_screen_done) {  // OP1_OG_SCREEN / run.sh --screen: start on a named screen
    I.env_screen_done = true;
    if (const char* env = std::getenv("OP1_OG_SCREEN"); env && *env) {
      const auto it = std::find(I.names.begin(), I.names.end(), std::string(env));
      if (it != I.names.end()) I.browse = int(it - I.names.begin());
    }
  }
  I.capture_defaults(s);
  if (ui.power_off) {  // the power switch is off: a black display
    std::fill(dst.px.begin(), dst.px.end(), 0xFF000000u);
    I.shown = "off";
    return I.shown;
  }
  std::string want;
  if (I.browse >= 0 && I.browse < int(I.names.size())) want = I.names[size_t(I.browse)];
  else if (const char* w = I.want(s)) want = w;
  I.shown = want;
  if (!I.assets.available()) { draw_fallback(dst, s); return I.shown; }
  if (want.empty()) { draw_missing(dst, mode_name(ui.mode)); return I.shown; }
  svg::Document* doc = I.doc(want);
  if (!doc) { draw_missing(dst, want + " (UNREADABLE)"); return I.shown; }

  const ScreenBinding* b = find_binding(want);
  BindCtx ctx{*doc, I.font, s, ui, t, want};
  doc->reset_overrides();
  if (b && b->bind) b->bind(ctx);
  uint64_t key = 0xcbf29ce484222325ull;
  hash_overrides(doc->root(), key);
  if (key != I.raster_key || doc != I.raster_doc) {
    doc->draw(I.raster);
    I.raster_key = key;
    I.raster_doc = doc;
  }
  dst.px = I.raster.px;
  if (b && b->draw) b->draw(ctx, dst);
  return I.shown;
}

}  // namespace op1
