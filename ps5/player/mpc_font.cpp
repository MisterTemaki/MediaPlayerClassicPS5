// Media Player Classic PS5: fonts and DrawText (mpc_font.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_font.h"

#include "mpc_res.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <dirent.h>

#include "OrbisPaths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace mpc
{
namespace
{
struct Face
{
	stbtt_fontinfo info;
	std::shared_ptr<std::vector<unsigned char>> owned; // a user font read from disk (shared by a collection's faces)
	int win_ascent = 0, win_descent = 0, units = 2048;
	bool ok = false;
	// a font of the fonts folder: its file ("name.ttc#1" for a collection's second face) and names
	std::string file, family, style;
	bool is_bold = false, is_italic = false;

	bool Init(const unsigned char* data, size_t size, int index = 0)
	{
		const int off = stbtt_GetFontOffsetForIndex(data, index);
		if (off < 0 || !stbtt_InitFont(&info, data, off))
			return false;
		(void)size;
		// Windows' text cell: OS/2 usWinAscent + usWinDescent
		int a = 0, d = 0, g = 0;
		stbtt_GetFontVMetrics(&info, &a, &d, &g);
		win_ascent = a;
		win_descent = -d;
		const stbtt_uint32 os2 = stbtt__find_table(const_cast<unsigned char*>(data), off, "OS/2");
		if (os2)
		{
			win_ascent = (data[os2 + 74] << 8) | data[os2 + 75];
			win_descent = (data[os2 + 76] << 8) | data[os2 + 77];
			// usWeightClass, fsSelection (bit 0 italic, bit 5 bold)
			const int weight = (data[os2 + 4] << 8) | data[os2 + 5];
			const int sel = (data[os2 + 62] << 8) | data[os2 + 63];
			is_bold = weight >= 600 || (sel & 0x20);
			is_italic = (sel & 1) != 0;
		}
		const stbtt_uint32 headoff = stbtt__find_table(const_cast<unsigned char*>(data), off, "head");
		const unsigned char* head = data + headoff;
		units = headoff ? (head[18] << 8 | head[19]) : 2048;
		if (headoff && !os2)
		{
			// macStyle: bit 0 bold, bit 1 italic
			is_bold = head[45] & 1;
			is_italic = (head[45] & 2) != 0;
		}
		ReadNames(data, off);
		ok = true;
		return true;
	}
	float ScaleFor(int em) const { return stbtt_ScaleForMappingEmToPixels(&info, float(em)); }

private:
	// the family and style names (the typographic ones, 16/17, when there are any), in UTF-8
	void ReadNames(const unsigned char* data, int off)
	{
		const stbtt_uint32 nm = stbtt__find_table(const_cast<unsigned char*>(data), off, "name");
		if (!nm)
			return;
		const int count = (data[nm + 2] << 8) | data[nm + 3];
		const int strings = nm + ((data[nm + 4] << 8) | data[nm + 5]);
		// the best record per name id: Windows Unicode US English, then Windows Unicode, then Unicode, then Mac Roman
		std::string best[18];
		int rank[18] = {};
		for (int i = 0; i < count && i < 4096; i++)
		{
			const unsigned char* r = data + nm + 6 + 12 * i;
			const int platform = (r[0] << 8) | r[1], enc = (r[2] << 8) | r[3], lang = (r[4] << 8) | r[5];
			const int id = (r[6] << 8) | r[7], len = (r[8] << 8) | r[9], o = (r[10] << 8) | r[11];
			if (id != 1 && id != 2 && id != 16 && id != 17)
				continue;
			int rk = 0;
			bool utf16 = true;
			if (platform == 3 && (enc == 1 || enc == 10))
				rk = lang == 0x409 ? 4 : 3;
			else if (platform == 0)
				rk = 2;
			else if (platform == 1 && enc == 0)
			{
				rk = 1;
				utf16 = false;
			}
			if (rk <= rank[id])
				continue;
			const unsigned char* p = data + strings + o;
			std::string out;
			for (int k = 0; k < len;)
			{
				uint32_t cp;
				if (utf16)
				{
					if (k + 1 >= len)
						break;
					cp = (p[k] << 8) | p[k + 1];
					k += 2;
					if (cp >= 0xd800 && cp < 0xdc00 && k + 1 < len)
					{
						const uint32_t lo = (p[k] << 8) | p[k + 1];
						cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
						k += 2;
					}
				}
				else
					cp = p[k++]; // Mac Roman: its ASCII half
				if (cp < 0x20)
					continue;
				if (cp < 0x80)
					out += char(cp);
				else if (cp < 0x800)
				{
					out += char(0xc0 | (cp >> 6));
					out += char(0x80 | (cp & 0x3f));
				}
				else if (cp < 0x10000)
				{
					out += char(0xe0 | (cp >> 12));
					out += char(0x80 | ((cp >> 6) & 0x3f));
					out += char(0x80 | (cp & 0x3f));
				}
				else
				{
					out += char(0xf0 | (cp >> 18));
					out += char(0x80 | ((cp >> 12) & 0x3f));
					out += char(0x80 | ((cp >> 6) & 0x3f));
					out += char(0x80 | (cp & 0x3f));
				}
			}
			if (!out.empty())
			{
				best[id] = out;
				rank[id] = rk;
			}
		}
		family = !best[16].empty() ? best[16] : best[1];
		style = !best[17].empty() ? best[17] : best[2];
	}
};

struct Faces
{
	Face regular, bold, roboto;
	std::vector<std::unique_ptr<Face>> extra;
	bool loaded = false;

	void Load()
	{
		if (loaded)
			return;
		loaded = true;
		const Blob r = res::FontSelawik(), b = res::FontSelawikBold(), ro = res::FontRoboto();
		regular.Init(r.data, r.size);
		bold.Init(b.data, b.size);
		roboto.Init(ro.data, ro.size);
	}
};
Faces g_faces;

struct Glyph
{
	std::vector<uint8_t> bits;
	int w = 0, h = 0, xoff = 0, yoff = 0;
	float advance = 0;
};

struct GlyphKey
{
	const Face* face;
	int em;
	int glyph;
	bool synth;
	bool operator<(const GlyphKey& o) const
	{
		if (face != o.face)
			return face < o.face;
		if (em != o.em)
			return em < o.em;
		if (glyph != o.glyph)
			return glyph < o.glyph;
		return synth < o.synth;
	}
};
std::map<GlyphKey, Glyph> g_glyphs;

// UTF-8 to code points (bad bytes become U+FFFD)
std::vector<uint32_t> Decode(const std::string& s)
{
	std::vector<uint32_t> out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();)
	{
		const unsigned char c = s[i];
		uint32_t cp = 0xfffd;
		int n = 1;
		if (c < 0x80)
			cp = c;
		else if ((c >> 5) == 6 && i + 1 < s.size())
		{
			cp = ((c & 0x1f) << 6) | (s[i + 1] & 0x3f);
			n = 2;
		}
		else if ((c >> 4) == 14 && i + 2 < s.size())
		{
			cp = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);
			n = 3;
		}
		else if ((c >> 3) == 30 && i + 3 < s.size())
		{
			cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3f) << 12) | ((s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f);
			n = 4;
		}
		out.push_back(cp);
		i += n;
	}
	return out;
}

