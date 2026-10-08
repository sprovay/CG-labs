#version 450

layout(location = 0) in vec3 in_position;

layout(set = 0, binding = 0) uniform ObjectUniforms {
	mat4 mvp;
	vec4 object_color;
} object_uniforms;

layout(location = 0) out vec3 vertex_color;

void main() {
	gl_Position = object_uniforms.mvp * vec4(in_position, 1.0);
	// Map local coordinates from approximately [-1/sqrt(3), 1/sqrt(3)]
	// to the RGB range [0, 1]. The rasterizer interpolates this color.
	vertex_color = clamp(in_position * 0.8660254 + vec3(0.5), 0.0, 1.0);
}
