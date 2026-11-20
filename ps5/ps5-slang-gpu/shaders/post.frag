// ps5-slang-gpu 1.2 (Media Player Classic PS5 1.7): the video post-processing's last pass, a fragment shader in the
// frame's render pass under the UI surface (docs/POST-PROCESSING.md; the quad: video.vert). Each pixel of the picture's
// part on the display, from the image the compute passes made (the scaler's at the display's size, or the picture's own
// when it is shown at its size), in this order:
//   1. FSR 1's RCAS sharpening when FSR 1 scaled it (third_party/ffx-fsr1, MIT; the strength in stops);
//   2. to the display: tone mapping of an HDR picture to SDR (BT.2390, Hable, Reinhard: as video.frag), or HDR10 output;
//   3. MPC-HC's colour controls (brightness, contrast, saturation) on the result's codes.
//
// SPDX-License-Identifier: MIT
#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"

layout(location = 0) in vec2 Tc;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 0) uniform sampler2D Img; // A2B10G10R10, read with texelFetch (its part pc.tone.yz)

layout(push_constant) uniform Pc
{
	layout(offset = 16) ivec4 area; // the image's corner on the display (x, y), flags (1 RCAS, 2 HDR10 out), transfer
	ivec4 tone;                     // the curve (0 off, 1 BT.2390, 2 Hable, 3 Reinhard); the image's part read (w, h), 0
	vec4 color;                     // brightness (-1..1), contrast (0..2), saturation (0..2), 0
	uvec4 rcas;                     // FsrRcasCon's
	vec4 tm;                        // the curve's top (nits), the white (nits), 1 / white, 0
	vec4 pq;                        // BT.2390: PqInv(top), PqInv(white) / PqInv(top), ks, 0
}
pc;

ivec2 gSize; // the part of the image the passes wrote (it can be larger: made once at the display's size)

#define A_GPU 1
#define A_GLSL 1
#ifdef POST_FP16
#define A_HALF 1
#endif
#include "../third_party/ffx-fsr1/ffx_a.h"
#ifdef POST_FP16
#define FSR_RCAS_H 1
AH4 FsrRcasLoadH(ASW2 p) { return AH4(texelFetch(Img, clamp(ivec2(p), ivec2(0), gSize - 1), 0)); }
void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}
#else
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(Img, clamp(ivec2(p), ivec2(0), gSize - 1), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif
#include "../third_party/ffx-fsr1/ffx_fsr1.h"

void main()
{
	gSize = max(min(pc.tone.yz, textureSize(Img, 0)), ivec2(1));
	const ivec2 p = clamp(ivec2(gl_FragCoord.xy) - pc.area.xy, ivec2(0), gSize - 1);
	vec3 c;
	if ((pc.area.z & 1) != 0)
	{
#ifdef POST_FP16
		AH1 r, g, b;
		FsrRcasH(r, g, b, AU2(p), pc.rcas);
		c = vec3(r, g, b);
#else
		AF1 r, g, b;
		FsrRcasF(r, g, b, AU2(p), pc.rcas);
		c = vec3(r, g, b);
#endif
		c = clamp(c, 0.0, 1.0);
	}
	else
		c = texelFetch(Img, p, 0).rgb;
	const int transfer = pc.area.w;
	if ((pc.area.z & 2) != 0)
		c = PostToHdr10(c, transfer);
	else if (transfer != kPostSdr && pc.tone.x != kPostTmOff)
		c = PostToneMap(c, transfer, pc.tone.x, pc.tm, pc.pq);
	c = PostColor(c, pc.color.x, pc.color.y, pc.color.z);
	FragColor = vec4(c, 1.0);
}
