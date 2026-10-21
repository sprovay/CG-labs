#version 450

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform MarkerPushConstants {
	mat4 mvp;
	vec4 color;
} marker;

void main() {
	out_color = marker.color;
}