// the face that has cp, and its glyph index: primary (a subtitle font) first, then Selawik (Bold), Roboto, the
// folder's fonts
const Face* Pick(uint32_t cp, const Face* primary, bool bold, int* glyph)
{
	g_faces.Load();
	if (primary && primary->ok && (*glyph = stbtt_FindGlyphIndex(&primary->info, int(cp))) != 0)
		return primary;
	const Face* first = bold ? &g_faces.bold : &g_faces.regular;
	if (first->ok)
		if ((*glyph = stbtt_FindGlyphIndex(&first->info, int(cp))) != 0)
			return first;
	if (g_faces.roboto.ok && (*glyph = stbtt_FindGlyphIndex(&g_faces.roboto.info, int(cp))) != 0)
		return &g_faces.roboto;
	for (const auto& f : g_faces.extra)
		if (f.get() != primary && (*glyph = stbtt_FindGlyphIndex(&f->info, int(cp))) != 0)
			return f.get();
	// nothing has it: the main face's .notdef box
	*glyph = 0;
	if (primary && primary->ok)
		return primary;
	return first->ok ? first : nullptr;
}

// a synthetic bold's extra width, as FreeType's FT_GlyphSlot_Embolden (em / 24)
int SynthStrength(int em)
{
	return std::max(1, int(std::lround(em / 24.0)));
}

