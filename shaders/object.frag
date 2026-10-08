#version 450

layout(location = 0) in vec3 vertex_color;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform ObjectUniforms {
	mat4 mvp;
	vec4 object_color;
} object_uniforms;

void main() {
	out_color = vec4(vertex_color * object_uniforms.object_color.rgb,
	                 object_uniforms.object_color.a);
}
