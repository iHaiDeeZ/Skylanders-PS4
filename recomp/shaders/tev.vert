#version 450
// GX draws: the transform stage (XF) on the GPU. gx.cpp fetches the vertices (decoded to floats) into
// a storage buffer; indices are word offsets of the vertices. Word 0 of a vertex:
//   bits 0-13  the XF snapshot (gx_vk.cpp: kXfWords words each, layout below)
//   bit 16     matrix indices in the vertex (2 words: position/normal, tex 0-3 | tex 4-7, 6 bits each)
//   bits 17-18 normals: 0, 1 (normal) or 3 (normal, binormal, tangent)
//   bit 19     color 1 in the vertex format
//   bits 20-27 texture coordinates in the vertex (s, t each, in this order)
//   bit 30     dropped (a position index of all ones)
//   bit 31     transformed on the CPU (SSA_GX_CPUXF=1): clip position, colors 0 and 1 (RGBA8), then
//              numTex texture coordinates (s, t, q)
// Then position (3 floats), [matrix indices], [normals], colors 0 and 1 (RGBA8, white when missing),
// [texture coordinates]. The math follows gx.cpp's transform() (and Dolphin's), rounding included.
//
// XF snapshot: 0-255 XF 0x000-0x0FF (position / texture matrices), 256-383 0x400-0x47F (normal),
// 384-639 0x500-0x5FF (post-transform), 640-767 0x600-0x67F (lights), 768-863 registers
// 0x1000-0x105F (0x1018/0x1019: the MATINDEX_A/B in use), 864-865 scissor offset x, y (floats).

layout(std430, set = 0, binding = 0) readonly buffer Verts { uint w[]; } verts;
layout(std430, set = 0, binding = 4) readonly buffer Xfs { uint w[]; } xfs;
layout(push_constant) uniform PC { uint state; uint numTex; uint pad0; uint pad1; } pc;

layout(location = 0) out vec4 col0;
layout(location = 1) out vec4 col1;
layout(location = 2) out vec3 tc[8];
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[2]; };

const uint kXfWords = 868u;
uint X;

float vf(uint o) { return uintBitsToFloat(verts.w[o]); }
float mf(uint a) { return uintBitsToFloat(xfs.w[X + (a & 255u)]); }
float nf(uint a) { return uintBitsToFloat(xfs.w[X + 256u + (a & 127u)]); }
float pf(uint a) { return uintBitsToFloat(xfs.w[X + 384u + (a & 255u)]); }
uint lw(uint a) { return xfs.w[X + 640u + (a & 127u)]; }
vec3 l3(uint a) { return vec3(uintBitsToFloat(lw(a)), uintBitsToFloat(lw(a + 1u)), uintBitsToFloat(lw(a + 2u))); }
uint reg(uint r) { return xfs.w[X + 768u + (r - 0x1000u)]; }
float regf(uint r) { return uintBitsToFloat(reg(r)); }

// a GX color register (RGBA, red in the high byte) as 0-255
vec4 rgba(uint v) { return vec4(float(v >> 24), float((v >> 16) & 255u), float((v >> 8) & 255u), float(v & 255u)); }
// a vertex color word (red in the low byte) as 0-255
vec4 bytes4(uint v) { return vec4(float(v & 255u), float((v >> 8) & 255u), float((v >> 16) & 255u), float(v >> 24)); }
vec3 safeNormalize(vec3 v) { const float l = sqrt(dot(v, v)); return l > 0.0 ? v / l : v; }
// (std::round: halves away from zero)
vec4 roundAway(vec4 v) { return sign(v) * floor(abs(v) + 0.5); }

