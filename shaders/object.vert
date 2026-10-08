#version 450

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform PushConstants {
	mat4 mvp;
	vec4 object_color;
} push_constants;

layout(location = 0) out vec3 vertex_color;

void main() {
	gl_Position = push_constants.mvp * vec4(in_position, 1.0);
	// Map local coordinates from approximately [-1/sqrt(3), 1/sqrt(3)]
	// to the RGB range [0, 1]. The rasterizer interpolates this color.
	vertex_color = clamp(in_position * 0.8660254 + vec3(0.5), 0.0, 1.0);
}