const Glyph& GetGlyph(const Face* f, int em, int glyph, bool synth)
{
	GlyphKey k{f, em, glyph, synth};
	auto it = g_glyphs.find(k);
	if (it != g_glyphs.end())
		return it->second;
	Glyph& g = g_glyphs[k];
	const float scale = f->ScaleFor(em);
	int adv = 0, lsb = 0;
	stbtt_GetGlyphHMetrics(&f->info, glyph, &adv, &lsb);
	g.advance = adv * scale;
	int x0, y0, x1, y1;
	stbtt_GetGlyphBitmapBox(&f->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
	g.w = x1 - x0;
	g.h = y1 - y0;
	g.xoff = x0;
	g.yoff = y0;
	if (g.w > 0 && g.h > 0)
	{
		g.bits.assign(size_t(g.w) * g.h, 0);
		stbtt_MakeGlyphBitmap(&f->info, g.bits.data(), g.w, g.h, g.w, scale, scale, glyph);
		// a touch more weight, as ClearType's gamma gives Segoe UI on a light background
		for (uint8_t& b : g.bits)
			b = uint8_t(std::min(255, int(std::pow(b / 255.0, 0.8) * 255 + 0.5)));
		if (synth)
		{
			// synthetic bold: the strokes widened by s pixels to the right and s / 2 upwards, the advance by s
			const int s = SynthStrength(em), sv = s / 2;
			const int nw = g.w + s, nh = g.h + sv;
			std::vector<uint8_t> nb(size_t(nw) * nh, 0);
			for (int y = 0; y < g.h; y++)
				for (int x = 0; x < g.w; x++)
				{
					const uint8_t v = g.bits[size_t(y) * g.w + x];
					if (!v)
						continue;
					for (int dy = 0; dy <= sv; dy++)
					{
						uint8_t* row = nb.data() + size_t(y + sv - dy) * nw;
						for (int dx = 0; dx <= s; dx++)
							row[x + dx] = std::max(row[x + dx], v);
					}
				}
			g.bits.swap(nb);
			g.w = nw;
			g.h = nh;
			g.yoff -= sv;
		}
	}
	if (synth)
		g.advance += SynthStrength(em);
	return g;
}
} // namespace

// what Layout needs of a Font
struct FontAccess
{
	static const Face* FaceOf(const Font& f) { return static_cast<const Face*>(f.face_); }
	static bool Synth(const Font& f) { return f.synth_; }
};

namespace
{
// lays a line out: calls fn(glyph, x) for each, returns the width
template <typename Fn>
float Layout(const Font& font, const std::string& utf8, Fn fn)
{
	const int em = font.Em();
	const bool bold = font.bold();
	const Face* primary = FontAccess::FaceOf(font);
	const bool synth = FontAccess::Synth(font);
	const std::vector<uint32_t> cps = Decode(utf8);
	float x = 0;
	const Face* prev_face = nullptr;
	int prev_glyph = 0;
	for (uint32_t cp : cps)
	{
		if (cp == '\t')
			cp = ' ';
		int glyph = 0;
		const Face* f = Pick(cp, primary, bold, &glyph);
		if (!f)
			continue;
		if (prev_face == f && prev_glyph)
			x += stbtt_GetGlyphKernAdvance(&f->info, prev_glyph, glyph) * f->ScaleFor(em);
		const Glyph& g = GetGlyph(f, em, glyph, synth && f == primary);
		fn(g, x);
		x += g.advance;
		prev_face = f;
		prev_glyph = glyph;
	}
	return x;
}

std::map<std::pair<int, bool>, std::unique_ptr<Font>> g_sized;
} // namespace

Font::Font(int em, bool bold, const void* face, bool synth_bold) : em_(em), bold_(bold), face_(face), synth_(synth_bold)
{
	g_faces.Load();
	const Face* f = static_cast<const Face*>(face);
	if (!f || !f->ok)
		f = bold ? &g_faces.bold : &g_faces.regular;
	if (f->ok)
	{
		const float scale = f->ScaleFor(em);
		ascent_ = int(std::ceil(f->win_ascent * scale));
		descent_ = int(std::ceil(f->win_descent * scale));
	}
	else
	{
		ascent_ = em;
		descent_ = em / 4;
	}
}

