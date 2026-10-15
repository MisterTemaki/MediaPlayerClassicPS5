// Media Player Classic PS5: a small GDI (mpc_gdi.h).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_gdi.h"

#include <cmath>
#include <cstring>

namespace mpc
{
namespace
{
inline uint32_t Px(COLORREF c)
{
	return 0xff000000u | (c & 0xffffff);
}

// dst = src (premultiplied, a 0..255) over dst
inline uint32_t Over(uint32_t dst, uint32_t src)
{
	const uint32_t a = src >> 24;
	if (a == 255)
		return src;
	if (a == 0)
		return dst;
	const uint32_t ia = 255 - a;
	const uint32_t rb = (((dst & 0x00ff00ffu) * ia + 0x00800080u) >> 8) & 0x00ff00ffu;
	const uint32_t g = (((dst & 0x0000ff00u) * ia + 0x00008000u) >> 8) & 0x0000ff00u;
	uint32_t r = (src & 0x00ffffffu) + rb + g;
	return 0xff000000u | r;
}

inline uint32_t Mix(uint32_t dst, COLORREF c, uint32_t a) // a 0..255, c straight
{
	const uint32_t ia = 255 - a;
	const uint32_t r = ((dst & 0xff) * ia + (c & 0xff) * a + 127) / 255;
	const uint32_t g = (((dst >> 8) & 0xff) * ia + ((c >> 8) & 0xff) * a + 127) / 255;
	const uint32_t b = (((dst >> 16) & 0xff) * ia + ((c >> 16) & 0xff) * a + 127) / 255;
	return 0xff000000u | r | (g << 8) | (b << 16);
}
} // namespace

void CDC::FillSolidRect(const CRect& rr, COLORREF c)
{
	CRect r = rr;
	if (!bmp_.Valid() || !r.Intersect(clip_))
		return;
	const uint32_t p = Px(c);
	for (int y = r.top; y < r.bottom; y++)
	{
		uint32_t* row = bmp_.px + size_t(y) * bmp_.pitch;
		std::fill(row + r.left, row + r.right, p);
	}
}

void CDC::BlendRect(const CRect& rr, COLORREF c, int alpha)
{
	CRect r = rr;
	if (!bmp_.Valid() || !r.Intersect(clip_) || alpha <= 0)
		return;
	for (int y = r.top; y < r.bottom; y++)
	{
		uint32_t* row = bmp_.px + size_t(y) * bmp_.pitch;
		for (int x = r.left; x < r.right; x++)
			row[x] = Mix(row[x], c, uint32_t(std::min(alpha, 255)));
	}
}

void CDC::Draw3dRect(const CRect& r, COLORREF tl, COLORREF br)
{
	if (r.IsRectEmpty())
		return;
	// GDI: top and left in tl (top: left..right-1, left: top..bottom-1), right and bottom in br
	FillSolidRect(CRect(r.left, r.top, r.right - 1, r.top + 1), tl);
	FillSolidRect(CRect(r.left, r.top, r.left + 1, r.bottom - 1), tl);
	FillSolidRect(CRect(r.right - 1, r.top, r.right, r.bottom), br);
	FillSolidRect(CRect(r.left, r.bottom - 1, r.right, r.bottom), br);
}

void CDC::SetPixel(int x, int y, COLORREF c)
{
	if (bmp_.Valid() && clip_.PtInRect(x, y))
		bmp_.px[size_t(y) * bmp_.pitch + x] = Px(c);
}

COLORREF CDC::GetPixel(int x, int y) const
{
	if (!bmp_.Valid() || x < 0 || y < 0 || x >= bmp_.w || y >= bmp_.h)
		return 0;
	return bmp_.px[size_t(y) * bmp_.pitch + x] & 0xffffff;
}

void CDC::LineTo(int x1, int y1, COLORREF c)
{
	int x0 = pen_.x, y0 = pen_.y;
	pen_ = {x1, y1};
	const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;)
	{
		if (x0 == x1 && y0 == y1)
			break; // GDI leaves the end point out
		SetPixel(x0, y0, c);
		const int e2 = 2 * err;
		if (e2 >= dy)
		{
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx)
		{
			err += dx;
			y0 += sy;
		}
	}
}

