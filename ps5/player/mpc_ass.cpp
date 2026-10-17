// Media Player Classic PS5: ASS/SSA subtitles drawn by libass (mpc_ass.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_ass.h"

#include "mpc_res.h"

#include "OrbisPaths.h"

extern "C" {
#include <ass/ass.h>
}

#include <dirent.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace mpc
{
namespace ass
{
namespace
{
// libass's errors, warnings and notes (which font each style got, a glyph no font has) into the log; its
// debug lines are left out
void Message(int level, const char* fmt, va_list args, void*)
{
	if (level > 4) // MSGL_INFO
		return;
	char line[512];
	vsnprintf(line, sizeof(line), fmt, args);
	OrbisLog("[libass] %s", line);
}

bool FontFile(const std::string& name)
{
	std::string low = name;
	for (char& c : low)
		c = char(tolower(static_cast<unsigned char>(c)));
	return low.size() > 4 && (low.compare(low.size() - 4, 4, ".ttf") == 0 || low.compare(low.size() - 4, 4, ".otf") == 0 ||
								 low.compare(low.size() - 4, 4, ".ttc") == 0);
}

// the fonts folder's font files, sorted (mpc_font.cpp's LoadExtraFonts takes them in the same order)
std::vector<std::string> FontFiles(const std::string& dir)
{
	std::vector<std::string> out;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (dirent* e = readdir(d))
			if (FontFile(e->d_name))
				out.push_back(dir + "/" + e->d_name);
		closedir(d);
	}
	std::sort(out.begin(), out.end());
	return out;
}

inline uint32_t Div255(uint32_t v)
{
	return (v + 1 + (v >> 8)) >> 8; // v / 255, rounded, for v <= 255 * 255
}
} // namespace

struct Renderer::Impl
{
	std::mutex mu;
	ASS_Library* lib = nullptr;
	ASS_Renderer* rnd = nullptr;
	std::map<int, ASS_Track*> tracks;
	// the last picture
	Image img;
	int img_x = 0, img_y = 0;
	bool img_empty = true;
	int last_id = -1, last_fw = 0, last_fh = 0, last_sw = 0, last_sh = 0;
	int last_clip[4] = {0, 0, 0, 0};

	ASS_Track* Track(int id)
	{
		auto it = tracks.find(id);
		return it == tracks.end() ? nullptr : it->second;
	}

	// ASS_Image list -> one premultiplied picture (0xAABBGGRR), only inside the clip
	void Compose(const ASS_Image* list, int cl, int ct, int cr, int cb)
	{
		int l = INT32_MAX, t = INT32_MAX, r = INT32_MIN, b = INT32_MIN;
		for (const ASS_Image* i = list; i; i = i->next)
		{
			if (i->w <= 0 || i->h <= 0 || (i->color & 0xff) == 0xff)
				continue;
			l = std::min(l, i->dst_x);
			t = std::min(t, i->dst_y);
			r = std::max(r, i->dst_x + i->w);
			b = std::max(b, i->dst_y + i->h);
		}
		l = std::max(l, cl);
		t = std::max(t, ct);
		r = std::min(r, cr);
		b = std::min(b, cb);
		if (r <= l || b <= t)
		{
			img_empty = true;
			return;
		}
		img_empty = false;
		img_x = l;
		img_y = t;
		const int w = r - l, h = b - t;
		if (img.Width() == w && img.Height() == h)
			img.Clear(0);
		else
			img.Create(w, h);
		uint32_t* px = img.Data();
		for (const ASS_Image* i = list; i; i = i->next)
		{
			if (i->w <= 0 || i->h <= 0)
				continue;
			// colour: 0xRRGGBBAA, AA the transparency (0 opaque)
			const uint32_t cr_ = i->color >> 24, cg = (i->color >> 16) & 0xff, cbl = (i->color >> 8) & 0xff;
			const uint32_t opacity = 255 - (i->color & 0xff);
			if (opacity == 0)
				continue;
			const int x0 = std::max(i->dst_x, l), x1 = std::min(i->dst_x + i->w, r);
			const int y0 = std::max(i->dst_y, t), y1 = std::min(i->dst_y + i->h, b);
			for (int y = y0; y < y1; y++)
			{
				const uint8_t* src = i->bitmap + size_t(y - i->dst_y) * i->stride - i->dst_x;
				uint32_t* dst = px + size_t(y - t) * w - l;
				for (int x = x0; x < x1; x++)
				{
					const uint32_t cov = src[x];
					if (cov == 0)
						continue;
					const uint32_t k = Div255(cov * opacity); // this layer's alpha here
					const uint32_t d = dst[x];
					const uint32_t ik = 255 - k;
					// premultiplied "over": the colour times k, plus what is there times (1 - k)
					const uint32_t nr = Div255(cr_ * k) + Div255((d & 0xff) * ik);
					const uint32_t ng = Div255(cg * k) + Div255(((d >> 8) & 0xff) * ik);
					const uint32_t nb = Div255(cbl * k) + Div255(((d >> 16) & 0xff) * ik);
					const uint32_t na = k + Div255((d >> 24) * ik);
					dst[x] = std::min(nr, 255u) | (std::min(ng, 255u) << 8) | (std::min(nb, 255u) << 16) |
							 (std::min(na, 255u) << 24);
				}
			}
		}
	}
};

Renderer::Renderer() : m(new Impl)
{
	m->lib = ass_library_init();
	if (!m->lib)
	{
		OrbisLog("[subs] libass: can't start");
		return;
	}
	ass_set_message_cb(m->lib, Message, nullptr);
	ass_set_extract_fonts(m->lib, 1);
	// the app's own fonts
	const Blob fonts[] = {res::FontSelawik(), res::FontSelawikBold(), res::FontRoboto()};
	const char* const names[] = {"Selawik-Regular.ttf", "Selawik-Bold.ttf", "Roboto-Regular.ttf"};
	for (int i = 0; i < 3; i++)
		ass_add_font(m->lib, names[i], reinterpret_cast<const char*>(fonts[i].data), int(fonts[i].size));
	// the fonts folder: libass reads it in itself (every font in it, as an embedded font)
	const std::string dir = OrbisDir("fonts");
	const std::vector<std::string> extra = FontFiles(dir);
	if (!extra.empty())
		ass_set_fonts_dir(m->lib, dir.c_str());
	m->rnd = ass_renderer_init(m->lib);
	if (!m->rnd)
	{
		OrbisLog("[subs] libass: no renderer");
		return;
	}
	// no font provider on the console (no fontconfig): the fonts above only; a name nobody has is drawn in
	// Selawik, and a character Selawik hasn't got in the fonts folder's first font
	ass_set_fonts(m->rnd, extra.empty() ? nullptr : extra.front().c_str(), "Selawik", ASS_FONTPROVIDER_NONE, nullptr, 0);
	ass_set_hinting(m->rnd, ASS_HINTING_NONE);
	ass_set_shaper(m->rnd, ASS_SHAPING_COMPLEX);
	ass_set_cache_limits(m->rnd, 0, 64); // glyphs: the default; bitmaps: 64 MB
	OrbisLog("[subs] libass %s ready: %zu font(s) in %s", Version().c_str(), extra.size(), dir.c_str());
}

Renderer::~Renderer()
{
	for (auto& t : m->tracks)
		ass_free_track(t.second);
	if (m->rnd)
		ass_renderer_done(m->rnd);
	if (m->lib)
		ass_library_done(m->lib);
}

void Renderer::AddFont(const std::string& name, const uint8_t* data, size_t size)
{
	std::lock_guard<std::mutex> l(m->mu);
	if (!m->lib || !data || size == 0 || size > (64u << 20))
		return;
	ass_add_font(m->lib, name.c_str(), reinterpret_cast<const char*>(data), int(size));
}

void Renderer::NewTrack(int id, const uint8_t* header, size_t size)
{
	std::lock_guard<std::mutex> l(m->mu);
	if (!m->lib || m->Track(id))
		return;
	ASS_Track* t = ass_new_track(m->lib);
	if (!t)
		return;
	if (header && size > 0)
		ass_process_codec_private(t, reinterpret_cast<const char*>(header), int(size));
	m->tracks[id] = t;
}

bool Renderer::ReadScript(int id, const std::vector<uint8_t>& script)
{
	std::lock_guard<std::mutex> l(m->mu);
	if (!m->lib || script.empty())
		return false;
	std::vector<char> buf(script.begin(), script.end());
	ASS_Track* t = ass_read_memory(m->lib, buf.data(), buf.size(), nullptr);
	if (!t)
		return false;
	if (t->n_events == 0)
	{
		ass_free_track(t);
		return false;
	}
	if (ASS_Track* old = m->Track(id))
		ass_free_track(old);
	m->tracks[id] = t;
	return true;
}

bool Renderer::HasTrack(int id) const
{
	std::lock_guard<std::mutex> l(m->mu);
	return m->Track(id) != nullptr;
}

void Renderer::AddEvent(int id, const uint8_t* data, size_t size, int64_t start_ms, int64_t duration_ms)
{
	std::lock_guard<std::mutex> l(m->mu);
	ASS_Track* t = m->Track(id);
	if (!t || !data || size == 0)
		return;
	ass_process_chunk(t, reinterpret_cast<const char*>(data), int(size), start_ms, duration_ms);
}

const Image* Renderer::Render(int id, int64_t now_ms, int frame_w, int frame_h, int storage_w, int storage_h,
	int clip_l, int clip_t, int clip_r, int clip_b, int* x, int* y, bool* changed)
{
	std::lock_guard<std::mutex> l(m->mu);
	*changed = false;
	ASS_Track* t = m->Track(id);
	if (!t || !m->rnd || frame_w <= 0 || frame_h <= 0)
	{
		*changed = !m->img_empty;
		m->img_empty = true;
		m->last_id = -1;
		return nullptr;
	}
	if (storage_w <= 0 || storage_h <= 0)
	{
		storage_w = frame_w;
		storage_h = frame_h;
	}
	const bool resized = frame_w != m->last_fw || frame_h != m->last_fh || storage_w != m->last_sw || storage_h != m->last_sh;
	if (resized)
	{
		ass_set_frame_size(m->rnd, frame_w, frame_h);
		ass_set_storage_size(m->rnd, storage_w, storage_h);
		m->last_fw = frame_w;
		m->last_fh = frame_h;
		m->last_sw = storage_w;
		m->last_sh = storage_h;
	}
	const bool reclip = clip_l != m->last_clip[0] || clip_t != m->last_clip[1] || clip_r != m->last_clip[2] ||
						clip_b != m->last_clip[3];
	int detect = 0;
	ASS_Image* list = ass_render_frame(m->rnd, t, now_ms, &detect);
	if (detect != 0 || resized || reclip || id != m->last_id)
	{
		m->Compose(list, clip_l, clip_t, clip_r, clip_b);
		m->last_id = id;
		m->last_clip[0] = clip_l;
		m->last_clip[1] = clip_t;
		m->last_clip[2] = clip_r;
		m->last_clip[3] = clip_b;
		*changed = true;
	}
	if (m->img_empty)
		return nullptr;
	*x = m->img_x;
	*y = m->img_y;
	return &m->img;
}

std::string Version()
{
	const int v = ass_library_version(); // the digits in hex: 0x01705000 is 0.17.5
	char s[32];
	snprintf(s, sizeof(s), "%x.%x.%x", (v >> 28) & 0xf, (v >> 20) & 0xff, (v >> 12) & 0xff);
	return s;
}
} // namespace ass
} // namespace mpc
