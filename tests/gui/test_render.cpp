// Renders every screen SVG of the user's firmware image headlessly and checks that each one draws
// something (owner: t6). Needs the image: OP1_OG_FIRMWARE=/path/to/op1_246.op1 (skipped, with ok, when it
// is unset, so the tests stay runnable on machines without it).
//
// OP1_OG_SHEET_DIR=dir  additionally writes dir/screens-NN.bmp contact sheets (320x160 cells, 4 columns)
//                       and dir/<name>.bmp for each screen. These contain Teenage Engineering artwork
//                       rendered from the user's own image: keep them out of the repository.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "../../src/gui/svg.h"
#include "op1/ui.h"

namespace {

bool write_bmp(const std::string& path, const op1::svg::Image& im) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t row = uint32_t(im.w) * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * uint32_t(im.h);
  uint8_t h[54] = {'B', 'M'};
  auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) h[off + i] = uint8_t(v >> (8 * i)); };
  put32(2, size); put32(10, 54); put32(14, 40); put32(18, uint32_t(im.w)); put32(22, uint32_t(im.h));
  h[26] = 1; h[28] = 24; put32(34, size - 54);
  std::fwrite(h, 1, 54, f);
  std::vector<uint8_t> line(row + pad, 0);
  for (int y = im.h - 1; y >= 0; --y) {
    for (int x = 0; x < im.w; ++x) {
      const uint32_t c = im.px[size_t(y) * size_t(im.w) + size_t(x)];
      line[size_t(x) * 3] = uint8_t(c); line[size_t(x) * 3 + 1] = uint8_t(c >> 8); line[size_t(x) * 3 + 2] = uint8_t(c >> 16);
    }
    std::fwrite(line.data(), 1, line.size(), f);
  }
  std::fclose(f);
  return true;
}

}  // namespace

int main() {
  const char* fw = std::getenv("OP1_OG_FIRMWARE");
  if (!fw || !*fw) {
    std::printf("gui/test_render: skipped (OP1_OG_FIRMWARE not set)\n");
    return 0;
  }
  auto assets = op1::create_asset_provider();
  std::string err;
  CHECK(assets->open(fw, err));
  if (!err.empty()) std::fprintf(stderr, "assets: %s\n", err.c_str());
  const std::vector<std::string> names = assets->list("svg");
  CHECK_EQ(names.size(), 61u);

  const char* sheet = std::getenv("OP1_OG_SHEET_DIR");
  const int cols = 4, per_sheet = 16, cw = 320, ch = 160;
  std::vector<op1::svg::Image> sheets;
  int empty = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    std::vector<uint8_t> bytes;
    CHECK(assets->read(names[i], bytes));
    op1::svg::Document doc;
    std::string e;
    const bool ok = doc.load(std::string(bytes.begin(), bytes.end()), e);
    if (!ok) std::fprintf(stderr, "%s: %s\n", names[i].c_str(), e.c_str());
    CHECK(ok);
    op1::svg::Image im(cw, ch);
    doc.draw(im);
    size_t lit = 0;
    for (uint32_t p : im.px) lit += (p & 0xFFFFFF) != 0;
    // a screen that parsed but drew nothing is a renderer bug, except templates that are blank until
    // the firmware shows their hidden parts
    std::printf("%-16s shapes %5zu  lit pixels %6zu\n", names[i].c_str(), doc.shapes(), lit);
    if (lit == 0 && names[i] != "drw") { ++empty;  // drw.svg is an empty template: the firmware draws DrWave by code
      std::fprintf(stderr, "%s: drew nothing\n", names[i].c_str()); }
    if (sheet && *sheet) {
      write_bmp(std::string(sheet) + "/" + names[i] + ".bmp", im);
      const size_t s = i / per_sheet, k = i % per_sheet;
      if (sheets.size() <= s) sheets.emplace_back(cols * cw, (per_sheet / cols) * ch);
      for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x)
          sheets[s].at(int(k % cols) * cw + x, int(k / cols) * ch + y) = im.px[size_t(y) * cw + size_t(x)];
    }
  }
  CHECK_EQ(empty, 0);
  for (size_t s = 0; s < sheets.size(); ++s) {
    char nm[32];
    std::snprintf(nm, sizeof nm, "/screens-%02zu.bmp", s);
    write_bmp(std::string(sheet) + nm, sheets[s]);
  }
  return check_result("gui/test_render");
}
