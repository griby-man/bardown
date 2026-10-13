#version 450
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() { color = vec4(texture(picture, uv).rgb, 1.0); }
