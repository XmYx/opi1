// The worker (t2, 2026-10-23; was a placeholder): the non-realtime thread for everything slow. It reads the
// factory presets and their audio from the user's firmware image (factory.h), builds patches and samples and
// hands them to the core, and frees what the audio thread hands back through Core::retire(), so memory is
// never freed on the audio thread.
//
// At start, as the device does, the instruments get sound to play (PARITY-AUDIT.md §1.1: before this, drum
// mode and the sampler were silent on any audio device):
//   - the drum slot loads the first factory drum kit (op1_factory.db drum_presets order: "neoprene" on R.00246),
//     its patch and its kit audio (content/audio/factory_drum/<name>.raw);
//   - the synth slot receives the first factory sampler preset's sample and root frequency
//     (content/audio/factory_synth/<name>.raw, base_freq), so the sampler engine plays as soon as it is picked.
// request(WorkerOp::LoadFactoryPreset) loads any factory preset (domain Synth or Drum, index into that table).
// A retired SampleData is freed only 1 s after the core hands it back: snapshots carry a pointer to the
// sampler's sample (InstrumentView::sample) and the UI may still be reading an older one.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "factory.h"
#include "seq_state.h"
#include "op1/core.h"
#include "op1/dsp.h"
#include "op1/io.h"
#include "op1/patch.h"

namespace op1 {
namespace {

using state::FactoryEntry;
using state::FactoryLibrary;

class FactoryWorker final : public Worker {
 public:
  ~FactoryWorker() override { stop(); }

  bool start(Core& core, AssetProvider* assets, std::string& err) override {
    core_ = &core;
    assets_ = assets;
    bool ok = true;
    if (assets && assets->available()) {
      if (lib_.open(*assets, err)) {
        have_lib_ = true;
        const int kit = lib_.first_of(true, "drum");
        if (kit >= 0) queue({Domain::Drum, kit, false});
        const int smp = lib_.first_of(false, "sampler");
        if (smp >= 0) queue({Domain::Synth, smp, true});
      } else {
        ok = false;
      }
      // t5: the sequencer states stored in op1.db (finger slots, pattern steps, endless steps, tombola physics,
      // the selected types), as the device restores them at boot (seq_state.h)
      std::string serr;
      state::sequencer_state_commands(*assets, seq_boot_, serr);
      if (!serr.empty()) std::fprintf(stderr, "op1-og: worker: sequencer state: %s\n", serr.c_str());
    } else {
      err = "no firmware image: drum kits and sampler samples are not available (the drum and sampler stay silent)";
      ok = false;
    }
    run_ = true;
    thread_ = std::thread([this] { loop(); });
    return ok;
  }

