// ps5-slang-gpu 1.2 (Media Player Classic PS5 1.7): what the video post-processing passes share (docs/POST-PROCESSING.md
// in the player's repository). Included by prep.comp, pyr.comp, flow.comp, warp.comp, scale.comp, easu.comp and
// post.frag (glslangValidator -I shaders).
//
// POST_FP16 (the FP16 build of a pass, used when the device has shaderFloat16 and shaderInt16): `hf` and friends are
// float16_t, else float. Only arithmetic is half: images are read and written as float.
//
// SPDX-License-Identifier: MIT
#ifndef POST_COMMON_GLSL
#define POST_COMMON_GLSL

#ifdef POST_FP16
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#define hf float16_t
#define hf2 f16vec2
#define hf3 f16vec3
#define hf4 f16vec4
#else
#define hf float
#define hf2 vec2
#define hf3 vec3
#define hf4 vec4
#endif

// BT.709 luma (for a picture's motion and its banding: the exact weights don't matter there)
const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

// Integer hash (a pixel's own random numbers, the same each run: the tests compare pictures)
uint PostHash(uvec3 v)
{
	uint h = v.x * 0x8da6b343u ^ v.y * 0xd8163841u ^ v.z * 0xcb1ab31fu;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return h;
}

float PostRand(uvec3 v) // 0..1
{
	return float(PostHash(v) >> 8) * (1.0 / 16777216.0);
}

// ---- transfer functions and tone mapping: the same numbers as video.frag (and the player's CPU path) ----------------
float PostPqEotf(float e) // signal 0..1 -> nits
{
	const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const float p = pow(max(e, 0.0), 1.0 / m2);
	return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float PostPqInv(float nits) // nits -> signal 0..1
{
	const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	const float y = pow(max(nits, 0.0) / 10000.0, m1);
	return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float PostHlgInvOetf(float e) // signal 0..1 -> scene light 0..1
{
	const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
	return e <= 0.5 ? e * e / 3.0 : (exp((e - c) / a) + b) / 12.0;
}

float PostHable(float x)
{
	const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
	return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F;
}

const int kPostSdr = 0, kPostPq = 1, kPostHlg = 2;
const int kPostTmOff = 0, kPostBt2390 = 1, kPostHable = 2, kPostReinhard = 3;

// nits in -> nits out, from [0, top] to [0, white]; tm: top, white, 1 / white; pq: BT.2390's PqInv(top),
// PqInv(white) / PqInv(top), ks
float PostCurve(float L, int curve, vec4 tm, vec4 pq)
{
	const float top = tm.x, white = tm.y;
	if (L <= 0.0)
		return 0.0;
	if (top <= white)
		return L;
	if (curve == kPostHable)
		return white * PostHable(L / white * 2.0) / PostHable(top / white * 2.0);
	if (curve == kPostReinhard)
	{
		const float x = L / white, lw2 = (top / white) * (top / white);
		return white * x * (1.0 + x / lw2) / (1.0 + x);
	}
	const float src = pq.x, max_lum = pq.y, ks = pq.z;
	const float e1 = PostPqInv(L) / src;
	float e2 = e1;
	if (e1 > ks && ks < 1.0)
	{
		const float t = (e1 - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;
		e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) + (-2.0 * t3 + 3.0 * t2) * max_lum;
	}
	return PostPqEotf(min(e2, 1.0) * src);
}

// an HDR signal (PQ or HLG, BT.2020) to SDR R'G'B' (BT.709, gamma 2.4), as video.frag's ToneMap
vec3 PostToneMap(vec3 e, int transfer, int curve, vec4 tm, vec4 pq)
{
	vec3 lin;
	if (transfer == kPostHlg)
	{
		lin = vec3(PostHlgInvOetf(e.r), PostHlgInvOetf(e.g), PostHlgInvOetf(e.b));
		const float ys = dot(lin, vec3(0.2627, 0.6780, 0.0593));
		lin *= 1000.0 * pow(clamp(ys, 0.0, 1.0), 0.2);
	}
	else
		lin = vec3(PostPqEotf(e.r), PostPqEotf(e.g), PostPqEotf(e.b));
	vec3 c = vec3(dot(vec3(1.6605, -0.5876, -0.0728), lin), dot(vec3(-0.1246, 1.1329, -0.0083), lin),
		dot(vec3(-0.0182, -0.1006, 1.1187), lin));
	c = max(c, vec3(0.0));
	const float mx = min(max(c.r, max(c.g, c.b)), tm.x);
	const float gain = mx > 0.0 ? PostCurve(mx, curve, tm, pq) / mx * tm.z : tm.z;
	c = min(c * gain, vec3(1.0));
	return pow(c, vec3(1.0 / 2.4));
}

// to the native HDR10 output (PQ, BT.2020): PQ kept; HLG and SDR to absolute light, then PQ (as video.frag)
vec3 PostToHdr10(vec3 rgb, int transfer)
{
	if (transfer == kPostPq)
		return rgb;
	vec3 light;
	if (transfer == kPostHlg)
	{
		light = vec3(PostHlgInvOetf(rgb.r), PostHlgInvOetf(rgb.g), PostHlgInvOetf(rgb.b));
		light *= 1000.0 * pow(max(dot(light, vec3(0.2627, 0.6780, 0.0593)), 0.0), 0.2);
	}
	else
	{
		light = pow(rgb, vec3(2.4)) * 203.0;
		light = vec3(dot(light, vec3(0.6274, 0.3293, 0.0433)), dot(light, vec3(0.0691, 0.9195, 0.0114)),
			dot(light, vec3(0.0164, 0.0880, 0.8956)));
	}
	return vec3(PostPqInv(light.r), PostPqInv(light.g), PostPqInv(light.b));
}

// MPC-HC's colour controls on R'G'B' (codes, 0..1): brightness -1..1 (added), contrast 0..2 and saturation 0..2
// (times; 1 leaves the picture as it is), around mid grey and the pixel's own luma
vec3 PostColor(vec3 c, float brightness, float contrast, float saturation)
{
	const float y = dot(c, kLuma);
	c = mix(vec3(y), c, saturation);
	c = (c - 0.5) * contrast + 0.5 + brightness;
	return clamp(c, 0.0, 1.0);
}

#endif