void CDC::DrawFocusRect(const CRect& r)
{
	for (int x = r.left; x < r.right; x += 2)
	{
		SetPixel(x, r.top, GetPixel(x, r.top) ^ 0xffffff);
		SetPixel(x, r.bottom - 1, GetPixel(x, r.bottom - 1) ^ 0xffffff);
	}
	for (int y = r.top + 2; y < r.bottom - 1; y += 2)
	{
		SetPixel(r.left, y, GetPixel(r.left, y) ^ 0xffffff);
		SetPixel(r.right - 1, y, GetPixel(r.right - 1, y) ^ 0xffffff);
	}
}

void CDC::AlphaBlit(int x, int y, const Bitmap& src, int opacity)
{
	if (!bmp_.Valid() || !src.Valid())
		return;
	CRect r(x, y, x + src.w, y + src.h);
	if (!r.Intersect(clip_))
		return;
	for (int yy = r.top; yy < r.bottom; yy++)
	{
		uint32_t* d = bmp_.px + size_t(yy) * bmp_.pitch;
		const uint32_t* s = src.px + size_t(yy - y) * src.pitch - x;
		for (int xx = r.left; xx < r.right; xx++)
		{
			uint32_t p = s[xx];
			if (opacity < 255)
			{
				const uint32_t a = opacity;
				p = ((((p & 0x00ff00ffu) * a) >> 8) & 0x00ff00ffu) | ((((p >> 8) & 0x00ff00ffu) * a) & 0xff00ff00u);
			}
			d[xx] = Over(d[xx], p);
		}
	}
}

void CDC::BitBlt(int x, int y, const Bitmap& src)
{
	if (!bmp_.Valid() || !src.Valid())
		return;
	CRect r(x, y, x + src.w, y + src.h);
	if (!r.Intersect(clip_))
		return;
	for (int yy = r.top; yy < r.bottom; yy++)
		memcpy(bmp_.px + size_t(yy) * bmp_.pitch + r.left, src.px + size_t(yy - y) * src.pitch + (r.left - x),
			size_t(r.Width()) * 4);
}

void CDC::StretchBlt(const CRect& dst, const Bitmap& src)
{
	if (!bmp_.Valid() || !src.Valid() || dst.IsRectEmpty())
		return;
	CRect r = dst;
	if (!r.Intersect(clip_))
		return;
	const double sx = double(src.w) / dst.Width(), sy = double(src.h) / dst.Height();
	for (int y = r.top; y < r.bottom; y++)
	{
		double fy = (y - dst.top + 0.5) * sy - 0.5;
		fy = std::max(0.0, std::min(fy, double(src.h - 1)));
		const int y0 = int(fy), y1 = std::min(y0 + 1, src.h - 1);
		const uint32_t wy = uint32_t((fy - y0) * 256);
		uint32_t* d = bmp_.px + size_t(y) * bmp_.pitch;
		for (int x = r.left; x < r.right; x++)
		{
			double fx = (x - dst.left + 0.5) * sx - 0.5;
			fx = std::max(0.0, std::min(fx, double(src.w - 1)));
			const int x0 = int(fx), x1 = std::min(x0 + 1, src.w - 1);
			const uint32_t wx = uint32_t((fx - x0) * 256);
			const uint32_t* r0 = src.px + size_t(y0) * src.pitch;
			const uint32_t* r1 = src.px + size_t(y1) * src.pitch;
			uint32_t out = 0;
			for (int sh = 0; sh < 32; sh += 8)
			{
				const uint32_t a = (r0[x0] >> sh) & 0xff, b = (r0[x1] >> sh) & 0xff;
				const uint32_t c = (r1[x0] >> sh) & 0xff, e = (r1[x1] >> sh) & 0xff;
				const uint32_t top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + e * wx;
				out |= (((top * (256 - wy) + bot * wy) >> 16) & 0xff) << sh;
			}
			d[x] = Over(d[x], out);
		}
	}
}

void CDC::MaskBlit(int x, int y, const uint8_t* mask, int w, int h, int pitch, COLORREF c, int opacity)
{
	if (!bmp_.Valid() || !mask)
		return;
	CRect r(x, y, x + w, y + h);
	if (!r.Intersect(clip_))
		return;
	for (int yy = r.top; yy < r.bottom; yy++)
	{
		uint32_t* d = bmp_.px + size_t(yy) * bmp_.pitch;
		const uint8_t* m = mask + size_t(yy - y) * pitch - x;
		for (int xx = r.left; xx < r.right; xx++)
		{
			uint32_t a = m[xx];
			if (!a)
				continue;
			if (opacity < 255)
				a = a * uint32_t(opacity) / 255;
			d[xx] = a >= 255 ? Px(c) : Mix(d[xx], c, a);
		}
	}
}

