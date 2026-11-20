#version 450
#extension GL_EXT_nonuniform_qualifier : require
// The GX TEV (texture environment) as an uber-shader: every stage's configuration comes from the
// draw's state (BP registers, packed by gx_vk.cpp), with the hardware's integer math (after
// Dolphin's UberShaderPixel, GPL-2.0-or-later): 8-bit inputs, 11-bit signed registers, bias, scale,
// clamp, compare modes, swap tables, konst selection, alpha test and fog.
// Indirect textures as Dolphin's shaders do them: texture coordinates in fixed point (1/128 texel of
// the coordinate's scale), up to 4 indirect stages whose samples (alpha, blue, green) offset the TEV
// stages' coordinates through a matrix. Not yet: Z textures, the bump alpha raster colors.

layout(location = 0) in vec4 col0;
layout(location = 1) in vec4 col1;
layout(location = 2) in vec3 tc[8];
layout(location = 0, index = 0) out vec4 outColor;
#ifdef DUAL_SRC
// (dual-source blending, as Dolphin's shaders: with the destination alpha constant on (BP 0x42) the EFB
// gets that constant as its alpha while blending still uses the TEV's alpha - this second output, which
// the pipeline's SRC1 factors read. SSPE52 marks nearly every draw's glow / depth-of-field weight this way,
// translucent ones included: without it they wrote their own alpha and lit up in the glow pass.)
layout(location = 0, index = 1) out vec4 outBlend;
#endif

struct State
{
	uint genmode, alphaTest, flags, pad0;
	uint tref[4];
	uint cenv[16];
	uint aenv[16];
	uint ksel[8];
	ivec4 reg[4];   // PREV, C0, C1, C2 at the start of the draw
	ivec4 konst[4];
	uint tex[8];    // per map: descriptor slot | sampler << 16; 0xFFFFFFFF: none
	vec4 suSize[8]; // xy: texture coordinate i's scale; zw: 1 / map i's size
	vec4 fog;       // A, C, B magnitude, B shift
	uint fogSel, fogColor, pad1, pad2;
	uint indCmd[16]; // BP 0x10-0x1F: bt 0-1, fmt 2-3, bias 4-6, bs 7-8, mid 9-12, sw 13-15, tw 16-18, fb_addprev 20
	uint iref, ss0, ss1, pad3;
	ivec4 indMtx[6]; // rows (ma mc me | mb md mf), w: 17 - scale
};
layout(std430, set = 0, binding = 1) readonly buffer States { State s[]; } states;
layout(set = 0, binding = 2) uniform texture2D textures[];
layout(set = 0, binding = 3) uniform sampler samplers[64];
layout(push_constant) uniform PC { uint state; uint numTex; uint pad0; uint pad1; } pc;

#define S states.s[pc.state]

ivec4 swapped(ivec4 c, uint table)
{
	const uint k0 = S.ksel[table * 2], k1 = S.ksel[table * 2 + 1];
	return ivec4(c[k0 & 3u], c[(k0 >> 2) & 3u], c[k1 & 3u], c[(k1 >> 2) & 3u]);
}

// texture coordinate i in fixed point: texels of its scale (SU size), 7 fraction bits
ivec2 fixpointUv(uint coord)
{
	const vec3 c = tc[coord];
	return ivec2((c.z != 0.0 ? c.xy / c.z : c.xy) * S.suSize[coord].xy * 128.0);
}

// map's texture at a fixed-point texel coordinate
ivec4 sampleMapAt(uint map, ivec2 uv)
{
	const uint t = S.tex[map];
	if (t == 0xFFFFFFFFu)
		return ivec4(255);
	const vec2 st = vec2(uv) * S.suSize[map].zw * (1.0 / 128.0);
	const vec4 v = texture(sampler2D(textures[nonuniformEXT(t & 0xFFFFu)], samplers[nonuniformEXT(t >> 16)]), st);
	return ivec4(round(v * 255.0));
}

ivec4 sampleMap(uint map, uint coord)
{
	return sampleMapAt(map, fixpointUv(coord));
}

