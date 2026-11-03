#version 300 es
precision highp float;
in vec2 uv;
out vec4 color;
void main() { color = vec4(uv.x, uv.y, 0.5, 1.0); }
