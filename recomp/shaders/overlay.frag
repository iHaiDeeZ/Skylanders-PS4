#version 450
// The quick menu: ImGui's colour times its font atlas (RGBA)
layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vCol;
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vCol * texture(atlas, vUv);
}