// one color channel (XF 0x100E+ COLORnCNTRL / 0x1010+ ALPHAnCNTRL): bit 0 material from the vertex,
// 1 lighting, 2-5 and 11-14 the light mask, 6 ambient from the vertex, 7-8 diffuse, 9-10 attenuation
vec4 litChannel(uint ctrl, vec4 vcolor, uint matReg, uint ambReg, vec3 pos, vec3 nrm)
{
	const vec4 mat = (ctrl & 1u) != 0u ? vcolor : rgba(matReg);
	if ((ctrl & 2u) == 0u)
		return mat;
	vec4 lacc = (ctrl & 0x40u) != 0u ? vcolor : rgba(ambReg);
	const uint mask = ((ctrl >> 2) & 15u) | ((ctrl >> 11) & 15u) << 4;
	const uint diffuse = (ctrl >> 7) & 3u, attnFn = (ctrl >> 9) & 3u;
	for (uint l = 0u; l < 8u; l++)
	{
		if ((mask & (1u << l)) == 0u)
			continue;
		const uint base = 16u * l;
		const vec4 color = rgba(lw(base + 3u));
		const vec3 cosatt = l3(base + 4u), distatt = l3(base + 7u), lpos = l3(base + 10u), ldirv = l3(base + 13u);
		vec3 ldir;
		float attn = 1.0;
		if (attnFn == 0u || attnFn == 2u)
		{
			ldir = safeNormalize(lpos - pos);
			if (dot(ldir, ldir) == 0.0)
				ldir = nrm;
		}
		else if (attnFn == 1u)
		{
			ldir = safeNormalize(lpos - pos);
			attn = dot(nrm, ldir) >= 0.0 ? max(0.0, dot(nrm, ldirv)) : 0.0;
			const vec3 d = diffuse == 0u ? distatt : safeNormalize(distatt);
			const vec3 v = vec3(1.0, attn, attn * attn);
			const float den = dot(d, v);
			attn = den != 0.0 ? max(0.0, dot(cosatt, v)) / den : 0.0;
		}
		else
		{
			ldir = lpos - pos;
			const float dist2 = dot(ldir, ldir), dist = sqrt(dist2);
			if (dist > 0.0)
				ldir /= dist;
			attn = max(0.0, dot(ldir, ldirv));
			const float den = dot(distatt, vec3(1.0, dist, dist2));
			attn = den != 0.0 ? max(0.0, cosatt.x + cosatt.y * attn + cosatt.z * attn * attn) / den : 0.0;
		}
		float k = attn;
		if (diffuse == 1u)
			k *= dot(ldir, nrm);
		else if (diffuse == 2u)
			k *= max(0.0, dot(ldir, nrm));
		lacc += roundAway(k * color);
	}
	const vec4 a = clamp(lacc, 0.0, 255.0);
	return floor(mat * (a + floor(a / 128.0)) / 256.0);
}

