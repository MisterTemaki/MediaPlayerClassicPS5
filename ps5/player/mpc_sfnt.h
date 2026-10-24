// Media Player Classic PS5: a check of a TrueType / OpenType file (or collection) before stb_truetype reads it.
//
// stb_truetype trusts every offset in the file; the fonts folder is the user's, and a file copied half way over FTP or
// damaged is read as it is. So a font is only used once its table directory, the tables stb_truetype and the player
// read (cmap and its subtables, head, hhea, hmtx, maxp, loca and glyf or CFF, name, OS/2, kern) and every loca entry lie
// inside the file. (The owner of the data also pads it with zeros, kPad bytes, so the 16-bit offsets inside a table
// can't reach past the buffer either.)
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mpc
{
namespace sfnt
{
// zeros after a font's data in memory: room for the 16-bit offsets and counts inside a table
constexpr size_t kPad = 1 << 20;

// the number of faces in the file (1 for a .ttf/.otf, the collection's count for a .ttc, at most 16), or 0 when it
// isn't a font this player can read safely (why says what is wrong)
int CountFaces(const uint8_t* data, size_t size, std::string* why);
// face index of the file checked: its tables inside the file; offset gets where it starts
bool CheckFace(const uint8_t* data, size_t size, int index, uint32_t* offset, std::string* why);
// a table of the face at off: its offset and length (both inside the file once CheckFace said yes), false if absent
bool FindTable(const uint8_t* data, size_t size, uint32_t off, const char* tag, uint32_t* at, uint32_t* len);
} // namespace sfnt
} // namespace mpc