  void stop() override {
    {
      std::lock_guard<std::mutex> l(m_);
      run_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  bool request(const Command& c) override {
    if (WorkerOp(c.op) != WorkerOp::LoadFactoryPreset || !have_lib_) return false;
    if (c.domain != Domain::Synth && c.domain != Domain::Drum) return false;
    const auto& v = c.domain == Domain::Drum ? lib_.drums() : lib_.synths();
    if (c.index < 0 || size_t(c.index) >= v.size()) return false;
    queue({c.domain, c.index, false});
    return true;
  }

 private:
  struct Job {
    Domain domain = Domain::Synth;
    int index = 0;
    bool sample_only = false;  // hand over the preset's sample without loading the patch
  };

  void queue(const Job& j) {
    {
      std::lock_guard<std::mutex> l(m_);
      jobs_.push_back(j);
    }
    cv_.notify_all();
  }

  void loop() {
    for (const Command& c : seq_boot_) post(c);
    seq_boot_.clear();
    for (;;) {
      Job j;
      bool have = false, quit = false;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait_for(l, std::chrono::milliseconds(10), [&] { return !run_ || !jobs_.empty(); });
        if (!jobs_.empty()) {
          j = jobs_.front();
          jobs_.pop_front();
          have = true;
        } else if (!run_) {
          quit = true;
        }
      }
      if (quit) break;
      if (have) load(j);
      drain();
    }
    drain(true);  // stopping: the UI is gone (op1-og stops the worker after the window closes)
  }

  // Posts to the core; the queue is large, but retry briefly if the audio thread is behind.
  bool post(const Command& c) {
    for (int k = 0; k < 200; ++k) {
      if (core_->post_worker(c)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
  void post_sample(Domain d, SampleData* s) {
    if (!s) return;
    Command c;
    c.domain = d;
    c.op = uint16_t(InstrumentOp::SetSample);
    c.ptr = s;
    if (!post(c)) free_sample(s);
  }

  void load(const Job& j) {
    std::string err;
    if (j.domain == Domain::Drum) {
      const FactoryEntry& e = lib_.drums()[size_t(j.index)];
      auto* p = new DrumPatch;
      if (!FactoryLibrary::parse_drum(e.json, *p, err)) {
        std::fprintf(stderr, "op1-og: worker: drum preset %s: %s\n", e.name.c_str(), err.c_str());
        delete p;
        return;
      }
      const bool kit = p->type == InstrumentType::Drum;
      Command c;
      c.domain = Domain::Drum;
      c.op = uint16_t(InstrumentOp::LoadDrumPatch);
      c.ptr = p;
      if (!post(c)) { delete p; return; }
      if (kit) post_sample(Domain::Drum, FactoryLibrary::load_raw(*assets_, "factory_drum/" + e.name + ".raw", 0));
      return;
    }
    const FactoryEntry& e = lib_.synths()[size_t(j.index)];
    SynthPatch sp;
    if (!FactoryLibrary::parse_synth(e.json, sp, err)) {
      std::fprintf(stderr, "op1-og: worker: synth preset %s: %s\n", e.name.c_str(), err.c_str());
      return;
    }
    if (!j.sample_only) {
      auto* p = new SynthPatch(sp);
      Command c;
      c.domain = Domain::Synth;
      c.op = uint16_t(InstrumentOp::LoadSynthPatch);
      c.ptr = p;
      if (!post(c)) { delete p; return; }
    }
    if (sp.type == InstrumentType::Sampler)
      post_sample(Domain::Synth, FactoryLibrary::load_raw(*assets_, "factory_synth/" + e.name + ".raw", sp.base_freq));
  }

  static void free_sample(SampleData* s) {
    if (s) delete[] s->frames;
    delete s;
  }

  void drain(bool final = false) {
    Command c;
    while (core_->pop_retired(c)) {
      switch (InstrumentOp(c.op)) {
        case InstrumentOp::LoadSynthPatch: delete static_cast<SynthPatch*>(c.ptr); break;
        case InstrumentOp::LoadDrumPatch: delete static_cast<DrumPatch*>(c.ptr); break;
        case InstrumentOp::SetSample:
          // the UI may still be drawing a snapshot that points at it (InstrumentView::sample): keep it 1 s
          if (c.ptr) grace_.push_back({std::chrono::steady_clock::now(), static_cast<SampleData*>(c.ptr)});
          break;
        default: break;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    while (!grace_.empty() && (final || now - grace_.front().first >= std::chrono::seconds(1))) {
      free_sample(grace_.front().second);
      grace_.pop_front();
    }
  }

  Core* core_ = nullptr;
  AssetProvider* assets_ = nullptr;
  FactoryLibrary lib_;
  bool have_lib_ = false;
  std::vector<Command> seq_boot_;  // t5: op1.db sequencer states, posted once when the thread starts
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::deque<std::pair<std::chrono::steady_clock::time_point, SampleData*>> grace_;  // retired samples
  bool run_ = false;
  std::thread thread_;
};

}  // namespace

std::unique_ptr<Worker> create_worker() { return std::make_unique<FactoryWorker>(); }

}  // namespace op1
