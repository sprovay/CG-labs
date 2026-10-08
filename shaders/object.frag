#version 450

layout(location = 0) in vec3 vertex_color;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PushConstants {
	mat4 mvp;
	vec4 object_color;
} push_constants;

void main() {
	out_color = vec4(vertex_color * push_constants.object_color.rgb,
	                 push_constants.object_color.a);
}
