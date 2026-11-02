#version 450

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec3 vertex_color;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform SceneUniforms {
	mat4 view_projection;
	vec4 camera_position;
	vec4 ambient_color_intensity;
	vec4 directional_direction;
	vec4 directional_color_intensity;
	ivec4 light_counts;
} scene;

struct PointLight {
	vec4 position;
	vec4 color_intensity;
};

struct SpotLight {
	vec4 position;
	vec4 direction;
	vec4 color_intensity;
	vec4 cone_angles;
};

layout(std430, set = 0, binding = 1) readonly buffer PointLightBuffer {
	PointLight point_lights[];
};

layout(std430, set = 0, binding = 2) readonly buffer SpotLightBuffer {
	SpotLight spot_lights[];
};

layout(set = 1, binding = 0) uniform ModelUniforms {
	mat4 model;
	mat4 normal_matrix;
	vec4 albedo;
	vec4 specular_shininess;
} model_data;

vec3 blinnPhong(vec3 normal, vec3 light_direction, vec3 view_direction,
	             vec3 light_color, float intensity) {
	float diffuse_factor = max(dot(normal, light_direction), 0.0);
	vec3 diffuse = model_data.albedo.rgb * vertex_color * light_color *
	               intensity * diffuse_factor;
	vec3 specular = vec3(0.0);
	if (diffuse_factor > 0.0) {
		vec3 halfway_direction = normalize(light_direction + view_direction);
		float highlight = pow(max(dot(normal, halfway_direction), 0.0),
		                      model_data.specular_shininess.a);
		specular = model_data.specular_shininess.rgb * light_color *
		           intensity * highlight;
	}
	return diffuse + specular;
}

void main() {
	vec3 normal = normalize(world_normal);
	vec3 view_direction = normalize(scene.camera_position.xyz - world_position);
	vec3 lighting = model_data.albedo.rgb * vertex_color *
	                scene.ambient_color_intensity.rgb *
	                scene.ambient_color_intensity.a;

	vec3 directional_to_light = normalize(-scene.directional_direction.xyz);
	lighting += blinnPhong(normal, directional_to_light, view_direction,
	                       scene.directional_color_intensity.rgb,
	                       scene.directional_color_intensity.a);

	for (int i = 0; i < scene.light_counts.x; ++i) {
		vec3 to_light = point_lights[i].position.xyz - world_position;
		float distance_squared = max(dot(to_light, to_light), 0.01);
		lighting += blinnPhong(normal, normalize(to_light), view_direction,
		                       point_lights[i].color_intensity.rgb,
		                       point_lights[i].color_intensity.a /
		                       distance_squared);
	}

	for (int i = 0; i < scene.light_counts.y; ++i) {
		vec3 to_light = spot_lights[i].position.xyz - world_position;
		float distance_squared = max(dot(to_light, to_light), 0.01);
		vec3 light_direction = normalize(to_light);
		float angle_cosine = dot(-light_direction,
		                         normalize(spot_lights[i].direction.xyz));
		float cone = smoothstep(spot_lights[i].cone_angles.y,
		                        spot_lights[i].cone_angles.x,
		                        angle_cosine);
		lighting += blinnPhong(normal, light_direction, view_direction,
		                       spot_lights[i].color_intensity.rgb,
		                       spot_lights[i].color_intensity.a * cone /
		                       distance_squared);
	}

	vec3 mapped = lighting / (lighting + vec3(1.0));
	mapped = pow(mapped, vec3(1.0 / 2.2));
	out_color = vec4(mapped, model_data.albedo.a);
}