// ---- Image ----
void Image::Create(int w, int h)
{
	w_ = std::max(w, 0);
	h_ = std::max(h, 0);
	data_.assign(size_t(w_) * h_, 0);
}

void Image::Clear(uint32_t px)
{
	std::fill(data_.begin(), data_.end(), px);
}

void Image::FromRGBA(const uint8_t* rgba, int w, int h)
{
	Create(w, h);
	for (size_t i = 0; i < data_.size(); i++)
	{
		const uint32_t a = rgba[i * 4 + 3];
		const uint32_t r = (rgba[i * 4] * a + 127) / 255, g = (rgba[i * 4 + 1] * a + 127) / 255,
					   b = (rgba[i * 4 + 2] * a + 127) / 255;
		data_[i] = r | (g << 8) | (b << 16) | (a << 24);
	}
}

Image Image::Scaled(int w, int h) const
{
	Image out(w, h);
	if (IsNull() || w <= 0 || h <= 0)
		return out;
	// area average when shrinking, bilinear when growing (STRETCH_HALFTONE's look)
	const double sx = double(w_) / w, sy = double(h_) / h;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			double acc[4] = {0, 0, 0, 0}, wsum = 0;
			if (sx > 1.0 || sy > 1.0)
			{
				const double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
				for (int yy = int(y0); yy < std::min(int(std::ceil(y1)), h_); yy++)
				{
					const double wy = std::min(y1, yy + 1.0) - std::max(y0, double(yy));
					for (int xx = int(x0); xx < std::min(int(std::ceil(x1)), w_); xx++)
					{
						const double wgt = wy * (std::min(x1, xx + 1.0) - std::max(x0, double(xx)));
						const uint32_t p = data_[size_t(yy) * w_ + xx];
						for (int c = 0; c < 4; c++)
							acc[c] += wgt * ((p >> (c * 8)) & 0xff);
						wsum += wgt;
					}
				}
			}
			else
			{
				double fx = (x + 0.5) * sx - 0.5, fy = (y + 0.5) * sy - 0.5;
				fx = std::max(0.0, std::min(fx, w_ - 1.0));
				fy = std::max(0.0, std::min(fy, h_ - 1.0));
				const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, w_ - 1), y1 = std::min(y0 + 1, h_ - 1);
				const double ax = fx - x0, ay = fy - y0;
				const uint32_t q[4] = {data_[size_t(y0) * w_ + x0], data_[size_t(y0) * w_ + x1], data_[size_t(y1) * w_ + x0],
					data_[size_t(y1) * w_ + x1]};
				const double wq[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
				for (int k = 0; k < 4; k++)
					for (int c = 0; c < 4; c++)
						acc[c] += wq[k] * ((q[k] >> (c * 8)) & 0xff);
				wsum = 1;
			}
			uint32_t p = 0;
			for (int c = 0; c < 4; c++)
				p |= uint32_t(std::min(255.0, std::max(0.0, acc[c] / (wsum > 0 ? wsum : 1) + 0.5))) << (c * 8);
			out.data_[size_t(y) * w + x] = p;
		}
	return out;
}

Image Image::Grayed() const
{
	// ImageGrayer::Gray: saturation to 0 (HLS: L = (max + min) / 2), brightness * 1.5 (0 becomes 128)
	Image out = *this;
	for (uint32_t& p : out.data_)
	{
		const uint32_t a = p >> 24;
		if (!a)
			continue;
		// back to straight colour
		const int r = int(((p & 0xff) * 255 + a / 2) / a), g = int((((p >> 8) & 0xff) * 255 + a / 2) / a),
				  b = int((((p >> 16) & 0xff) * 255 + a / 2) / a);
		const int l = (std::max({r, g, b}) + std::min({r, g, b})) / 2;
		const int adj = l == 0 ? 128 : std::min(int(std::lround(l * 1.5)), 255);
		const uint32_t v = uint32_t(adj) * a / 255;
		p = v | (v << 8) | (v << 16) | (a << 24);
	}
	return out;
}
} // namespace mpc
