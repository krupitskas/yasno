#version 460 core

in vec2 vTexCoord;

out vec4 FragColor;

uniform sampler2D albedoTexture;

void main() {
	FragColor = texture(albedoTexture, vTexCoord);
}