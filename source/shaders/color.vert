#version 460 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec4 aColor;

out vec4 fColor;

void main() {
	fColor = aColor;
	gl_Position = vec4(aPos, 1.0);
}