Font::Font(int em, bool bold) : em_(em), bold_(bold)
{
	g_faces.Load();
	const Face& f = bold ? g_faces.bold : g_faces.regular;
	if (f.ok)
	{
		const float scale = f.ScaleFor(em);
		ascent_ = int(std::ceil(f.win_ascent * scale));
		descent_ = int(std::ceil(f.win_descent * scale));
	}
	else
	{
		ascent_ = em;
		descent_ = em / 4;
	}
}

int Font::Measure(const std::string& utf8) const
{
	return int(std::ceil(Layout(*this, utf8, [](const Glyph&, float) {})));
}

int Font::Draw(CDC& dc, int x, int y, const std::string& utf8, COLORREF c, int opacity) const
{
	const int base = y + ascent_;
	const float w = Layout(*this, utf8, [&](const Glyph& g, float gx) {
		if (!g.bits.empty())
			dc.MaskBlit(x + int(std::lround(gx)) + g.xoff, base + g.yoff, g.bits.data(), g.w, g.h, g.w, c, opacity);
	});
	return int(std::ceil(w));
}

int Font::DrawOutlined(CDC& dc, int x, int y, const std::string& utf8, COLORREF c, COLORREF outline, int width) const
{
	if (width > 0)
		for (int dy = -width; dy <= width; dy++)
			for (int dx = -width; dx <= width; dx++)
				if ((dx || dy) && dx * dx + dy * dy <= width * width + width)
					Draw(dc, x + dx, y + dy, utf8, outline);
	return Draw(dc, x, y, utf8, c);
}

void Font::DrawMask(uint8_t* mask, int w, int h, int pitch, int x, int y, const std::string& utf8) const
{
	const int base = y + ascent_;
	Layout(*this, utf8, [&](const Glyph& g, float gx) {
		const int ox = x + int(std::lround(gx)) + g.xoff, oy = base + g.yoff;
		for (int yy = 0; yy < g.h; yy++)
		{
			const int ty = oy + yy;
			if (ty < 0 || ty >= h)
				continue;
			for (int xx = 0; xx < g.w; xx++)
			{
				const int tx = ox + xx;
				if (tx < 0 || tx >= w)
					continue;
				uint8_t& d = mask[size_t(ty) * pitch + tx];
				d = std::max(d, g.bits[size_t(yy) * g.w + xx]);
			}
		}
	});
}

Font& FontOfSize(int em, bool bold)
{
	auto& slot = g_sized[{em, bold}];
	if (!slot)
		slot.reset(new Font(em, bold));
	return *slot;
}

// Segoe UI 9 pt: 12 px at 96 dpi
Font& UiFont()
{
	return FontOfSize(ScaleY(12), false);
}

Font& UiBoldFont()
{
	return FontOfSize(ScaleY(12), true);
}

std::string StripPrefix(const std::string& text)
{
	std::string out;
	for (size_t i = 0; i < text.size(); i++)
	{
		if (text[i] == '&')
		{
			if (i + 1 < text.size() && text[i + 1] == '&')
			{
				out += '&';
				i++;
			}
			continue;
		}
		out += text[i];
	}
	return out;
}

char PrefixChar(const std::string& text)
{
	for (size_t i = 0; i + 1 < text.size(); i++)
		if (text[i] == '&')
		{
			if (text[i + 1] == '&')
			{
				i++;
				continue;
			}
			const char c = text[i + 1];
			return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
		}
	return 0;
}

std::string FitText(const Font& f, const std::string& text, int max_px)
{
	if (f.Measure(text) <= max_px)
		return text;
	const std::vector<uint32_t> cps = Decode(text);
	// cut whole code points from the end until text + "..." fits
	std::string cut;
	for (size_t n = cps.size(); n > 0; n--)
	{
		cut.clear();
		for (size_t i = 0; i < n - 1; i++)
		{
			const uint32_t cp = cps[i];
			if (cp < 0x80)
				cut += char(cp);
			else if (cp < 0x800)
			{
				cut += char(0xc0 | (cp >> 6));
				cut += char(0x80 | (cp & 0x3f));
			}
			else if (cp < 0x10000)
			{
				cut += char(0xe0 | (cp >> 12));
				cut += char(0x80 | ((cp >> 6) & 0x3f));
				cut += char(0x80 | (cp & 0x3f));
			}
			else
			{
				cut += char(0xf0 | (cp >> 18));
				cut += char(0x80 | ((cp >> 12) & 0x3f));
				cut += char(0x80 | ((cp >> 6) & 0x3f));
				cut += char(0x80 | (cp & 0x3f));
			}
		}
		if (f.Measure(cut + "...") <= max_px)
			return cut + "...";
	}
	return "...";
}