// a TEV stage's texture coordinate with its indirect command applied (BP 0x10 + n): the indirect
// stage's offset (masked by format, biased) through the matrix, added to the wrapped coordinate
// (and to the previous stage's with fb_addprev)
const int kIndMask[4] = int[4](255, 31, 15, 7);
const int kIndWrap[7] = int[7](0, 256 << 7, 128 << 7, 64 << 7, 32 << 7, 16 << 7, 1);
ivec2 indirectCoord(uint n, uint coord, ivec3 indTex[4], uint numInd, inout ivec2 prevCoord)
{
	const uint cmd = S.indCmd[n];
	const ivec2 uv = fixpointUv(coord);
	const uint bt = cmd & 3u, fmt = (cmd >> 2) & 3u, bias = (cmd >> 4) & 7u, mid = (cmd >> 9) & 15u;
	const uint sw = (cmd >> 13) & 7u, tw = (cmd >> 16) & 7u;
	ivec2 trans = ivec2(0);
	if (bt < numInd && mid != 0u)
	{
		ivec3 crd = indTex[bt] & kIndMask[fmt];
		const int add = fmt == 0u ? -128 : 1;
		if ((bias & 1u) != 0u) crd.x += add;
		if ((bias & 2u) != 0u) crd.y += add;
		if ((bias & 4u) != 0u) crd.z += add;
		const uint m = (mid & 3u) - 1u;
		if (m < 3u)
		{
			const int w = S.indMtx[2u * m].w;
			if (mid <= 3u)
				trans = ivec2(dot(S.indMtx[2u * m].xyz, crd), dot(S.indMtx[2u * m + 1u].xyz, crd)) >> 3;
			else if (mid <= 7u)
				trans = (uv * crd.xx) >> 8;
			else
				trans = (uv * crd.yy) >> 8;
			trans = w >= 0 ? trans >> w : trans << (-w);
		}
	}
	ivec2 wrapped;
	wrapped.x = sw == 0u ? uv.x : sw >= 6u ? 0 : (uv.x & (kIndWrap[sw] - 1));
	wrapped.y = tw == 0u ? uv.y : tw >= 6u ? 0 : (uv.y & (kIndWrap[tw] - 1));
	ivec2 c = wrapped + trans;
	if (((cmd >> 20) & 1u) != 0u)
		c += prevCoord;
	c = (c << 8) >> 8; // (24-bit coordinates)
	prevCoord = c;
	return c;
}

const int kFraction[8] = int[8](255, 223, 191, 159, 128, 96, 64, 32);

ivec3 konstColor(uint sel)
{
	if (sel < 8u)
		return ivec3(kFraction[sel]);
	if (sel < 12u)
		return ivec3(0);
	if (sel < 16u)
		return S.konst[sel - 12u].rgb;
	return ivec3(S.konst[(sel - 16u) & 3u][(sel - 16u) >> 2]);
}
int konstAlpha(uint sel)
{
	if (sel < 8u)
		return kFraction[sel];
	if (sel < 16u)
		return 0;
	return S.konst[(sel - 16u) & 3u][(sel - 16u) >> 2];
}

ivec3 colorInput(uint sel, ivec4 r[4], ivec4 tex, ivec4 ras, ivec3 k)
{
	switch (sel)
	{
	case 0u: return r[0].rgb;
	case 1u: return r[0].aaa;
	case 2u: return r[1].rgb;
	case 3u: return r[1].aaa;
	case 4u: return r[2].rgb;
	case 5u: return r[2].aaa;
	case 6u: return r[3].rgb;
	case 7u: return r[3].aaa;
	case 8u: return tex.rgb;
	case 9u: return tex.aaa;
	case 10u: return ras.rgb;
	case 11u: return ras.aaa;
	case 12u: return ivec3(255);
	case 13u: return ivec3(128);
	case 14u: return k;
	default: return ivec3(0);
	}
}
int alphaInput(uint sel, ivec4 r[4], ivec4 tex, ivec4 ras, int k)
{
	switch (sel)
	{
	case 0u: return r[0].a;
	case 1u: return r[1].a;
	case 2u: return r[2].a;
	case 3u: return r[3].a;
	case 4u: return tex.a;
	case 5u: return ras.a;
	case 6u: return k;
	default: return 0;
	}
}

ivec3 tevLerp3(ivec3 A, ivec3 B, ivec3 C, ivec3 D, uint bias, bool op, uint shift)
{
	C += C >> 7;
	if (bias == 1u) D += 128;
	else if (bias == 2u) D -= 128;
	ivec3 lerp = (A << 8) + (B - A) * C;
	if (shift != 3u)
	{
		lerp = lerp << shift;
		D = D << shift;
	}
	if (shift != 3u) // (the rounding term: color stages round unless dividing by 2)
		lerp = lerp + (op ? 127 : 128);
	ivec3 result = lerp >> 8;
	result = op ? D - result : D + result;
	if (shift == 3u)
		result = result >> 1;
	return result;
}
int tevLerp1(int A, int B, int C, int D, uint bias, bool op, uint shift)
{
	C += C >> 7;
	if (bias == 1u) D += 128;
	else if (bias == 2u) D -= 128;
	int lerp = (A << 8) + (B - A) * C;
	if (shift != 3u)
	{
		lerp = lerp << shift;
		D = D << shift;
	}
	if (shift == 3u) // (alpha stages round only when dividing by 2)
		lerp = lerp + (op ? 127 : 128);
	int result = lerp >> 8;
	result = op ? D - result : D + result;
	if (shift == 3u)
		result = result >> 1;
	return result;
}

