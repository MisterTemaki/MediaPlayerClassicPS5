// Media Player Classic PS5: the font file check (mpc_sfnt.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_sfnt.h"

#include <algorithm>
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
		// the arguments are read only for ARGS_ARE_XY_VALUES (stb_truetype reads none for point matching, so the
		// walk follows the same bytes): words with ARG_1_AND_2_ARE_WORDS, else bytes
		p += 4;
		if (flags & 2)
			p += (flags & 1) ? 4 : 2;
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
// a simple glyph (numberOfContours >= 0): endPtsOfContours increasing, the instructions, the flags (with their repeats)
// and the x and y coordinates all inside its len bytes
bool SimpleGlyphFits(const uint8_t* g, uint32_t len)
{
	const uint32_t contours = U16(g);
	if (contours == 0)
		return true; // no outline: stb_truetype reads nothing past the header
	uint64_t p = 10 + 2ull * contours;
	if (p + 2 > len)
		return false;
	uint32_t points = 0;
	for (uint32_t c = 0; c < contours; c++)
	{
		const uint32_t end = U16(g + 10 + 2 * c);
		if (c && end < points) // points: the previous end + 1
			return false;
		points = end + 1;
	}
	p += 2 + U16(g + 10 + 2 * contours); // the instructions
	uint64_t xbytes = 0, ybytes = 0;
	for (uint32_t i = 0; i < points;)
	{
		if (p >= len)
			return false;
		const uint8_t flag = g[p++];
		uint32_t times = 1;
		if (flag & 8) // REPEAT_FLAG
		{
			if (p >= len)
				return false;
			times += g[p++];
		}
		if (times > points - i)
			times = points - i;
		xbytes += uint64_t(times) * ((flag & 2) ? 1 : (flag & 16) ? 0 : 2);
		ybytes += uint64_t(times) * ((flag & 4) ? 1 : (flag & 32) ? 0 : 2);
		i += times;
	}
	return p + xbytes + ybytes <= len;
}
} // namespace

bool GposKerningFits(const uint8_t* data, size_t size, uint32_t off)
{
	uint32_t gp, len;
	if (!FindTable(data, size, off, "GPOS", &gp, &len))
		return true; // none: the kern table is used
	const uint8_t* d = data + gp;
	auto in = [len](uint64_t o, uint64_t n) { return o + n <= len; };
	if (!in(0, 10))
		return false;
	if (U16(d) != 1 || U16(d + 2) != 0)
		return true; // a version stb_truetype leaves alone
	const uint32_t ll = U16(d + 8);
	if (!in(ll, 2) || !in(ll + 2, 2ull * U16(d + ll)))
		return false;
	const uint32_t lookups = U16(d + ll);
	for (uint32_t i = 0; i < lookups; i++)
	{
		const uint32_t lo = ll + U16(d + ll + 2 + 2 * i);
		if (!in(lo, 6))
			return false;
		if (U16(d + lo) != 2) // only pair adjustment is read
			continue;
		const uint32_t subs = U16(d + lo + 4);
		if (!in(lo + 6, 2ull * subs))
			return false;
		for (uint32_t k = 0; k < subs; k++)
		{
			const uint32_t st = lo + U16(d + lo + 6 + 2 * k);
			if (!in(st, 8))
				return false;
			const uint32_t format = U16(d + st), cov = st + U16(d + st + 2);
			// the coverage table and the largest coverage index it can give
			if (!in(cov, 4))
				return false;
			const uint32_t cformat = U16(d + cov), count = U16(d + cov + 2);
			int64_t max_index = -1;
			if (cformat == 1)
			{
				if (!in(cov + 4, 2ull * count))
					return false;
				max_index = int64_t(count) - 1;
			}
			else if (cformat == 2)
			{
				if (!in(cov + 4, 6ull * count))
					return false;
				for (uint32_t r = 0; r < count; r++)
				{
					const uint8_t* rr = d + cov + 4 + 6 * r;
					if (U16(rr + 2) >= U16(rr))
						max_index = std::max<int64_t>(max_index, int64_t(U16(rr + 4)) + U16(rr + 2) - U16(rr));
				}
			}
			if (max_index < 0 || U16(d + st + 4) != 4 || U16(d + st + 6) != 0)
				continue; // nothing covered, or value formats stb_truetype doesn't read
			if (format == 1)
			{
				// stb_truetype reads the pair set's offset and its count for any coverage index, then the pairs
				const uint32_t sets = in(st, 10) ? U16(d + st + 8) : 0;
				if (!in(st + 10, 2ull * uint64_t(max_index + 1)))
					return false;
				for (int64_t c = 0; c <= max_index; c++)
				{
					const uint32_t pv = st + U16(d + st + 10 + 2 * c);
					if (!in(pv, 2) || (c < sets && !in(pv + 2, 4ull * U16(d + pv))))
						return false;
				}
			}
			else if (format == 2)
			{
				if (!in(st, 16))
					return false;
				for (uint32_t cd : {st + U16(d + st + 8), st + U16(d + st + 10)})
				{
					if (!in(cd, 4))
						return false;
					if (U16(d + cd) == 1 && !(in(cd, 6) && in(cd + 6, 2ull * U16(d + cd + 4))))
						return false;
					if (U16(d + cd) == 2 && !in(cd + 4, 6ull * U16(d + cd + 2)))
						return false;
				}
				// every class pair's record inside the subtable
				if (!in(st + 16, 2ull * U16(d + st + 12) * U16(d + st + 14)))
					return false;
			}
		}
	}
	return true;
}

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
		// every glyph: a simple one's contours, instructions, flags and coordinates inside its data (stb_truetype sizes
		// its buffers from them); a composite's components inside its data, each a glyph of the font, nested at most 16
		// deep, never itself (stb_truetype recurses)
		std::vector<uint8_t> state(glyphs, 0); // 0 not seen, 1 being walked, 2 fine
		const char* what = nullptr;
		for (uint32_t g0 = 0; g0 < glyphs && !what; g0++)
		{
			if (state[g0])
				continue;
			auto walk = [&](uint32_t g, int depth, auto& self) -> void {
				if (what)
					return;
				if (g >= glyphs || depth > 16 || state[g] == 1)
				{
					what = "a composite glyph that is too deep, made of itself or of a glyph the font hasn't got";
					return;
				}
				if (state[g] == 2)
					return;
				state[g] = 1;
				const uint32_t len = at[g + 1] - at[g];
				const uint8_t* gd = data + t + at[g];
				if (len > 0 && len < 10)
					what = "a glyph shorter than its header";
				else if (len > 0 && int16_t(U16(gd)) < 0)
				{
					if (!Components(gd, len, [&](uint32_t c) { self(c, depth + 1, self); }))
						what = what ? what : "a composite glyph whose components are cut off";
				}
				else if (len > 0 && !SimpleGlyphFits(gd, len))
					what = "a glyph whose points run past its data";
				state[g] = 2;
			};
			walk(g0, 0, walk);
		}
		if (what)
			return Fail(why, what);
	}
	else if (!FindTable(data, size, off, "CFF ", &t, &t_len) || t_len < 4)
		return Fail(why, "no outlines (glyf or CFF)");
	if (offset)
		*offset = off;
	return true;
}
} // namespace sfnt
} // namespace mpc