std::vector<std::string> WrapText(const Font& f, const std::string& text, int max_px)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t nl = text.find('\n', start);
		const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		std::string line;
		size_t i = 0;
		while (i < para.size())
		{
			size_t sp = para.find(' ', i);
			const std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
			const std::string trial = line.empty() ? word : line + " " + word;
			if (f.Measure(trial) <= max_px || line.empty())
			{
				if (line.empty() && f.Measure(word) > max_px)
				{
					// a word longer than the line: cut it where it overflows
					std::string part;
					for (char ch : word)
					{
						if ((ch & 0xc0) != 0x80 && !part.empty() && f.Measure(part + ch) > max_px)
						{
							lines.push_back(part);
							part.clear();
						}
						part += ch;
					}
					line = part;
				}
				else
					line = trial;
			}
			else
			{
				lines.push_back(line);
				line = word;
			}
			if (sp == std::string::npos)
				break;
			i = sp + 1;
		}
		lines.push_back(line);
		if (nl == std::string::npos)
			break;
		start = nl + 1;
	}
	return lines;
}

int DrawText(CDC& dc, const Font& f, const std::string& text, CRect& r, unsigned flags, COLORREF c)
{
	const bool prefix = !(flags & DT_NOPREFIX);
	const std::string shown = prefix ? StripPrefix(text) : text;
	if (!(flags & DT_SINGLELINE) && (flags & DT_WORDBREAK))
	{
		const std::vector<std::string> lines = WrapText(f, shown, std::max(1, r.Width()));
		const int lh = f.Height();
		if (flags & DT_CALCRECT)
		{
			int w = 0;
			for (const auto& l : lines)
				w = std::max(w, f.Measure(l));
			r.right = r.left + w;
			r.bottom = r.top + lh * int(lines.size());
			return r.Height();
		}
		int y = r.top;
		for (const auto& l : lines)
		{
			int x = r.left;
			const int w = f.Measure(l);
			if (flags & DT_CENTER)
				x = r.left + (r.Width() - w) / 2;
			else if (flags & DT_RIGHT)
				x = r.right - w;
			f.Draw(dc, x, y, l, c);
			y += lh;
		}
		return y - r.top;
	}
	std::string line = shown;
	if (flags & DT_END_ELLIPSIS)
		line = FitText(f, line, r.Width());
	const int w = f.Measure(line), h = f.Height();
	if (flags & DT_CALCRECT)
	{
		r.right = r.left + w;
		r.bottom = r.top + h;
		return h;
	}
	int x = r.left, y = r.top;
	if (flags & DT_CENTER)
		x = r.left + (r.Width() - w) / 2;
	else if (flags & DT_RIGHT)
		x = r.right - w;
	if (flags & DT_VCENTER)
		y = r.top + (r.Height() - h) / 2;
	else if (flags & DT_BOTTOM)
		y = r.bottom - h;
	const CRect old = dc.GetClip();
	CRect clip = r;
	clip.Intersect(old);
	dc.SetClip(clip);
	f.Draw(dc, x, y, line, c);
	// the accelerator's underline
	if (prefix && !(flags & DT_HIDEPREFIX) && line == shown)
	{
		size_t pos = std::string::npos;
		for (size_t i = 0, o = 0; i + 1 < text.size(); i++, o++)
		{
			if (text[i] == '&')
			{
				if (text[i + 1] == '&')
				{
					i++;
					continue;
				}
				pos = o;
				break;
			}
		}
		if (pos != std::string::npos && pos < shown.size())
		{
			size_t end = pos + 1;
			while (end < shown.size() && (shown[end] & 0xc0) == 0x80)
				end++;
			const int ux = x + f.Measure(shown.substr(0, pos));
			const int uw = std::max(1, f.Measure(shown.substr(pos, end - pos)));
			const int uy = y + f.Ascent() + std::max(1, f.Em() / 12);
			dc.FillSolidRect(CRect(ux, uy, ux + uw, uy + std::max(1, f.Em() / 14)), c);
		}
	}
	dc.SetClip(old);
	return h;
}

