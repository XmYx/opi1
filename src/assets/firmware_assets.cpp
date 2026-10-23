// Asset provider (owner: t6). Opens the user's OG firmware image read-only, unpacks its container in
// memory and serves the screen SVGs and the factory/state databases to the UI and the worker.
// Nothing it reads is ever written to disk, into the source tree or into the shared folder.
//
// Container (firmware-behaviour.md §1): 4-byte little-endian CRC-32 of the rest, then an LZMA "alone"
// stream that unpacks to a tar. Names inside the tar are matched by suffix:
//   content/display/<name>.svg  -> kind "svg"   (read("<name>") or read("<name>.svg"))
//   content/<name>.db           -> kind "db"
//   content/audio/<dir>/<f>.raw -> kind "audio", name "audio/<dir>/<f>.raw" (factory sampler samples and drum
//                                  kits, 16-bit LE mono; read by the worker, t2 2026-10-23)
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <map>

#include "lzma_alone.h"
#include "op1/ui.h"

namespace op1 {
namespace {

constexpr size_t kMaxImage = size_t(256) << 20, kMaxTar = size_t(512) << 20;

class FirmwareAssets final : public AssetProvider {
 public:
  bool open(const std::string& path, std::string& err) override {
    if (path.empty()) { err = "no firmware image given (--firmware PATH or OP1_OG_FIRMWARE)"; return false; }
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { err = "not a file: " + path; return false; }
    if (size_t(st.st_size) > kMaxImage) { err = "image is unreasonably large: " + path; return false; }
    std::vector<uint8_t> raw(size_t(st.st_size));
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot read: " + path; return false; }
    const size_t got = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    if (got != raw.size() || raw.size() < 4 + 13) { err = "short read: " + path; return false; }
    uint32_t want = 0;
    std::memcpy(&want, raw.data(), 4);  // little-endian host
    if (crc32_ieee(raw.data() + 4, raw.size() - 4) != want) { err = "firmware CRC-32 mismatch: " + path; return false; }
    if (!lzma_alone_decode(raw.data() + 4, raw.size() - 4, kMaxTar, tar_, err)) return false;
    raw.clear();
    raw.shrink_to_fit();
    if (!index_tar(err)) return false;
    // The file name carries the release (op1_246.op1 -> R.00246); the image stores it only inside the
    // boot loaders, so the name is a label, not a check.
    const size_t slash = path.find_last_of('/');
    version_ = path.substr(slash == std::string::npos ? 0 : slash + 1);
    return available();
  }
  bool available() const override { return !svg_.empty(); }
  std::string version() const override { return version_; }

  std::vector<std::string> list(const std::string& kind) const override {
    std::vector<std::string> out;
    if (kind != "svg" && kind != "db" && kind != "audio") return out;
    for (const auto& kv : kind == "db" ? db_ : kind == "audio" ? audio_ : svg_) out.push_back(kv.first);
    return out;
  }
  bool read(const std::string& name, std::vector<uint8_t>& out) const override {
    std::string key = name;
    if (key.size() > 4 && key.compare(key.size() - 4, 4, ".svg") == 0) key.resize(key.size() - 4);
    auto it = svg_.find(key);
    if (it == svg_.end()) {
      it = db_.find(name);
      if (it == db_.end()) {
        it = audio_.find(name);
        if (it == audio_.end()) return false;
      }
    }
    out.assign(tar_.begin() + long(it->second.first), tar_.begin() + long(it->second.first + it->second.second));
    return true;
  }

 private:
  static size_t octal(const char* p, size_t n) {
    size_t v = 0;
    for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + size_t(p[i] - '0');
    return v;
  }
  static bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
  }
  bool index_tar(std::string& err) {
    size_t pos = 0;
    std::string long_name;
    while (pos + 512 <= tar_.size()) {
      const char* h = reinterpret_cast<const char*>(tar_.data() + pos);
      if (h[0] == 0) break;  // end of archive
      std::string name(h, strnlen(h, 100));
      if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345]) name = std::string(h + 345, strnlen(h + 345, 155)) + "/" + name;
      const size_t size = octal(h + 124, 12);
      const char type = h[156];
      const size_t body = pos + 512;
      if (body + size > tar_.size()) { err = "tar: entry runs past the archive"; return false; }
      if (type == 'L') {  // GNU long name for the next entry
        const char* b = reinterpret_cast<const char*>(tar_.data() + body);
        long_name.assign(b, strnlen(b, size));
      } else {
        if (!long_name.empty()) { name = long_name; long_name.clear(); }
        if (type == '0' || type == 0) {
          const size_t slash = name.find_last_of('/');
          const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
          if (name.find("display/") != std::string::npos && ends_with(base, ".svg"))
            svg_[base.substr(0, base.size() - 4)] = {body, size};
          else if (ends_with(base, ".db"))
            db_[base] = {body, size};
          else if (const size_t a = name.find("content/audio/"); a != std::string::npos && ends_with(base, ".raw"))
            audio_["audio/" + name.substr(a + 14)] = {body, size};
        }
      }
      pos = body + ((size + 511) & ~size_t(511));
    }
    if (svg_.empty()) { err = "no display/*.svg in the image (is this an OG OP-1 firmware?)"; return false; }
    return true;
  }

  std::vector<uint8_t> tar_;
  std::map<std::string, std::pair<size_t, size_t>> svg_, db_, audio_;  // name -> (offset, size) inside tar_
  std::string version_;
};

}  // namespace

std::unique_ptr<AssetProvider> create_asset_provider() { return std::make_unique<FirmwareAssets>(); }

}  // namespace op1
