// Media Player Classic PS5: the font file check (mpc_sfnt.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_sfnt.h"

#include <vector>

namespace mpc
{
namespace sfnt
{
namespace
{
uint32_t U16(const uint8_t* p)
{
	return uint32_t(p[0]) << 8 | p[1];
}

uint32_t U32(const uint8_t* p)
{
	return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool Tag(const uint8_t* p, const char* t)
{
	return p[0] == uint8_t(t[0]) && p[1] == uint8_t(t[1]) && p[2] == uint8_t(t[2]) && p[3] == uint8_t(t[3]);
}

// what stb_truetype takes for a font (stbtt__isfont)
bool IsFontTag(const uint8_t* p)
{
	return Tag(p, "1\0\0\0") || Tag(p, "typ1") || Tag(p, "OTTO") || (p[0] == 0 && p[1] == 1 && p[2] == 0 && p[3] == 0) ||
		   Tag(p, "true");
}

bool Fail(std::string* why, const char* what)
{
	if (why)
		*why = what;
	return false;
}

// a composite glyph's components (TrueType glyf): calls fn(glyph index); false when it runs past the glyph's data
template <typename Fn>
bool Components(const uint8_t* g, uint32_t len, Fn fn)
{
	uint32_t p = 10;
	for (int n = 0; n < 1024; n++)
	{
		if (p + 4 > len)
			return false;
		const uint32_t flags = U16(g + p), glyph = U16(g + p + 2);
		p += 4 + ((flags & 1) ? 4 : 2); // ARG_1_AND_2_ARE_WORDS
		if (flags & 8)
			p += 2; // WE_HAVE_A_SCALE
		else if (flags & 0x40)
			p += 4; // X_AND_Y_SCALE
		else if (flags & 0x80)
			p += 8; // TWO_BY_TWO
		if (p > len)
			return false;
		fn(glyph);
		if (!(flags & 0x20)) // MORE_COMPONENTS
			return true;
	}
	return false;
}
} // namespace

bool FindTable(const uint8_t* data, size_t size, uint32_t off, const char* tag, uint32_t* at, uint32_t* len)
{
	if (uint64_t(off) + 12 > size)
		return false;
	const uint32_t n = U16(data + off + 4);
	if (uint64_t(off) + 12 + 16ull * n > size)
		return false;
	for (uint32_t i = 0; i < n; i++)
	{
		const uint8_t* e = data + off + 12 + 16 * i;
		if (Tag(e, tag))
		{
			*at = U32(e + 8);
			*len = U32(e + 12);
			return uint64_t(*at) + *len <= size;
		}
	}
	return false;
}

int CountFaces(const uint8_t* data, size_t size, std::string* why)
{
	if (!data || size < 12)
		return Fail(why, "shorter than a font's header"), 0;
	if (Tag(data, "ttcf"))
	{
		const uint32_t n = U32(data + 8);
		if (n == 0 || 12ull + 4ull * n > size)
			return Fail(why, "a collection whose list of fonts is cut off"), 0;
		return int(n > 16 ? 16 : n);
	}
	if (!IsFontTag(data))
		return Fail(why, "not a TrueType / OpenType font"), 0;
	return 1;
}

bool CheckFace(const uint8_t* data, size_t size, int index, uint32_t* offset, std::string* why)
{
	const int faces = CountFaces(data, size, why);
	if (index < 0 || index >= faces)
		return faces ? Fail(why, "no such face in the collection") : false;
	const uint32_t off = Tag(data, "ttcf") ? U32(data + 12 + 4 * index) : 0;
	if (uint64_t(off) + 12 > size || !IsFontTag(data + off))
		return Fail(why, "a face that starts outside the file");
	const uint32_t n = U16(data + off + 4);
	if (n == 0 || uint64_t(off) + 12 + 16ull * n > size)
		return Fail(why, "the table directory is cut off");
	for (uint32_t i = 0; i < n; i++)
	{
		const uint8_t* e = data + off + 12 + 16 * i;
		if (uint64_t(U32(e + 8)) + U32(e + 12) > size)
			return Fail(why, "a table runs past the end of the file (a file not copied whole?)");
	}
	uint32_t head, head_len, hhea, hhea_len, hmtx, hmtx_len, maxp, maxp_len, cmap, cmap_len, t, t_len;
	if (!FindTable(data, size, off, "head", &head, &head_len) || head_len < 54)
		return Fail(why, "no head table");
	if (!FindTable(data, size, off, "hhea", &hhea, &hhea_len) || hhea_len < 36)
		return Fail(why, "no hhea table");
	if (!FindTable(data, size, off, "maxp", &maxp, &maxp_len) || maxp_len < 6)
		return Fail(why, "no maxp table");
	if (!FindTable(data, size, off, "hmtx", &hmtx, &hmtx_len))
		return Fail(why, "no hmtx table");
	if (!FindTable(data, size, off, "cmap", &cmap, &cmap_len) || cmap_len < 4)
		return Fail(why, "no cmap table");
	if (U16(data + head + 18) < 16)
		return Fail(why, "a bad unitsPerEm");
	const uint32_t long_metrics = U16(data + hhea + 34), glyphs = U16(data + maxp + 4);
	if (long_metrics == 0 || 4ull * long_metrics > hmtx_len)
		return Fail(why, "the horizontal metrics are cut off");
	// the character maps: every subtable's header and arrays inside cmap
	const uint32_t maps = U16(data + cmap + 2);
	if (4ull + 8ull * maps > cmap_len)
		return Fail(why, "the cmap table is cut off");
	for (uint32_t i = 0; i < maps; i++)
	{
		const uint64_t o = U32(data + cmap + 4 + 8 * i + 4); // platform, encoding, offset
		if (o + 4 > cmap_len)
			return Fail(why, "a character map outside its table");
		const uint8_t* m = data + cmap + o;
		const uint32_t format = U16(m);
		uint64_t need = 4;
		if (format == 0)
			need = 6 + 256;
		else if (format == 4)
			need = o + 14 <= cmap_len ? 16 + 4ull * U16(m + 6) : 14;
		else if (format == 6)
			need = o + 10 <= cmap_len ? 10 + 2ull * U16(m + 8) : 10;
		else if (format == 12 || format == 13)
			need = o + 16 <= cmap_len ? 16 + 12ull * U32(m + 12) : 16;
		if (o + need > cmap_len)
			return Fail(why, "a character map is cut off");
	}
	if (FindTable(data, size, off, "glyf", &t, &t_len))
	{
		// TrueType outlines: every loca entry inside glyf, in order; composite glyphs without loops
		uint32_t loca, loca_len;
		if (!FindTable(data, size, off, "loca", &loca, &loca_len))
			return Fail(why, "glyf without loca");
		const uint32_t fmt = U16(data + head + 50);
		if (fmt > 1)
			return Fail(why, "a bad indexToLocFormat");
		if (uint64_t(glyphs + 1) * (fmt ? 4 : 2) > loca_len)
			return Fail(why, "the loca table is cut off");
		std::vector<uint32_t> at(glyphs + 1);
		for (uint32_t g = 0; g <= glyphs; g++)
		{
			at[g] = fmt ? U32(data + loca + 4 * g) : 2 * U16(data + loca + 2 * g);
			if (at[g] > t_len || (g && at[g] < at[g - 1]))
				return Fail(why, "a glyph outside the glyf table");
		}
		// composites: each component a glyph of the font, nested at most 16 deep, never itself (stb_truetype recurses)
		std::vector<uint8_t> state(glyphs, 0); // 0 not seen, 1 being walked, 2 fine
		for (uint32_t g0 = 0; g0 < glyphs; g0++)
		{
			if (state[g0])
				continue;
			bool bad = false;
			auto walk = [&](uint32_t g, int depth, auto& self) -> void {
				if (bad)
					return;
				if (g >= glyphs || depth > 16 || state[g] == 1)
				{
					bad = true;
					return;
				}
				if (state[g] == 2)
					return;
				state[g] = 1;
				const uint32_t len = at[g + 1] - at[g];
				if (len >= 10 && int16_t(U16(data + t + at[g])) < 0)
					if (!Components(data + t + at[g], len, [&](uint32_t c) { self(c, depth + 1, self); }))
						bad = true;
				state[g] = 2;
			};
			walk(g0, 0, walk);
			if (bad)
				return Fail(why, "a composite glyph that is cut off, too deep or made of itself");
		}
	}
	else if (!FindTable(data, size, off, "CFF ", &t, &t_len) || t_len < 4)
		return Fail(why, "no outlines (glyf or CFF)");
	if (offset)
		*offset = off;
	return true;
}
} // namespace sfnt
} // namespace mpc
