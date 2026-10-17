// Media Player Classic PS5: ASS/SSA subtitles drawn by libass, as MPC-HC draws them with its own VSFilter-derived
// renderer (positions, \an and margins, the script's styles and fonts, colours, borders and shadows, rotation,
// karaoke and the other timed effects, PlayResX/Y scaled to the picture).
//
// One Renderer per opened file: an ASS_Library and an ASS_Renderer, made the first time an ASS/SSA track turns up,
// with one ASS_Track per subtitle track (embedded: the codec private header and each packet as it is demuxed;
// external .ass/.ssa: the whole script). Fonts (there is no fontconfig on the console, so no system font provider):
//   - the file's attached fonts (Matroska attachments);
//   - the fonts folder (/data/mpcps5/fonts: OrbisDir("fonts"), the same as mpc_font.h's extra fonts), the first of
//     them also being the last resort for characters the default family has not got;
//   - the app's own fonts, built in (Selawik, Selawik Bold, Roboto); Selawik, the subtitles' font until now, is the
//     default family for every font name the script asks for and nobody has.
// Every call is thread-safe (the demuxer adds events while the UI thread renders).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mpc_gdi.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mpc
{
namespace ass
{
class Renderer
{
public:
	Renderer();
	~Renderer();
	Renderer(const Renderer&) = delete;
	Renderer& operator=(const Renderer&) = delete;

	// a font from the file (an attachment): before or after the tracks, used from the next frame on
	void AddFont(const std::string& name, const uint8_t* data, size_t size);
	// an embedded track: its codec private data (the script's header: [Script Info], styles, the events' format)
	void NewTrack(int id, const uint8_t* header, size_t size);
	// an external script (UTF-8, whole); false when libass finds no events in it
	bool ReadScript(int id, const std::vector<uint8_t>& script);
	bool HasTrack(int id) const;
	// one packet of an embedded track: "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text" (Matroska's
	// form); the same ReadOrder again (a seek replays packets) is ignored
	void AddEvent(int id, const uint8_t* data, size_t size, int64_t start_ms, int64_t duration_ms);

	// The track's picture at now_ms, for a picture shown at frame_w x frame_h (storage_w x storage_h: the video's
	// own size, for the aspect ratio), only the part inside clip (in the frame's coordinates) composed: a
	// premultiplied image to blend at (*x, *y) in the frame's coordinates, or nullptr when nothing shows. *changed
	// is false when it is the same as the last call's (libass's change detection: then nothing is blended again).
	// For one thread (the UI's); the image stays valid until the next call.
	const Image* Render(int id, int64_t now_ms, int frame_w, int frame_h, int storage_w, int storage_h, int clip_l,
		int clip_t, int clip_r, int clip_b, int* x, int* y, bool* changed);

	struct Impl;

private:
	std::unique_ptr<Impl> m;
};

// libass's version ("0.17.5"), for the About box and the logs
std::string Version();
} // namespace ass
} // namespace mpc
