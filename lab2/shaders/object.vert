#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(set = 0, binding = 0) uniform SceneUniforms {
	mat4 view_projection;
	vec4 camera_position;
	vec4 ambient_color_intensity;
	vec4 directional_direction;
	vec4 directional_color_intensity;
	ivec4 light_counts;
} scene;

layout(set = 1, binding = 0) uniform ModelUniforms {
	mat4 model;
	mat4 normal_matrix;
	vec4 albedo;
	vec4 specular_shininess;
} model_data;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec3 world_normal;
layout(location = 2) out vec3 vertex_color;

void main() {
	vec4 world = model_data.model * vec4(in_position, 1.0);
	world_position = world.xyz;
	world_normal = normalize(mat3(model_data.normal_matrix) * in_normal);
	vertex_color = clamp(in_position * 0.8660254 + vec3(0.5), 0.0, 1.0);
	gl_Position = scene.view_projection * world;
}