namespace
{
bool IsFontFile(const std::string& n)
{
	std::string low = n;
	for (char& ch : low)
		ch = char(tolower(static_cast<unsigned char>(ch)));
	return low.size() > 4 && (low.compare(low.size() - 4, 4, ".ttf") == 0 || low.compare(low.size() - 4, 4, ".otf") == 0 ||
								 low.compare(low.size() - 4, 4, ".ttc") == 0);
}

std::string Lower(std::string s)
{
	for (char& ch : s)
		ch = char(tolower(static_cast<unsigned char>(ch)));
	return s;
}

// the subtitles' font: nullptr Selawik, &g_faces.roboto, or one of g_faces.extra
const Face* g_sub_face = nullptr;
std::map<std::tuple<int, const Face*, bool, bool>, std::unique_ptr<Font>> g_sub_sized;

// the face drawing bold text for sub (the family's bold face in the folder), and whether it must be thickened
const Face* BoldFaceFor(const Face* sub, bool* synth)
{
	*synth = false;
	if (!sub || sub->is_bold)
		return sub;
	if (sub != &g_faces.roboto && !sub->family.empty())
	{
		const std::string fam = Lower(sub->family);
		const Face* best = nullptr;
		// the same slant first
		for (const auto& f : g_faces.extra)
			if (f->is_bold && Lower(f->family) == fam &&
				(!best || (f->is_italic == sub->is_italic && best->is_italic != sub->is_italic)))
				best = f.get();
		if (best)
			return best;
	}
	*synth = true;
	return sub;
}

std::string ChoiceName(const Face& f)
{
	std::string name = f.family.empty() ? f.file : f.family;
	if (!f.style.empty() && Lower(f.style) != "regular" && Lower(f.style) != "normal" && Lower(f.style) != "book")
		name += " " + f.style;
	return name;
}
} // namespace

int LoadExtraFonts(const std::string& dir)
{
	g_faces.Load();
	DIR* d = opendir(dir.c_str());
	if (!d)
		return 0;
	int added = 0;
	std::vector<std::string> names;
	while (dirent* e = readdir(d))
	{
		const std::string n = e->d_name;
		if (IsFontFile(n))
			names.push_back(n);
	}
	closedir(d);
	std::sort(names.begin(), names.end());
	for (const auto& n : names)
	{
		bool have = false;
		for (const auto& f : g_faces.extra)
			if (f->file == n || f->file.compare(0, n.size() + 1, n + "#") == 0)
				have = true;
		if (have)
			continue;
		FILE* f = fopen((dir + "/" + n).c_str(), "rb");
		if (!f)
			continue;
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		auto data = std::make_shared<std::vector<unsigned char>>();
		if (size > 0 && size < (64l << 20))
		{
			data->resize(size_t(size));
			if (fread(data->data(), 1, data->size(), f) == data->size())
			{
				// a collection (.ttc): each of its faces (up to 16)
				const int faces = std::max(1, std::min(16, stbtt_GetNumberOfFonts(data->data())));
				for (int i = 0; i < faces; i++)
				{
					std::unique_ptr<Face> face(new Face);
					face->owned = data;
					if (!face->Init(data->data(), data->size(), i))
						continue;
					face->file = i ? n + "#" + std::to_string(i) : n;
					g_faces.extra.push_back(std::move(face));
					added++;
				}
			}
		}
		fclose(f);
	}
	return added;
}

std::vector<FontChoice> SubtitleFontChoices(const std::string& dir)
{
	const int added = LoadExtraFonts(dir);
	std::vector<FontChoice> out = {{"", "Selawik (Segoe UI)"}, {"Roboto", "Roboto"}};
	std::vector<FontChoice> folder;
	for (const auto& f : g_faces.extra)
	{
		// only the files still there
		const std::string file = f->file.substr(0, f->file.find('#'));
		if (FILE* t = fopen((dir + "/" + file).c_str(), "rb"))
		{
			fclose(t);
			folder.push_back({f->file, ChoiceName(*f)});
		}
	}
	std::stable_sort(folder.begin(), folder.end(), [](const FontChoice& a, const FontChoice& b) { return Lower(a.name) < Lower(b.name); });
	OrbisLog("[subs] Default Style fonts: %zu in %s (%d new)", folder.size(), dir.c_str(), added);
	out.insert(out.end(), folder.begin(), folder.end());
	return out;
}

