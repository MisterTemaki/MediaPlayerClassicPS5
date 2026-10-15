// Media Player Classic PS5: fonts and DrawText (mpc_font.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_font.h"

#include "mpc_res.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <dirent.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

namespace mpc
{
namespace
{
struct Face
{
	stbtt_fontinfo info;
	std::vector<unsigned char> owned; // a user font read from disk
	int win_ascent = 0, win_descent = 0, units = 2048;
	bool ok = false;

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
		}
		const unsigned char* head = data + stbtt__find_table(const_cast<unsigned char*>(data), off, "head");
		units = head != data ? (head[18] << 8 | head[19]) : 2048;
		ok = true;
		return true;
	}
	float ScaleFor(int em) const { return stbtt_ScaleForMappingEmToPixels(&info, float(em)); }
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
	bool operator<(const GlyphKey& o) const
	{
		if (face != o.face)
			return face < o.face;
		if (em != o.em)
			return em < o.em;
		return glyph < o.glyph;
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

// the face that has cp, and its glyph index
const Face* Pick(uint32_t cp, bool bold, int* glyph)
{
	g_faces.Load();
	const Face* first = bold ? &g_faces.bold : &g_faces.regular;
	if (first->ok)
		if ((*glyph = stbtt_FindGlyphIndex(&first->info, int(cp))) != 0)
			return first;
	if (g_faces.roboto.ok && (*glyph = stbtt_FindGlyphIndex(&g_faces.roboto.info, int(cp))) != 0)
		return &g_faces.roboto;
	for (const auto& f : g_faces.extra)
		if ((*glyph = stbtt_FindGlyphIndex(&f->info, int(cp))) != 0)
			return f.get();
	// nothing has it: the main face's .notdef box
	*glyph = 0;
	return first->ok ? first : nullptr;
}

const Glyph& GetGlyph(const Face* f, int em, int glyph)
{
	GlyphKey k{f, em, glyph};
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
	}
	return g;
}

// lays a line out: calls fn(face, glyph, x) for each, returns the width
template <typename Fn>
float Layout(const std::string& utf8, int em, bool bold, Fn fn)
{
	const std::vector<uint32_t> cps = Decode(utf8);
	float x = 0;
	const Face* prev_face = nullptr;
	int prev_glyph = 0;
	for (uint32_t cp : cps)
	{
		if (cp == '\t')
			cp = ' ';
		int glyph = 0;
		const Face* f = Pick(cp, bold, &glyph);
		if (!f)
			continue;
		if (prev_face == f && prev_glyph)
			x += stbtt_GetGlyphKernAdvance(&f->info, prev_glyph, glyph) * f->ScaleFor(em);
		const Glyph& g = GetGlyph(f, em, glyph);
		fn(g, x);
		x += g.advance;
		prev_face = f;
		prev_glyph = glyph;
	}
	return x;
}

std::map<std::pair<int, bool>, std::unique_ptr<Font>> g_sized;
} // namespace

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
	return int(std::ceil(Layout(utf8, em_, bold_, [](const Glyph&, float) {})));
}

int Font::Draw(CDC& dc, int x, int y, const std::string& utf8, COLORREF c, int opacity) const
{
	const int base = y + ascent_;
	const float w = Layout(utf8, em_, bold_, [&](const Glyph& g, float gx) {
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
	Layout(utf8, em_, bold_, [&](const Glyph& g, float gx) {
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

void LoadExtraFonts(const std::string& dir)
{
	g_faces.Load();
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	std::vector<std::string> names;
	while (dirent* e = readdir(d))
	{
		std::string n = e->d_name;
		std::string low = n;
		for (char& ch : low)
			ch = char(tolower(ch));
		if (low.size() > 4 &&
			(low.compare(low.size() - 4, 4, ".ttf") == 0 || low.compare(low.size() - 4, 4, ".otf") == 0 ||
				low.compare(low.size() - 4, 4, ".ttc") == 0))
			names.push_back(n);
	}
	closedir(d);
	std::sort(names.begin(), names.end());
	for (const auto& n : names)
	{
		FILE* f = fopen((dir + "/" + n).c_str(), "rb");
		if (!f)
			continue;
		std::unique_ptr<Face> face(new Face);
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size > 0 && size < (64l << 20))
		{
			face->owned.resize(size_t(size));
			if (fread(face->owned.data(), 1, face->owned.size(), f) == face->owned.size() &&
				face->Init(face->owned.data(), face->owned.size()))
				g_faces.extra.push_back(std::move(face));
		}
		fclose(f);
	}
	g_glyphs.clear();
}
} // namespace mpc
