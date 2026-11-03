#version 300 es
in vec4 pos;
in vec2 tc;
out vec2 uv;
void main() { gl_Position = pos; uv = tc; }