namespace
{
// the face of a Default Style font setting: nullptr Selawik; *found false when it isn't loaded
const Face* FaceForFile(const std::string& file, bool* found)
{
	g_faces.Load();
	*found = true;
	if (file.empty())
		return nullptr;
	if (file == "Roboto")
		return &g_faces.roboto;
	for (const auto& f : g_faces.extra)
		if (f->file == file)
			return f.get();
	*found = false;
	return nullptr;
}

Font& SubFontFor(const Face* face, int em, bool bold)
{
	if (!face)
		return FontOfSize(em, bold);
	bool synth = false;
	const Face* f = bold ? BoldFaceFor(face, &synth) : face;
	auto& slot = g_sub_sized[std::make_tuple(em, f, synth, bold)];
	if (!slot)
		slot.reset(new Font(em, bold, f, synth));
	return *slot;
}
} // namespace

bool SetSubtitleFont(const std::string& file)
{
	bool found = true;
	const Face* face = FaceForFile(file, &found);
	if (face != g_sub_face || !found)
	{
		if (!found)
			OrbisLog("[subs] the Default Style's font %s isn't in the fonts folder: Selawik", file.c_str());
		else if (!face)
			OrbisLog("[subs] Default Style font: Selawik (Segoe UI)");
		else
		{
			bool synth = false;
			const Face* b = BoldFaceFor(face, &synth);
			OrbisLog("[subs] Default Style font: %s (%s); bold: %s", ChoiceName(*face).c_str(),
				face->file.empty() ? "the app's own" : face->file.c_str(),
				synth ? "synthetic (no bold face of this family in the folder)" : b == face ? "the font itself" : b->file.c_str());
		}
	}
	g_sub_face = face;
	return found;
}

Font& SubtitleFontOfSize(int em, bool bold)
{
	return SubFontFor(g_sub_face, em, bold);
}

