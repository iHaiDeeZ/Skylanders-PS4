#version 450
// EFB copies (BP 0x52): a rectangle of the EFB's color or depth into a texture (or the XFB), with
// the copy format's channels laid out the way the texture decoder would read them back: an I8
// texture made from an R8 copy samples as (r, r, r, r), an IA8 one from RG8 as (g, g, g, r), and so on.
layout(set = 0, binding = 0) uniform sampler2D efbColor;
layout(set = 0, binding = 1) uniform sampler2D efbDepth;
layout(push_constant) uniform PC
{
	ivec4 src;  // x, y, width, height in EFB pixels
	uint mode;  // bits 0-3 format, 4 depth, 5 intensity, 6 half, 7 EFB has alpha, 8 XFB, 9 clear
	uint pad0, pad1, pad2; // (clear: the color as RGBA8 (red in the low byte), the 24-bit depth;
	                       // a copy: pad1 = the copy filter's weights summed, 64 = 1.0)
} pc;
layout(location = 0) out vec4 outColor;

void main()
{
	// an EFB clear with some channels masked (the pipeline's write mask): the clear color and depth
	if ((pc.mode & 512u) != 0u)
	{
		outColor = unpackUnorm4x8(pc.pad0);
		gl_FragDepth = float(pc.pad1 & 0xFFFFFFu) / 16777216.0;
		return;
	}
	gl_FragDepth = 0.0;
	const ivec2 p = ivec2(gl_FragCoord.xy);
	const bool half_ = (pc.mode & 64u) != 0u;
	vec4 c = vec4(0);
	float z = 0.0;
	if (half_)
	{
		for (int i = 0; i < 4; i++)
		{
			const ivec2 q = pc.src.xy + p * 2 + ivec2(i & 1, i >> 1);
			c += texelFetch(efbColor, q, 0);
			z += texelFetch(efbDepth, q, 0).r;
		}
		c *= 0.25;
		z *= 0.25;
	}
	else
	{
		c = texelFetch(efbColor, pc.src.xy + p, 0);
		z = texelFetch(efbDepth, pc.src.xy + p, 0).r;
	}
	if ((pc.mode & 128u) == 0u)
		c.a = 1.0;
	// (the copy filter's brightness: its weights' total over 64, colour only, clamped - Dolphin's copy with
	// its vertical filter off)
	if (pc.pad1 != 64u && (pc.mode & 16u) == 0u)
		c.rgb = min(c.rgb * (float(pc.pad1) / 64.0), vec3(1.0));
	const uint fmt = pc.mode & 15u;
	if ((pc.mode & 256u) != 0u)
	{
		outColor = vec4(c.rgb, 1.0);
		return;
	}
	if ((pc.mode & 16u) != 0u)
	{
		// (depth d is stored as d / 2^24, draws' a hair less (the viewport's max depth is 0xFFFFFF / 2^24,
		// gx_vk.cpp beginRendering): rounding gives d back for both)
		const uint d = min(uint(clamp(z, 0.0, 1.0) * 16777216.0 + 0.5), 16777215u);
		const float hi = float((d >> 16) & 255u) / 255.0, mid = float((d >> 8) & 255u) / 255.0, lo = float(d & 255u) / 255.0;
		switch (fmt)
		{
		case 3u: outColor = vec4(mid, mid, mid, hi); break;   // Z16 (IA8)
		case 6u: outColor = vec4(hi, mid, lo, 1.0); break;    // Z24X8 (RGBA8)
		case 9u: outColor = vec4(mid); break;                 // Z8M
		case 10u: outColor = vec4(lo); break;                 // Z8L
		case 11u: outColor = vec4(hi, hi, hi, mid); break;    // Z16R
		case 12u: outColor = vec4(lo, lo, lo, mid); break;    // Z16L
		default: outColor = vec4(hi); break;                  // Z4, Z8
		}
		return;
	}
	float i = c.r;
	if ((pc.mode & 32u) != 0u)
		i = clamp(0.257 * c.r + 0.504 * c.g + 0.098 * c.b + 16.0 / 255.0, 0.0, 1.0);
	switch (fmt)
	{
	case 0u: case 1u: case 8u: outColor = vec4(i); break;  // R4, R8 (Y8)
	case 2u: case 3u: outColor = vec4(i, i, i, c.a); break; // RA4, RA8
	case 4u: outColor = vec4(c.rgb, 1.0); break;            // RGB565
	case 7u: outColor = vec4(c.a); break;                   // A8
	case 9u: outColor = vec4(c.g); break;                   // G8
	case 10u: outColor = vec4(c.b); break;                  // B8
	case 11u: outColor = vec4(c.g, c.g, c.g, c.r); break;   // RG8
	case 12u: outColor = vec4(c.b, c.b, c.b, c.g); break;   // GB8
	default: outColor = c; break;                           // RGB5A3, RGBA8
	}
}
