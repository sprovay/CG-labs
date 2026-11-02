#version 450

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform MarkerPushConstants {
	mat4 mvp;
	vec4 color;
} marker;

void main() {
	gl_Position = marker.mvp * vec4(in_position, 1.0);
}