int RenderSubtitleText(const SubtitleStyle& st, const std::vector<std::string>& in_lines, const std::vector<bool>& in_italic,
	int max_w, Image& img)
{
	bool found = true;
	const Font& f = st.own_font ? SubFontFor(FaceForFile(st.font, &found), st.em, st.bold) : SubtitleFontOfSize(st.em, st.bold);
	std::vector<std::string> lines;
	std::vector<bool> italic;
	for (size_t li = 0; li < in_lines.size(); li++)
		for (const auto& w : WrapText(f, in_lines[li], max_w))
		{
			lines.push_back(w);
			italic.push_back(li < in_italic.size() && in_italic[li]);
		}
	const int outline = std::max(0, st.outline), shadow = std::max(0, st.shadow);
	std::vector<int> lw(lines.size());
	int tw = 0;
	for (size_t i = 0; i < lines.size(); i++)
		tw = std::max(tw, lw[i] = f.Measure(lines[i]));
	const int slant = f.Height() / 5; // room for a slanted line
	// the opaque box's border around each line (VSFilter: the outline's width)
	const int border = st.box ? std::max(outline, std::max(2, f.Height() / 10)) : 0;
	const int pad = std::max(outline, border) + shadow + 2 + slant;
	const int lh = f.Height();
	const int iw = tw + 2 * pad, ih = int(lines.size()) * lh + 2 * pad;
	std::vector<uint8_t> text(size_t(iw) * ih, 0);
	for (size_t i = 0; i < lines.size(); i++)
	{
		const int ly = pad + int(i) * lh;
		f.DrawMask(text.data(), iw, ih, iw, pad + (tw - lw[i]) / 2, ly, lines[i]);
		if (italic[i])
		{
			// italics: the line slanted (no italic face is used), 0.2 px per px from its baseline
			const int base = ly + f.Ascent();
			for (int y = std::max(0, ly); y < std::min(ih, ly + lh); y++)
			{
				const int shift = int((base - y) * 0.2);
				if (shift == 0)
					continue;
				uint8_t* row = text.data() + size_t(y) * iw;
				if (shift > 0)
				{
					for (int x = iw - 1; x >= shift; x--)
						row[x] = row[x - shift];
					std::fill(row, row + std::min(shift, iw), uint8_t(0));
				}
				else
				{
					for (int x = 0; x < iw + shift; x++)
						row[x] = row[x - shift];
					std::fill(row + std::max(0, iw + shift), row + iw, uint8_t(0));
				}
			}
		}
	}
	// the outline (the text grown by `outline` pixels), or the boxes behind the lines
	std::vector<uint8_t> edge(text.size(), 0);
	if (st.box)
	{
		for (size_t i = 0; i < lines.size(); i++)
		{
			if (lw[i] <= 0)
				continue;
			const int lx = pad + (tw - lw[i]) / 2, ly = pad + int(i) * lh;
			const int x0 = std::max(0, lx - border - (italic[i] ? slant / 2 : 0)), x1 = std::min(iw, lx + lw[i] + border + (italic[i] ? slant / 2 : 0));
			const int y0 = std::max(0, ly - (i == 0 ? border : 0)), y1 = std::min(ih, ly + lh + (i + 1 == lines.size() ? border : 0));
			for (int y = y0; y < y1; y++)
				std::fill(edge.begin() + size_t(y) * iw + x0, edge.begin() + size_t(y) * iw + x1, uint8_t(255));
		}
	}
	else if (outline > 0)
	{
		std::vector<uint8_t> tmp(text.size(), 0);
		// separable max: rows then columns (a square; close enough for these sizes)
		for (int y = 0; y < ih; y++)
			for (int x = 0; x < iw; x++)
			{
				uint8_t m = 0;
				for (int k = std::max(0, x - outline); k <= std::min(iw - 1, x + outline); k++)
					m = std::max(m, text[size_t(y) * iw + k]);
				tmp[size_t(y) * iw + x] = m;
			}
		for (int y = 0; y < ih; y++)
			for (int x = 0; x < iw; x++)
			{
				uint8_t m = 0;
				for (int k = std::max(0, y - outline); k <= std::min(ih - 1, y + outline); k++)
					m = std::max(m, tmp[size_t(k) * iw + x]);
				edge[size_t(y) * iw + x] = m;
			}
	}
	else
		edge = text;
	img.Create(iw, ih);
	uint32_t* px = img.Data();
	const int ea_scale = st.box ? std::max(0, std::min(255, st.box_alpha)) : 255;
	const uint32_t tc[3] = {uint32_t(GetRValue(st.color)), uint32_t(GetGValue(st.color)), uint32_t(GetBValue(st.color))};
	const uint32_t oc[3] = {uint32_t(GetRValue(st.outline_color)), uint32_t(GetGValue(st.outline_color)),
		uint32_t(GetBValue(st.outline_color))};
	for (int y = 0; y < ih; y++)
		for (int x = 0; x < iw; x++)
		{
			const size_t i = size_t(y) * iw + x;
			// shadow (the outline or the box shifted, half black), then the outline / box, then the text: premultiplied
			const int sxp = x - shadow, syp = y - shadow;
			uint32_t sh = 0;
			if (shadow > 0 && sxp >= 0 && syp >= 0)
			{
				sh = edge[size_t(syp) * iw + sxp] * uint32_t(ea_scale) / 255 / 2;
				if (st.box) // only where the box isn't (a see-through box shows no shadow through it)
					sh = sh * (255 - edge[i]) / 255;
			}
			uint32_t a = sh;
			uint32_t c[3] = {0, 0, 0};
			const uint32_t ea = edge[i] * uint32_t(ea_scale) / 255;
			for (int k = 0; k < 3; k++)
				c[k] = (oc[k] * ea + c[k] * (255 - ea)) / 255;
			a = ea + a * (255 - ea) / 255;
			const uint32_t ta = text[i];
			for (int k = 0; k < 3; k++)
				c[k] = (tc[k] * ta + c[k] * (255 - ta)) / 255;
			a = ta + a * (255 - ta) / 255;
			px[i] = c[0] | (c[1] << 8) | (c[2] << 16) | (a << 24);
		}
	return pad;
}

} // namespace mpc
