// Contract of the UI track (t6): the window, screens, input mapping and overlays (src/gui/), and the
// asset provider (src/assets/) that reads the original screens, fonts and factory database from the
// user's own firmware image at run time. That is how the port looks 1:1 like the OG without any
// Teenage Engineering asset in the source tree or the shared folder (architecture.md, "Assets").
// Owner: t3 (interface); t6 implements.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace op1 {

class Core;
class Worker;

class AssetProvider {
 public:
  virtual ~AssetProvider() = default;
  // The user's OG firmware image (e.g. op1_246.op1), read-only. false + err if missing or unreadable;
  // the UI then falls back to drawn placeholders and says so on screen.
  virtual bool open(const std::string& firmware_image, std::string& err) = 0;
  virtual bool available() const = 0;
  virtual std::string version() const = 0;                              // e.g. "R.00246"
  virtual std::vector<std::string> list(const std::string& kind) const = 0;  // "svg", "font", "db", ...
  virtual bool read(const std::string& name, std::vector<uint8_t>& out) const = 0;
};
std::unique_ptr<AssetProvider> create_asset_provider();

struct UiOptions {
  bool keyboard_overlay = false;  // --keyboard: on-screen keyboard and key-mapping overlay (off by default)
  int scale = 0;                  // 0 = fit the window, else integer scale of the 320x160 display
  double duration_s = 0;          // > 0: close after this long (tests, measurements)
  std::string screenshot;         // write the window once (BMP) after ~1.5 s, for verification
  std::string script;             // t9/t10: scripted input (src/gui/ui_input.h), "@FILE" or inline steps
};

// Runs on the main thread until the window closes or duration_s passes. Returns 0 on a clean exit,
// 2 if no display could be opened (the caller may continue headless).
int run_ui(Core& core, AssetProvider& assets, Worker* worker, const UiOptions& opt);
// t9/t10: the same UI without a window (no SDL video): plays opt.script, saves its screenshots. Returns when
// the script ends or duration_s passes.
// worker: as in run_ui, so sound keys and the preset browser load presets in scripted runs too (t6 review).
int run_ui_headless(Core& core, AssetProvider& assets, const UiOptions& opt, Worker* worker = nullptr);

}  // namespace op1
