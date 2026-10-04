#pragma once

// A minimal PNG writer for debug images (frames captured from the RSX backend): 8-bit RGB, no compression (zlib
// stored blocks), so it needs no library. Files are large (width * height * 3 bytes) but any viewer reads them.

#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket {

// The PNG file for `width` x `height` pixels given as 4 bytes each, rows top to bottom with `stride` bytes between
// them; the bytes of a pixel are taken in the order B, G, R (the fourth is ignored) when `bgra`, else R, G, B.
std::vector<u8> encode_png_rgb(const u8* pixels, u32 width, u32 height, u32 stride, bool bgra);

// Writes encode_png_rgb's file to `path`. False (and a log line) when the file cannot be written.
bool write_png_rgb(const std::string& path, const u8* pixels, u32 width, u32 height, u32 stride, bool bgra);

} // namespace pspocket
