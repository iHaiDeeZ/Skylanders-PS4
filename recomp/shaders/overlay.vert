#version 450
// The quick menu and the frame counter (ImGui's vertices: position in screen pixels, texture
// coordinate in the font atlas, colour), over the presented frame.
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 col;
layout(push_constant) uniform PC
{
	vec2 scale;
	vec2 translate;
} pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vCol;

void main()
{
	vUv = uv;
	vCol = col;
	gl_Position = vec4(pos * pc.scale + pc.translate, 0.0, 1.0);
}
