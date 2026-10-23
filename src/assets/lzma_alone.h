// Minimal LZMA "alone" (.lzma) decoder, written from the public LZMA specification (LzmaSpec.cpp by
// Igor Pavlov, public domain). Only what the OG firmware container needs: one stream, output size in the
// header, decoded into memory. No dependency on liblzma (not installed on the build machines).
// Owner: t6.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace op1 {

// in: 13-byte header (props, dict size, 8-byte uncompressed size) + range-coded data. The uncompressed
// size must be stored in the header (not -1) and not exceed max_out. false + err on corrupt input.
bool lzma_alone_decode(const uint8_t* in, size_t in_size, size_t max_out, std::vector<uint8_t>& out,
                       std::string& err);

uint32_t crc32_ieee(const uint8_t* p, size_t n);

}  // namespace op1