bool alphaCompare(int a, int ref, uint comp)
{
	switch (comp)
	{
	case 0u: return false;
	case 1u: return a < ref;
	case 2u: return a == ref;
	case 3u: return a <= ref;
	case 4u: return a > ref;
	case 5u: return a != ref;
	case 6u: return a >= ref;
	default: return true;
	}
}

void main()
{
	if (S.pad0 != 0u)
	{
		// (SSA_VK_DEBUG: 1 map 0's texture through coordinate 0, 2 color 0, 3 the texture coordinate)
		if (S.pad0 == 1u) outColor = vec4(sampleMap(0u, 0u)) / 255.0;
		else if (S.pad0 == 2u) outColor = col0;
		else outColor = vec4(fract(tc[0].xy), 0.0, 1.0);
#ifdef DUAL_SRC
		outBlend = outColor;
#endif
		return;
	}
	const uint stages = bitfieldExtract(S.genmode, 10, 4) + 1u;
	ivec4 r[4] = ivec4[4](S.reg[0], S.reg[1], S.reg[2], S.reg[3]);
	const ivec4 ras0 = ivec4(round(col0 * 255.0)), ras1 = ivec4(round(col1 * 255.0));
	// the indirect stages (genmode bits 16-18): map and coordinate from BP 0x27, the coordinate
	// scaled down by BP 0x25/0x26 (4 bits each for s and t), the sample's alpha, blue, green
	const uint numInd = min(bitfieldExtract(S.genmode, 16, 3), 4u);
	ivec3 indTex[4] = ivec3[4](ivec3(0), ivec3(0), ivec3(0), ivec3(0));
	for (uint i = 0u; i < numInd; i++)
	{
		const uint ir = S.iref >> (6u * i);
		const uint ss = (i < 2u ? S.ss0 : S.ss1) >> ((i & 1u) * 8u);
		const ivec2 uv = fixpointUv((ir >> 3) & 7u) >> ivec2(int(ss & 15u), int((ss >> 4) & 15u));
		indTex[i] = sampleMapAt(ir & 7u, uv).abg;
	}
	ivec2 prevCoord = ivec2(0);
	for (uint n = 0u; n < stages; n++)
	{
		const uint tref = S.tref[n >> 1] >> ((n & 1u) * 12u);
		const uint cc = S.cenv[n], ac = S.aenv[n];
		ivec4 tex = ivec4(255);
		if ((tref & 0x40u) != 0u)
		{
			const uint coord = (tref >> 3) & 7u;
			const ivec2 uv = S.indCmd[n] != 0u ? indirectCoord(n, coord, indTex, numInd, prevCoord) : fixpointUv(coord);
			tex = swapped(sampleMapAt(tref & 7u, uv), (ac >> 2) & 3u);
		}
		const uint rsel = (tref >> 7) & 7u;
		ivec4 ras = rsel == 0u ? ras0 : rsel == 1u ? ras1 : ivec4(0);
		ras = swapped(ras, ac & 3u);
		const uint ks = S.ksel[n >> 1] >> ((n & 1u) * 10u);
		const ivec3 kc = konstColor((ks >> 4) & 31u);
		const int ka = konstAlpha((ks >> 9) & 31u);

		// color
		const ivec3 cA = colorInput((cc >> 12) & 15u, r, tex, ras, kc) & 255;
		const ivec3 cB = colorInput((cc >> 8) & 15u, r, tex, ras, kc) & 255;
		const ivec3 cC = colorInput((cc >> 4) & 15u, r, tex, ras, kc) & 255;
		const ivec3 cD = colorInput(cc & 15u, r, tex, ras, kc);
		const uint cBias = (cc >> 16) & 3u, cShift = (cc >> 20) & 3u;
		const bool cOp = ((cc >> 18) & 1u) != 0u;
		ivec3 color;
		if (cBias != 3u)
			color = tevLerp3(cA, cB, cC, cD, cBias, cOp, cShift);
		else
		{
			const uint cmp = cShift << 1 | (cOp ? 1u : 0u);
			const int a16 = cA.g << 8 | cA.r, b16 = cB.g << 8 | cB.r;
			const int a24 = cA.b << 16 | a16, b24 = cB.b << 16 | b16;
			ivec3 sel;
			switch (cmp)
			{
			case 0u: sel = cA.r > cB.r ? cC : ivec3(0); break;
			case 1u: sel = cA.r == cB.r ? cC : ivec3(0); break;
			case 2u: sel = a16 > b16 ? cC : ivec3(0); break;
			case 3u: sel = a16 == b16 ? cC : ivec3(0); break;
			case 4u: sel = a24 > b24 ? cC : ivec3(0); break;
			case 5u: sel = a24 == b24 ? cC : ivec3(0); break;
			case 6u: sel = ivec3(cA.r > cB.r ? cC.r : 0, cA.g > cB.g ? cC.g : 0, cA.b > cB.b ? cC.b : 0); break;
			default: sel = ivec3(cA.r == cB.r ? cC.r : 0, cA.g == cB.g ? cC.g : 0, cA.b == cB.b ? cC.b : 0); break;
			}
			color = cD + sel;
		}
		color = ((cc >> 19) & 1u) != 0u ? clamp(color, 0, 255) : clamp(color, -1024, 1023);

		// alpha
		const int aA = alphaInput((ac >> 13) & 7u, r, tex, ras, ka) & 255;
		const int aB = alphaInput((ac >> 10) & 7u, r, tex, ras, ka) & 255;
		const int aC = alphaInput((ac >> 7) & 7u, r, tex, ras, ka) & 255;
		const int aD = alphaInput((ac >> 4) & 7u, r, tex, ras, ka);
		const uint aBias = (ac >> 16) & 3u, aShift = (ac >> 20) & 3u;
		const bool aOp = ((ac >> 18) & 1u) != 0u;
		int alpha;
		if (aBias != 3u)
			alpha = tevLerp1(aA, aB, aC, aD, aBias, aOp, aShift);
		else
		{
			const uint cmp = aShift << 1 | (aOp ? 1u : 0u);
			const int a16 = cA.g << 8 | cA.r, b16 = cB.g << 8 | cB.r;
			const int a24 = cA.b << 16 | a16, b24 = cB.b << 16 | b16;
			bool pass;
			switch (cmp)
			{
			case 0u: pass = cA.r > cB.r; break;
			case 1u: pass = cA.r == cB.r; break;
			case 2u: pass = a16 > b16; break;
			case 3u: pass = a16 == b16; break;
			case 4u: pass = a24 > b24; break;
			case 5u: pass = a24 == b24; break;
			case 6u: pass = aA > aB; break;
			default: pass = aA == aB; break;
			}
			alpha = aD + (pass ? aC : 0);
		}
		alpha = ((ac >> 19) & 1u) != 0u ? clamp(alpha, 0, 255) : clamp(alpha, -1024, 1023);

		r[(cc >> 22) & 3u].rgb = color;
		r[(ac >> 22) & 3u].a = alpha;
	}
	ivec4 prev = r[0] & 255;

	// alpha test (BP 0xF3: refs, compares, logic)
	const uint at = S.alphaTest;
	const bool p0 = alphaCompare(prev.a, int(at & 255u), (at >> 16) & 7u);
	const bool p1 = alphaCompare(prev.a, int((at >> 8) & 255u), (at >> 19) & 7u);
	bool pass;
	switch ((at >> 22) & 3u)
	{
	case 0u: pass = p0 && p1; break;
	case 1u: pass = p0 || p1; break;
	case 2u: pass = p0 != p1; break;
	default: pass = p0 == p1; break;
	}
	if (!pass)
		discard;

	// fog (BP 0xEE-0xF2; no range adjustment yet)
	const uint fsel = S.fogSel & 7u;
	if (fsel >= 2u)
	{
		const float zs = floor(gl_FragCoord.z * 16777216.0);
		float ze;
		if ((S.fogSel & 8u) == 0u)
			ze = S.fog.x * 16777216.0 / (S.fog.z - floor(zs / exp2(S.fog.w)));
		else
			ze = S.fog.x * zs / 16777216.0;
		ze -= S.fog.y;
		float f = clamp(ze, 0.0, 1.0);
		if (fsel == 4u) f = 1.0 - exp2(-8.0 * f);
		else if (fsel == 5u) f = 1.0 - exp2(-8.0 * f * f);
		else if (fsel == 6u) f = exp2(-8.0 * (1.0 - f));
		else if (fsel == 7u) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));
		const int fi = int(round(f * 256.0));
		const ivec3 fc = ivec3((S.fogColor >> 16) & 255u, (S.fogColor >> 8) & 255u, S.fogColor & 255u);
		prev.rgb = (prev.rgb * (256 - fi) + fc * fi) >> 8;
	}

	int outAlpha = prev.a;
	if ((S.flags & 1u) != 0u)
		outAlpha = int((S.flags >> 8) & 255u);
	outColor = vec4(prev.rgb, outAlpha) / 255.0;
#ifdef DUAL_SRC
	outBlend = vec4(prev.rgb, prev.a) / 255.0;
#endif
}