void main()
{
	const uint b = uint(gl_VertexIndex);
	const uint meta = verts.w[b];
	if ((meta & 0x80000000u) != 0u)
	{
		gl_Position = vec4(vf(b + 1u), vf(b + 2u), vf(b + 3u), vf(b + 4u));
		gl_ClipDistance[0] = gl_Position.z; // (Vulkan's z clip, depth clamp being on)
		gl_ClipDistance[1] = gl_Position.w - gl_Position.z;
		col0 =unpackUnorm4x8(verts.w[b + 5u]);
		col1 = unpackUnorm4x8(verts.w[b + 6u]);
		for (uint i = 0u; i < 8u; i++)
		{
			const uint o = b + 7u + 3u * i;
			tc[i] = i < pc.numTex ? vec3(vf(o), vf(o + 1u), vf(o + 2u)) : vec3(0, 0, 1);
		}
		return;
	}
	X = (meta & 0x3FFFu) * kXfWords;
	uint o = b + 1u;
	const vec3 pos = vec3(vf(o), vf(o + 1u), vf(o + 2u));
	o += 3u;
	// matrix indices
	uint pnmtx, tm[8];
	if ((meta & 0x10000u) != 0u)
	{
		const uint w0 = verts.w[o], w1 = verts.w[o + 1u];
		o += 2u;
		pnmtx = w0 & 63u;
		for (uint n = 0u; n < 4u; n++)
		{
			tm[n] = (w0 >> (6u + 6u * n)) & 63u;
			tm[n + 4u] = (w1 >> (6u * n)) & 63u;
		}
	}
	else
	{
		const uint a = reg(0x1018u), bb = reg(0x1019u);
		pnmtx = a & 63u;
		for (uint n = 0u; n < 4u; n++)
		{
			tm[n] = (a >> (6u + 6u * n)) & 63u;
			tm[n + 4u] = (bb >> (6u * n)) & 63u;
		}
	}
	// normal, binormal, tangent
	vec3 nbt[3] = vec3[3](vec3(0, 0, 1), vec3(0, 1, 0), vec3(1, 0, 0));
	const uint nrmCount = (meta >> 17) & 3u;
	for (uint k = 0u; k < 3u; k++)
		if (k < nrmCount)
		{
			nbt[k] = vec3(vf(o), vf(o + 1u), vf(o + 2u));
			o += 3u;
		}
	const vec4 vcol[2] = vec4[2](bytes4(verts.w[o]), bytes4(verts.w[o + 1u]));
	o += 2u;
	const uint texMask = (meta >> 20) & 255u;
	vec2 vtex[8];
	for (uint k = 0u; k < 8u; k++)
	{
		vtex[k] = vec2(0);
		if ((texMask & (1u << k)) != 0u)
		{
			vtex[k] = vec2(vf(o), vf(o + 1u));
			o += 2u;
		}
	}

	// position and normal matrices
	const uint m = pnmtx * 4u;
	const vec4 p4 = vec4(pos, 1.0);
	const vec3 v = vec3(dot(vec4(mf(m), mf(m + 1u), mf(m + 2u), mf(m + 3u)), p4), dot(vec4(mf(m + 4u), mf(m + 5u), mf(m + 6u), mf(m + 7u)), p4),
		dot(vec4(mf(m + 8u), mf(m + 9u), mf(m + 10u), mf(m + 11u)), p4));
	const uint nm = (pnmtx & 31u) * 3u;
	const vec3 n0 = nbt[0];
	const vec3 viewNrm = safeNormalize(vec3(nf(nm) * n0.x + nf(nm + 1u) * n0.y + nf(nm + 2u) * n0.z, nf(nm + 3u) * n0.x + nf(nm + 4u) * n0.y + nf(nm + 5u) * n0.z,
		nf(nm + 6u) * n0.x + nf(nm + 7u) * n0.y + nf(nm + 8u) * n0.z));

	// color channels (XF 0x1009: how many)
	const uint numChans = reg(0x1009u) & 3u;
	vec4 col[2];
	for (uint c = 0u; c < 2u; c++)
	{
		if (c < numChans)
		{
			const vec4 rgb = litChannel(reg(0x100Eu + c), vcol[c], reg(0x100Cu + c), reg(0x100Au + c), v, viewNrm);
			const vec4 alpha = litChannel(reg(0x1010u + c), vcol[c], reg(0x100Cu + c), reg(0x100Au + c), v, viewNrm);
			col[c] = vec4(rgb.rgb, alpha.a);
		}
		else if (c == 1u && numChans == 1u && (meta & 0x80000u) == 0u)
			col[1] = col[0];
		else
			col[c] = vcol[c];
	}

	// projection (XF 0x1020-0x1025, type at 0x1026: 0 perspective, 1 orthographic)
	float cx, cy, cz, cw;
	if (reg(0x1026u) == 0u)
	{
		cx = regf(0x1020u) * v.x + regf(0x1021u) * v.z;
		cy = regf(0x1022u) * v.y + regf(0x1023u) * v.z;
		cz = regf(0x1024u) * v.z + regf(0x1025u);
		cw = -v.z;
	}
	else
	{
		cx = regf(0x1020u) * v.x + regf(0x1021u);
		cy = regf(0x1022u) * v.y + regf(0x1023u);
		cz = regf(0x1024u) * v.z + regf(0x1025u);
		cw = 1.0;
	}
	// to Vulkan clip space over the 640x528 EFB: the viewport (XF 0x101A-0x101F) less the scissor
	// offset, depth in 2^24 units
	const float xoff = uintBitsToFloat(xfs.w[X + 864u]), yoff = uintBitsToFloat(xfs.w[X + 865u]);
	gl_Position = vec4((cx * regf(0x101Au) + (regf(0x101Du) - xoff) * cw) / 320.0 - cw, (cy * regf(0x101Bu) + (regf(0x101Eu) - yoff) * cw) / 264.0 - cw,
		(cz * regf(0x101Cu) + regf(0x101Fu) * cw) / 16777216.0, cw);
	// GX clips depth in its own clip space, -w <= z <= 0 (as Dolphin: depth clamp on, near and far as
	// clip distances). Vulkan's own z clip ran on the window depth above, (farZ * w + z * range) / 2^24,
	// where float rounding at the far plane is a whole depth unit: sky domes at the far plane lost
	// random triangles (the first level's jagged black patches in the sky).
	const float clipDepth = cz * (1.0 - 1e-7);
	gl_ClipDistance[0] = clipDepth + cw;
	gl_ClipDistance[1] = -clipDepth;
	if ((meta & 0x40000000u) != 0u)
		gl_Position = vec4(uintBitsToFloat(0x7FC00000u));
	col0 = clamp(floor(col[0] + 0.5), 0.0, 255.0) / 255.0;
	col1 = clamp(floor(col[1] + 0.5), 0.0, 255.0) / 255.0;

	// texgens (XF 0x1040+n: bit 1 STQ, bit 2 input form ABC1, bits 4-6 type (regular, emboss, color 0,
	// color 1), bits 7-11 source row, 12-14 emboss source); post-transform (XF 0x1050+n) with dual
	// texture transforms on (XF 0x1012 bit 0)
	const uint numTex = min(reg(0x103Fu) & 15u, 8u);
	const bool dual = (reg(0x1012u) & 1u) != 0u;
	vec3 t[8];
	for (uint n = 0u; n < 8u; n++)
	{
		t[n] = vec3(0, 0, 1);
		if (n >= numTex)
			continue;
		const uint info = reg(0x1040u + n);
		const uint type = (info >> 4) & 7u, row = (info >> 7) & 31u;
		if (type == 2u || type == 3u)
		{
			const vec4 c = col[type - 2u];
			t[n] = vec3(c.r / 255.0, c.g / 255.0, 1.0);
			continue;
		}
		if (type == 1u)
		{
			const uint src = (info >> 12) & 7u;
			if (src < n)
				t[n] = t[src];
			continue;
		}
		vec4 i4 = vec4(0, 0, 1, 1);
		if (row == 0u)
			i4.xyz = pos;
		else if (row == 1u)
			i4.xyz = nbt[0];
		else if (row == 2u)
			i4.xyz = col[0].rgb / 255.0;
		else if (row == 3u)
			i4.xyz = nbt[2];
		else if (row == 4u)
			i4.xyz = nbt[1];
		else if (row >= 5u && row < 13u)
			i4.xy = vtex[row - 5u];
		if (((info >> 2) & 1u) == 0u)
			i4.z = 1.0; // (AB11)
		const uint tb = tm[n] * 4u;
		vec3 r = vec3(dot(vec4(mf(tb), mf(tb + 1u), mf(tb + 2u), mf(tb + 3u)), i4), dot(vec4(mf(tb + 4u), mf(tb + 5u), mf(tb + 6u), mf(tb + 7u)), i4), 1.0);
		if ((info & 2u) != 0u)
			r.z = dot(vec4(mf(tb + 8u), mf(tb + 9u), mf(tb + 10u), mf(tb + 11u)), i4);
		if (dual)
		{
			const uint post = reg(0x1050u + n);
			vec3 s = r;
			if (((post >> 8) & 1u) != 0u)
				s = safeNormalize(s);
			const uint pb = (post & 63u) * 4u;
			const vec4 s4 = vec4(s, 1.0);
			r = vec3(dot(vec4(pf(pb), pf(pb + 1u), pf(pb + 2u), pf(pb + 3u)), s4), dot(vec4(pf(pb + 4u), pf(pb + 5u), pf(pb + 6u), pf(pb + 7u)), s4),
				dot(vec4(pf(pb + 8u), pf(pb + 9u), pf(pb + 10u), pf(pb + 11u)), s4));
		}
		t[n] = r;
	}
	for (uint n = 0u; n < 8u; n++)
		tc[n] = t[n];
}
