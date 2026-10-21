#include "application.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <imgui.h>

namespace application {

namespace {

struct Vec3 {
	float x;
	float y;
	float z;
};

struct Vertex {
	Vec3 position;
	Vec3 normal;
};

struct Buffer {
	VkBuffer handle = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	void* mapped_data = nullptr;
	VkDeviceSize size = 0;
};

struct Mesh {
	Buffer vertex_buffer;
	Buffer index_buffer;
	uint32_t index_count = 0;
};

struct Mat4 {
	// Column-major layout, matching GLSL mat4.
	std::array<float, 16> values{};
};

struct SceneUniform {
	Mat4 view_projection;
	std::array<float, 4> camera_position;
	std::array<float, 4> ambient_color_intensity;
	std::array<float, 4> directional_direction;
	std::array<float, 4> directional_color_intensity;
	std::array<int32_t, 4> light_counts;
};

struct ModelUniform {
	Mat4 model;
	Mat4 normal_matrix;
	std::array<float, 4> albedo;
	std::array<float, 4> specular_shininess;
};

struct GpuPointLight {
	std::array<float, 4> position;
	std::array<float, 4> color_intensity;
};

struct GpuSpotLight {
	std::array<float, 4> position;
	std::array<float, 4> direction;
	std::array<float, 4> color_intensity;
	std::array<float, 4> cone_angles;
};

struct MarkerPushConstants {
	Mat4 mvp;
	std::array<float, 4> color;
};

struct SceneObject {
	Vec3 position;
	Vec3 rotation;
	Vec3 scale;
	std::array<float, 4> albedo;
	std::array<float, 4> specular;
	float shininess;
	float animation_phase;
	Buffer uniform_buffer;
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
};

struct PointLight {
	Vec3 position;
	std::array<float, 3> color;
	float intensity;
};

struct SpotLight {
	Vec3 position;
	Vec3 direction;
	std::array<float, 3> color;
	float intensity;
	float inner_angle;
	float outer_angle;
};

static_assert(sizeof(SceneUniform) == 144);
static_assert(sizeof(ModelUniform) == 160);
static_assert(sizeof(GpuPointLight) == 32);
static_assert(sizeof(GpuSpotLight) == 64);
static_assert(sizeof(MarkerPushConstants) == 80);

constexpr float pi = 3.14159265358979323846F;
constexpr size_t scene_object_count = 3;
constexpr uint32_t cone_segment_count = 32;
constexpr int perspective_projection = 0;
constexpr int orthographic_projection = 1;

Mesh object_mesh;
Mesh marker_mesh;
Mesh cone_mesh;

VkDescriptorSetLayout scene_set_layout = VK_NULL_HANDLE;
VkDescriptorSetLayout model_set_layout = VK_NULL_HANDLE;
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkDescriptorSet scene_descriptor_set = VK_NULL_HANDLE;

VkPipelineLayout object_pipeline_layout = VK_NULL_HANDLE;
VkPipeline object_pipeline = VK_NULL_HANDLE;
VkPipelineLayout marker_pipeline_layout = VK_NULL_HANDLE;
VkPipeline marker_pipeline = VK_NULL_HANDLE;
VkPipeline cone_pipeline = VK_NULL_HANDLE;

Buffer scene_uniform_buffer;
Buffer point_light_buffer;
Buffer spot_light_buffer;

Vec3 camera_position = {0.0F, 0.4F, 4.5F};
float camera_yaw = -90.0F;
float camera_pitch = -5.0F;
float camera_fov = 55.0F;
int projection_mode = perspective_projection;
float orthographic_half_height = 2.25F;

Vec3 common_position = {0.0F, 0.0F, 0.0F};
Vec3 common_rotation = {0.0F, 0.0F, 0.0F};
Vec3 common_scale = {1.0F, 1.0F, 1.0F};
std::array<float, 4> common_tint = {1.0F, 1.0F, 1.0F, 1.0F};
bool animation_playing = true;
float animation_speed = 1.0F;
Vec3 trajectory_amplitude = {0.75F, 0.40F, 0.40F};
Vec3 trajectory_frequency = {1.00F, 2.00F, 1.50F};
Vec3 rotation_speed = {35.0F, 70.0F, 20.0F};
double previous_frame_time = 0.0;
double animation_time = 0.0;

std::array<float, 3> ambient_color = {0.18F, 0.20F, 0.25F};
float ambient_intensity = 0.35F;
Vec3 directional_direction = {-0.5F, -1.0F, -0.35F};
std::array<float, 3> directional_color = {1.0F, 0.94F, 0.82F};
float directional_intensity = 1.2F;
bool show_light_visualizers = true;

std::array<SceneObject, scene_object_count> scene_objects = {{
	{
		.position = {-0.50F, 0.0F, 0.0F},
		.rotation = {-20.0F, 25.0F, 0.0F},
		.scale = {0.62F, 0.62F, 0.62F},
		.albedo = {0.0F, 0.0F, 0.0F, 1.0F},
		.specular = {1.0F, 0.85F, 0.72F, 1.0F},
		.shininess = 48.0F,
		.animation_phase = 0.0F,
	},
	{
		.position = {0.35F, 0.0F, -0.25F},
		.rotation = {15.0F, 45.0F, 5.0F},
		.scale = {0.72F, 0.72F, 0.72F},
		.albedo = {0.0F, 0.0F, 0.0F, 1.0F},
		.specular = {0.85F, 1.0F, 0.90F, 1.0F},
		.shininess = 72.0F,
		.animation_phase = 2.0943951F,
	},
	{
		.position = {1.20F, 0.0F, -0.55F},
		.rotation = {-10.0F, 65.0F, 20.0F},
		.scale = {0.62F, 0.62F, 0.62F},
		.albedo = {0.0F, 0.0F, 0.0F, 1.0F},
		.specular = {0.78F, 0.86F, 1.0F, 1.0F},
		.shininess = 96.0F,
		.animation_phase = 4.1887902F,
	},
}};

std::vector<PointLight> point_lights = {
	{
		.position = {-1.8F, 1.5F, 1.4F},
		.color = {1.0F, 0.28F, 0.12F},
		.intensity = 5.0F,
	},
	{
		.position = {1.8F, 1.2F, 0.6F},
		.color = {0.15F, 0.40F, 1.0F},
		.intensity = 5.0F,
	},
};

std::vector<SpotLight> spot_lights = {
	{
		.position = {0.0F, 2.5F, 2.0F},
		.direction = {0.0F, -0.75F, -0.66F},
		.color = {0.65F, 0.85F, 1.0F},
		.intensity = 12.0F,
		.inner_angle = 14.0F,
		.outer_angle = 23.0F,
	},
};

float radians(const float degrees) {
	return degrees * pi / 180.0F;
}

Vec3 add(const Vec3 a, const Vec3 b) {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 subtract(const Vec3 a, const Vec3 b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 multiply(const Vec3 value, const float scalar) {
	return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float dot(const Vec3 a, const Vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3 a, const Vec3 b) {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x,
	};
}

float length(const Vec3 value) {
	return std::sqrt(dot(value, value));
}

Vec3 normalize(const Vec3 value) {
	const float vector_length = length(value);
	if (vector_length < 0.00001F) {
		return {0.0F, 0.0F, -1.0F};
	}
	return multiply(value, 1.0F / vector_length);
}

Mat4 identityMatrix() {
	Mat4 result{};
	result.values[0] = 1.0F;
	result.values[5] = 1.0F;
	result.values[10] = 1.0F;
	result.values[15] = 1.0F;
	return result;
}

Mat4 multiply(const Mat4& lhs, const Mat4& rhs) {
	Mat4 result{};
	for (uint32_t column = 0; column < 4; ++column) {
		for (uint32_t row = 0; row < 4; ++row) {
			for (uint32_t k = 0; k < 4; ++k) {
				result.values[column * 4 + row] +=
					lhs.values[k * 4 + row] * rhs.values[column * 4 + k];
			}
		}
	}
	return result;
}

Mat4 rotationX(const float angle) {
	Mat4 result = identityMatrix();
	const float cosine = std::cos(angle);
	const float sine = std::sin(angle);
	result.values[5] = cosine;
	result.values[6] = sine;
	result.values[9] = -sine;
	result.values[10] = cosine;
	return result;
}

Mat4 rotationY(const float angle) {
	Mat4 result = identityMatrix();
	const float cosine = std::cos(angle);
	const float sine = std::sin(angle);
	result.values[0] = cosine;
	result.values[2] = -sine;
	result.values[8] = sine;
	result.values[10] = cosine;
	return result;
}

Mat4 rotationZ(const float angle) {
	Mat4 result = identityMatrix();
	const float cosine = std::cos(angle);
	const float sine = std::sin(angle);
	result.values[0] = cosine;
	result.values[1] = sine;
	result.values[4] = -sine;
	result.values[5] = cosine;
	return result;
}

Mat4 scaling(const float x, const float y, const float z) {
	Mat4 result = identityMatrix();
	result.values[0] = x;
	result.values[5] = y;
	result.values[10] = z;
	return result;
}

Mat4 translation(const float x, const float y, const float z) {
	Mat4 result = identityMatrix();
	result.values[12] = x;
	result.values[13] = y;
	result.values[14] = z;
	return result;
}

Mat4 perspective(const float field_of_view, const float aspect,
	             const float near_plane, const float far_plane) {
	const float focal_length = 1.0F / std::tan(field_of_view * 0.5F);
	Mat4 result{};
	result.values[0] = focal_length / aspect;
	result.values[5] = -focal_length;
	result.values[10] = far_plane / (near_plane - far_plane);
	result.values[11] = -1.0F;
	result.values[14] = -(far_plane * near_plane) / (far_plane - near_plane);
	return result;
}

Mat4 orthographic(const float half_height, const float aspect,
	              const float near_plane, const float far_plane) {
	const float half_width = half_height * aspect;
	Mat4 result = identityMatrix();
	result.values[0] = 1.0F / half_width;
	result.values[5] = -1.0F / half_height;
	result.values[10] = 1.0F / (near_plane - far_plane);
	result.values[14] = near_plane / (near_plane - far_plane);
	return result;
}

Mat4 lookAt(const Vec3 eye, const Vec3 center, const Vec3 up) {
	const Vec3 forward = normalize(subtract(center, eye));
	const Vec3 right = normalize(cross(forward, up));
	const Vec3 corrected_up = cross(right, forward);

	Mat4 result = identityMatrix();
	result.values[0] = right.x;
	result.values[4] = right.y;
	result.values[8] = right.z;
	result.values[1] = corrected_up.x;
	result.values[5] = corrected_up.y;
	result.values[9] = corrected_up.z;
	result.values[2] = -forward.x;
	result.values[6] = -forward.y;
	result.values[10] = -forward.z;
	result.values[12] = -dot(right, eye);
	result.values[13] = -dot(corrected_up, eye);
	result.values[14] = dot(forward, eye);
	return result;
}

Vec3 cameraForward() {
	const float yaw = radians(camera_yaw);
	const float pitch = radians(camera_pitch);
	return normalize({
		std::cos(yaw) * std::cos(pitch),
		std::sin(pitch),
		std::sin(yaw) * std::cos(pitch),
	});
}

Mat4 createViewProjection(const VkExtent2D extent) {
	const float aspect = static_cast<float>(extent.width) /
	                     static_cast<float>(extent.height);
	const Mat4 view = lookAt(camera_position,
	                         add(camera_position, cameraForward()),
	                         {0.0F, 1.0F, 0.0F});
	const Mat4 projection = projection_mode == perspective_projection
		? perspective(radians(camera_fov), aspect, 0.05F, 100.0F)
		: orthographic(orthographic_half_height, aspect, 0.05F, 100.0F);
	return multiply(projection, view);
}

Vec3 animatedPosition(const SceneObject& object) {
	const float time = static_cast<float>(animation_time) +
	                   object.animation_phase;
	return {
		common_position.x + object.position.x +
			trajectory_amplitude.x * std::sin(trajectory_frequency.x * time),
		common_position.y + object.position.y +
			trajectory_amplitude.y * std::sin(trajectory_frequency.y * time),
		common_position.z + object.position.z +
			trajectory_amplitude.z * std::sin(trajectory_frequency.z * time),
	};
}

Vec3 animatedRotation(const SceneObject& object) {
	const float time = static_cast<float>(animation_time) +
	                   object.animation_phase;
	return {
		common_rotation.x + object.rotation.x + rotation_speed.x * time,
		common_rotation.y + object.rotation.y + rotation_speed.y * time,
		common_rotation.z + object.rotation.z + rotation_speed.z * time,
	};
}

Vec3 combinedScale(const SceneObject& object) {
	return {
		common_scale.x * object.scale.x,
		common_scale.y * object.scale.y,
		common_scale.z * object.scale.z,
	};
}

Mat4 createModelMatrix(const SceneObject& object) {
	const Vec3 position = animatedPosition(object);
	const Vec3 angles = animatedRotation(object);
	const Vec3 scale = combinedScale(object);
	const Mat4 rotation = multiply(
		rotationZ(radians(angles.z)),
		multiply(rotationY(radians(angles.y)),
		         rotationX(radians(angles.x))));
	return multiply(
		translation(position.x, position.y, position.z),
		multiply(rotation, scaling(scale.x, scale.y, scale.z)));
}

Mat4 createNormalMatrix(const SceneObject& object) {
	const Vec3 angles = animatedRotation(object);
	const Vec3 scale = combinedScale(object);
	const Mat4 rotation = multiply(
		rotationZ(radians(angles.z)),
		multiply(rotationY(radians(angles.y)),
		         rotationX(radians(angles.x))));
	const float inverse_x = 1.0F / std::max(std::abs(scale.x), 0.001F);
	const float inverse_y = 1.0F / std::max(std::abs(scale.y), 0.001F);
	const float inverse_z = 1.0F / std::max(std::abs(scale.z), 0.001F);
	return multiply(rotation, scaling(inverse_x, inverse_y, inverse_z));
}

Mat4 directionTransform(const Vec3 position, const Vec3 direction,
	                    const float radius, const float cone_length) {
	const Vec3 forward = normalize(direction);
	const Vec3 local_z = multiply(forward, -1.0F);
	const Vec3 helper = std::abs(local_z.y) > 0.95F
		? Vec3{1.0F, 0.0F, 0.0F}
		: Vec3{0.0F, 1.0F, 0.0F};
	const Vec3 local_x = normalize(cross(helper, local_z));
	const Vec3 local_y = cross(local_z, local_x);

	Mat4 orientation = identityMatrix();
	orientation.values[0] = local_x.x;
	orientation.values[1] = local_x.y;
	orientation.values[2] = local_x.z;
	orientation.values[4] = local_y.x;
	orientation.values[5] = local_y.y;
	orientation.values[6] = local_y.z;
	orientation.values[8] = local_z.x;
	orientation.values[9] = local_z.y;
	orientation.values[10] = local_z.z;

	return multiply(
		translation(position.x, position.y, position.z),
		multiply(orientation, scaling(radius, radius, cone_length)));
}

Vec3 pointOnTruncatedEdge(const Vec3& from, const Vec3& to) {
	return add(from, multiply(subtract(to, from), 1.0F / 3.0F));
}

void appendFace(const std::vector<Vec3>& positions,
	            const std::vector<uint32_t>& face,
	            std::vector<Vertex>& vertices,
	            std::vector<uint32_t>& indices) {
	if (face.size() < 3) {
		return;
	}

	const Vec3 first = positions[face[0]];
	Vec3 normal = normalize(cross(subtract(positions[face[1]], first),
	                              subtract(positions[face[2]], first)));
	Vec3 center{};
	for (const uint32_t position_index : face) {
		center = add(center, positions[position_index]);
	}
	center = multiply(center, 1.0F / static_cast<float>(face.size()));
	if (dot(normal, center) < 0.0F) {
		normal = multiply(normal, -1.0F);
	}

	const uint32_t first_vertex = static_cast<uint32_t>(vertices.size());
	for (const uint32_t position_index : face) {
		vertices.push_back({positions[position_index], normal});
	}
	for (uint32_t i = 1; i + 1 < face.size(); ++i) {
		indices.push_back(first_vertex);
		indices.push_back(first_vertex + i);
		indices.push_back(first_vertex + i + 1);
	}
}

void createTruncatedTetrahedron(std::vector<Vertex>& vertices,
	                           std::vector<uint32_t>& indices) {
	std::array<Vec3, 4> tetrahedron_vertices = {{
		{ 1.0F,  1.0F,  1.0F},
		{ 1.0F, -1.0F, -1.0F},
		{-1.0F,  1.0F, -1.0F},
		{-1.0F, -1.0F,  1.0F},
	}};
	for (Vec3& vertex : tetrahedron_vertices) {
		vertex = normalize(vertex);
	}

	std::vector<Vec3> positions;
	std::array<std::array<uint32_t, 4>, 4> edge_indices{};
	for (uint32_t from = 0; from < 4; ++from) {
		for (uint32_t to = 0; to < 4; ++to) {
			if (from == to) {
				continue;
			}
			edge_indices[from][to] = static_cast<uint32_t>(positions.size());
			positions.push_back(pointOnTruncatedEdge(
				tetrahedron_vertices[from], tetrahedron_vertices[to]));
		}
	}

	for (uint32_t center = 0; center < 4; ++center) {
		std::vector<uint32_t> face;
		for (uint32_t other = 0; other < 4; ++other) {
			if (other != center) {
				face.push_back(edge_indices[center][other]);
			}
		}
		appendFace(positions, face, vertices, indices);
	}

	const std::array<std::array<uint32_t, 3>, 4> original_faces = {{
		{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2},
	}};
	for (const auto& face : original_faces) {
		const uint32_t a = face[0];
		const uint32_t b = face[1];
		const uint32_t c = face[2];
		appendFace(positions,
			{edge_indices[a][b], edge_indices[b][a],
			 edge_indices[b][c], edge_indices[c][b],
			 edge_indices[c][a], edge_indices[a][c]},
			vertices, indices);
	}
}

void createMarkerCube(std::vector<Vertex>& vertices,
	                 std::vector<uint32_t>& indices) {
	const std::vector<Vec3> positions = {
		{-1.0F, -1.0F, -1.0F}, { 1.0F, -1.0F, -1.0F},
		{ 1.0F,  1.0F, -1.0F}, {-1.0F,  1.0F, -1.0F},
		{-1.0F, -1.0F,  1.0F}, { 1.0F, -1.0F,  1.0F},
		{ 1.0F,  1.0F,  1.0F}, {-1.0F,  1.0F,  1.0F},
	};
	appendFace(positions, {0, 3, 2, 1}, vertices, indices);
	appendFace(positions, {4, 5, 6, 7}, vertices, indices);
	appendFace(positions, {0, 4, 7, 3}, vertices, indices);
	appendFace(positions, {1, 2, 6, 5}, vertices, indices);
	appendFace(positions, {0, 1, 5, 4}, vertices, indices);
	appendFace(positions, {3, 7, 6, 2}, vertices, indices);
}

void createConeLines(std::vector<Vertex>& vertices,
	                std::vector<uint32_t>& indices) {
	vertices.push_back({{0.0F, 0.0F, 0.0F}, {}});
	vertices.push_back({{0.0F, 0.0F, -1.0F}, {}});
	for (uint32_t i = 0; i < cone_segment_count; ++i) {
		const float angle = 2.0F * pi * static_cast<float>(i) /
		                    static_cast<float>(cone_segment_count);
		vertices.push_back({{std::cos(angle), std::sin(angle), -1.0F}, {}});
	}
	for (uint32_t i = 0; i < cone_segment_count; ++i) {
		const uint32_t current = 2 + i;
		const uint32_t next = 2 + (i + 1) % cone_segment_count;
		indices.push_back(current);
		indices.push_back(next);
		if (i % 4 == 0) {
			indices.push_back(0);
			indices.push_back(current);
		}
	}
	indices.push_back(0);
	indices.push_back(1);
}

bool createBuffer(const VkDeviceSize size, const VkBufferUsageFlags usage,
	              const void* data, Buffer& result) {
	auto& context = graphics::internal::context;
	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	const VmaAllocationCreateInfo allocation_info = {
		.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
		         VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	VmaAllocationInfo created_allocation_info{};
	if (vmaCreateBuffer(context.allocator, &buffer_info, &allocation_info,
	                    &result.handle, &result.allocation,
	                    &created_allocation_info) != VK_SUCCESS) {
		return false;
	}
	if (created_allocation_info.pMappedData == nullptr) {
		vmaDestroyBuffer(context.allocator, result.handle, result.allocation);
		result = {};
		return false;
	}
	result.mapped_data = created_allocation_info.pMappedData;
	result.size = size;
	if (data != nullptr) {
		std::memcpy(result.mapped_data, data, static_cast<size_t>(size));
		vmaFlushAllocation(context.allocator, result.allocation, 0, size);
	}
	return true;
}

void destroyBuffer(Buffer& buffer) {
	if (buffer.handle != VK_NULL_HANDLE) {
		vmaDestroyBuffer(graphics::internal::context.allocator,
		                 buffer.handle, buffer.allocation);
	}
	buffer = {};
}

void writeBuffer(const Buffer& buffer, const void* data,
	             const VkDeviceSize size) {
	std::memcpy(buffer.mapped_data, data, static_cast<size_t>(size));
	vmaFlushAllocation(graphics::internal::context.allocator,
	                   buffer.allocation, 0, size);
}

std::vector<GpuPointLight> makeGpuPointLights() {
	std::vector<GpuPointLight> result;
	result.reserve(std::max<size_t>(point_lights.size(), 1));
	for (const PointLight& light : point_lights) {
		result.push_back({
			.position = {light.position.x, light.position.y,
			             light.position.z, 1.0F},
			.color_intensity = {light.color[0], light.color[1],
			                    light.color[2], light.intensity},
		});
	}
	if (result.empty()) {
		result.push_back({});
	}
	return result;
}

std::vector<GpuSpotLight> makeGpuSpotLights() {
	std::vector<GpuSpotLight> result;
	result.reserve(std::max<size_t>(spot_lights.size(), 1));
	for (const SpotLight& light : spot_lights) {
		const Vec3 direction = normalize(light.direction);
		result.push_back({
			.position = {light.position.x, light.position.y,
			             light.position.z, 1.0F},
			.direction = {direction.x, direction.y, direction.z, 0.0F},
			.color_intensity = {light.color[0], light.color[1],
			                    light.color[2], light.intensity},
			.cone_angles = {std::cos(radians(light.inner_angle)),
			                std::cos(radians(light.outer_angle)), 0.0F, 0.0F},
		});
	}
	if (result.empty()) {
		result.push_back({});
	}
	return result;
}

void updateSceneDescriptorStorageBindings() {
	const std::array<VkDescriptorBufferInfo, 2> infos = {{
		{
			.buffer = point_light_buffer.handle,
			.offset = 0,
			.range = point_light_buffer.size,
		},
		{
			.buffer = spot_light_buffer.handle,
			.offset = 0,
			.range = spot_light_buffer.size,
		},
	}};
	const std::array<VkWriteDescriptorSet, 2> writes = {{
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene_descriptor_set,
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &infos[0],
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene_descriptor_set,
			.dstBinding = 2,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &infos[1],
		},
	}};
	vkUpdateDescriptorSets(graphics::internal::context.device,
	                       static_cast<uint32_t>(writes.size()),
	                       writes.data(), 0, nullptr);
}

bool rebuildLightStorageBuffers() {
	const std::vector<GpuPointLight> gpu_points = makeGpuPointLights();
	const std::vector<GpuSpotLight> gpu_spots = makeGpuSpotLights();
	Buffer new_point_buffer;
	Buffer new_spot_buffer;
	if (!createBuffer(sizeof(GpuPointLight) * gpu_points.size(),
	                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	                  gpu_points.data(), new_point_buffer)) {
		return false;
	}
	if (!createBuffer(sizeof(GpuSpotLight) * gpu_spots.size(),
	                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	                  gpu_spots.data(), new_spot_buffer)) {
		destroyBuffer(new_point_buffer);
		return false;
	}

	auto& context = graphics::internal::context;
	vkQueueWaitIdle(context.graphics_queue);
	destroyBuffer(point_light_buffer);
	destroyBuffer(spot_light_buffer);
	point_light_buffer = new_point_buffer;
	spot_light_buffer = new_spot_buffer;
	updateSceneDescriptorStorageBindings();
	return true;
}

bool createDescriptorResources() {
	auto& context = graphics::internal::context;
	const std::array<VkDescriptorSetLayoutBinding, 3> scene_bindings = {{
		{
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
			              VK_SHADER_STAGE_FRAGMENT_BIT,
		},
		{
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
		{
			.binding = 2,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	}};
	const VkDescriptorSetLayoutCreateInfo scene_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(scene_bindings.size()),
		.pBindings = scene_bindings.data(),
	};
	if (vkCreateDescriptorSetLayout(context.device, &scene_layout_info,
	                                nullptr, &scene_set_layout) != VK_SUCCESS) {
		return false;
	}

	const VkDescriptorSetLayoutBinding model_binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
		              VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	const VkDescriptorSetLayoutCreateInfo model_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &model_binding,
	};
	if (vkCreateDescriptorSetLayout(context.device, &model_layout_info,
	                                nullptr, &model_set_layout) != VK_SUCCESS) {
		return false;
	}

	const SceneUniform initial_scene{};
	if (!createBuffer(sizeof(SceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                  &initial_scene, scene_uniform_buffer)) {
		return false;
	}
	const std::vector<GpuPointLight> gpu_points = makeGpuPointLights();
	const std::vector<GpuSpotLight> gpu_spots = makeGpuSpotLights();
	if (!createBuffer(sizeof(GpuPointLight) * gpu_points.size(),
	                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	                  gpu_points.data(), point_light_buffer) ||
	    !createBuffer(sizeof(GpuSpotLight) * gpu_spots.size(),
	                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	                  gpu_spots.data(), spot_light_buffer)) {
		return false;
	}
	const ModelUniform initial_model{};
	for (SceneObject& object : scene_objects) {
		if (!createBuffer(sizeof(ModelUniform),
		                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		                  &initial_model, object.uniform_buffer)) {
			return false;
		}
	}

	const std::array<VkDescriptorPoolSize, 2> pool_sizes = {{
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = static_cast<uint32_t>(scene_object_count + 1),
		},
		{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 2,
		},
	}};
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = static_cast<uint32_t>(scene_object_count + 1),
		.poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
		.pPoolSizes = pool_sizes.data(),
	};
	if (vkCreateDescriptorPool(context.device, &pool_info, nullptr,
	                           &descriptor_pool) != VK_SUCCESS) {
		return false;
	}

	std::array<VkDescriptorSetLayout, scene_object_count + 1> layouts{};
	layouts[0] = scene_set_layout;
	for (size_t i = 1; i < layouts.size(); ++i) {
		layouts[i] = model_set_layout;
	}
	std::array<VkDescriptorSet, scene_object_count + 1> sets{};
	const VkDescriptorSetAllocateInfo allocation_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptor_pool,
		.descriptorSetCount = static_cast<uint32_t>(sets.size()),
		.pSetLayouts = layouts.data(),
	};
	if (vkAllocateDescriptorSets(context.device, &allocation_info,
	                             sets.data()) != VK_SUCCESS) {
		return false;
	}
	scene_descriptor_set = sets[0];
	for (size_t i = 0; i < scene_objects.size(); ++i) {
		scene_objects[i].descriptor_set = sets[i + 1];
	}

	std::array<VkDescriptorBufferInfo, 3> scene_infos = {{
		{scene_uniform_buffer.handle, 0, sizeof(SceneUniform)},
		{point_light_buffer.handle, 0, point_light_buffer.size},
		{spot_light_buffer.handle, 0, spot_light_buffer.size},
	}};
	std::array<VkDescriptorBufferInfo, scene_object_count> model_infos{};
	std::array<VkWriteDescriptorSet, 3 + scene_object_count> writes{};
	for (uint32_t i = 0; i < 3; ++i) {
		writes[i] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene_descriptor_set,
			.dstBinding = i,
			.descriptorCount = 1,
			.descriptorType = i == 0
				? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
				: VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &scene_infos[i],
		};
	}
	for (size_t i = 0; i < scene_objects.size(); ++i) {
		model_infos[i] = {
			.buffer = scene_objects[i].uniform_buffer.handle,
			.offset = 0,
			.range = sizeof(ModelUniform),
		};
		writes[3 + i] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene_objects[i].descriptor_set,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &model_infos[i],
		};
	}
	vkUpdateDescriptorSets(context.device,
	                       static_cast<uint32_t>(writes.size()),
	                       writes.data(), 0, nullptr);
	return true;
}

std::vector<uint32_t> readShader(const char* path) {
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file) {
		throw std::runtime_error(std::string("Failed to open shader: ") + path);
	}
	const std::streamsize byte_size = file.tellg();
	if (byte_size <= 0 || byte_size % 4 != 0) {
		throw std::runtime_error(std::string("Invalid shader: ") + path);
	}
	std::vector<uint32_t> code(static_cast<size_t>(byte_size) /
	                           sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(code.data()), byte_size);
	if (!file) {
		throw std::runtime_error(std::string("Failed to read shader: ") + path);
	}
	return code;
}

VkShaderModule createShaderModule(const std::vector<uint32_t>& code) {
	const VkShaderModuleCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code.size() * sizeof(uint32_t),
		.pCode = code.data(),
	};
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(graphics::internal::context.device, &create_info,
	                         nullptr, &module) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create Vulkan shader module");
	}
	return module;
}

bool createPipelineVariant(const char* vertex_path, const char* fragment_path,
	                      const VkPipelineLayout layout,
	                      const VkPrimitiveTopology topology,
	                      const bool with_normals, VkPipeline& result) {
	auto& context = graphics::internal::context;
	VkShaderModule vertex_module = VK_NULL_HANDLE;
	VkShaderModule fragment_module = VK_NULL_HANDLE;
	try {
		vertex_module = createShaderModule(readShader(vertex_path));
		fragment_module = createShaderModule(readShader(fragment_path));
	} catch (...) {
		if (vertex_module != VK_NULL_HANDLE) {
			vkDestroyShaderModule(context.device, vertex_module, nullptr);
		}
		throw;
	}

	const std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages = {{
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_module,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_module,
			.pName = "main",
		},
	}};
	const VkVertexInputBindingDescription binding = {
		.binding = 0,
		.stride = sizeof(Vertex),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};
	const std::array<VkVertexInputAttributeDescription, 2> attributes = {{
		{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
		{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
	}};
	const VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &binding,
		.vertexAttributeDescriptionCount = with_normals ? 2U : 1U,
		.pVertexAttributeDescriptions = attributes.data(),
	};
	const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = topology,
		.primitiveRestartEnable = VK_FALSE,
	};
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	const VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0F,
	};
	const VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	const VkPipelineColorBlendAttachmentState blend_attachment = {
		.blendEnable = VK_FALSE,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
		                  VK_COLOR_COMPONENT_G_BIT |
		                  VK_COLOR_COMPONENT_B_BIT |
		                  VK_COLOR_COMPONENT_A_BIT,
	};
	const VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};
	const std::array<VkDynamicState, 2> dynamic_states = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
	};
	const VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
		.pDynamicStates = dynamic_states.data(),
	};
	const VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = static_cast<uint32_t>(shader_stages.size()),
		.pStages = shader_stages.data(),
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &color_blending,
		.pDynamicState = &dynamic_state,
		.layout = layout,
		.renderPass = context.render_pass,
		.subpass = 0,
	};
	const VkResult pipeline_result = vkCreateGraphicsPipelines(
		context.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &result);
	vkDestroyShaderModule(context.device, fragment_module, nullptr);
	vkDestroyShaderModule(context.device, vertex_module, nullptr);
	return pipeline_result == VK_SUCCESS;
}

bool createPipelines() {
	auto& context = graphics::internal::context;
	const std::array<VkDescriptorSetLayout, 2> object_layouts = {
		scene_set_layout, model_set_layout,
	};
	const VkPipelineLayoutCreateInfo object_layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = static_cast<uint32_t>(object_layouts.size()),
		.pSetLayouts = object_layouts.data(),
	};
	if (vkCreatePipelineLayout(context.device, &object_layout_info, nullptr,
	                           &object_pipeline_layout) != VK_SUCCESS) {
		return false;
	}

	const VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
		              VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(MarkerPushConstants),
	};
	const VkPipelineLayoutCreateInfo marker_layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	if (vkCreatePipelineLayout(context.device, &marker_layout_info, nullptr,
	                           &marker_pipeline_layout) != VK_SUCCESS) {
		return false;
	}

	return createPipelineVariant("shaders/object.vert.spv",
	                             "shaders/object.frag.spv",
	                             object_pipeline_layout,
	                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	                             true, object_pipeline) &&
	       createPipelineVariant("shaders/marker.vert.spv",
	                             "shaders/marker.frag.spv",
	                             marker_pipeline_layout,
	                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	                             false, marker_pipeline) &&
	       createPipelineVariant("shaders/marker.vert.spv",
	                             "shaders/marker.frag.spv",
	                             marker_pipeline_layout,
	                             VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	                             false, cone_pipeline);
}

bool createMesh(const std::vector<Vertex>& vertices,
	            const std::vector<uint32_t>& indices, Mesh& mesh) {
	if (!createBuffer(sizeof(Vertex) * vertices.size(),
	                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                  vertices.data(), mesh.vertex_buffer) ||
	    !createBuffer(sizeof(uint32_t) * indices.size(),
	                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                  indices.data(), mesh.index_buffer)) {
		return false;
	}
	mesh.index_count = static_cast<uint32_t>(indices.size());
	return true;
}

void destroyMesh(Mesh& mesh) {
	destroyBuffer(mesh.index_buffer);
	destroyBuffer(mesh.vertex_buffer);
	mesh = {};
}

void destroyResources() {
	auto& context = graphics::internal::context;
	if (cone_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(context.device, cone_pipeline, nullptr);
		cone_pipeline = VK_NULL_HANDLE;
	}
	if (marker_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(context.device, marker_pipeline, nullptr);
		marker_pipeline = VK_NULL_HANDLE;
	}
	if (object_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(context.device, object_pipeline, nullptr);
		object_pipeline = VK_NULL_HANDLE;
	}
	if (marker_pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(context.device, marker_pipeline_layout, nullptr);
		marker_pipeline_layout = VK_NULL_HANDLE;
	}
	if (object_pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(context.device, object_pipeline_layout, nullptr);
		object_pipeline_layout = VK_NULL_HANDLE;
	}
	if (descriptor_pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
		descriptor_pool = VK_NULL_HANDLE;
	}
	for (SceneObject& object : scene_objects) {
		destroyBuffer(object.uniform_buffer);
		object.descriptor_set = VK_NULL_HANDLE;
	}
	destroyBuffer(spot_light_buffer);
	destroyBuffer(point_light_buffer);
	destroyBuffer(scene_uniform_buffer);
	if (model_set_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(context.device, model_set_layout, nullptr);
		model_set_layout = VK_NULL_HANDLE;
	}
	if (scene_set_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(context.device, scene_set_layout, nullptr);
		scene_set_layout = VK_NULL_HANDLE;
	}
	scene_descriptor_set = VK_NULL_HANDLE;
	destroyMesh(cone_mesh);
	destroyMesh(marker_mesh);
	destroyMesh(object_mesh);
}

void drawVec3Control(const char* label, Vec3& value,
	                const float speed, const float minimum,
	                const float maximum) {
	float values[3] = {value.x, value.y, value.z};
	if (ImGui::DragFloat3(label, values, speed, minimum, maximum, "%.2f")) {
		value = {values[0], values[1], values[2]};
	}
}

void drawObjectControls(SceneObject& object, const size_t index) {
	ImGui::PushID(static_cast<int>(index));
	const std::string title = "Model " + std::to_string(index + 1);
	if (ImGui::TreeNode(title.c_str())) {
		drawVec3Control("Position", object.position, 0.02F, -5.0F, 5.0F);
		drawVec3Control("Rotation", object.rotation, 0.5F, -180.0F, 180.0F);
		drawVec3Control("Scale", object.scale, 0.01F, 0.05F, 3.0F);
		ImGui::ColorEdit3("Albedo", object.albedo.data());
		ImGui::ColorEdit3("Specular color", object.specular.data());
		ImGui::SliderFloat("Shininess", &object.shininess, 1.0F, 256.0F,
		                   "%.1f");
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void drawPointLightControls(bool& storage_changed) {
	if (ImGui::Button("Add point light")) {
		point_lights.push_back({
			.position = {0.0F, 1.5F, 1.5F},
			.color = {1.0F, 1.0F, 1.0F},
			.intensity = 4.0F,
		});
		storage_changed = true;
	}
	for (size_t i = 0; i < point_lights.size();) {
		ImGui::PushID(static_cast<int>(i));
		bool remove = false;
		const std::string title = "Point light " + std::to_string(i + 1);
		if (ImGui::TreeNode(title.c_str())) {
			drawVec3Control("Position", point_lights[i].position,
			                0.02F, -10.0F, 10.0F);
			ImGui::ColorEdit3("Color", point_lights[i].color.data());
			ImGui::SliderFloat("Intensity", &point_lights[i].intensity,
			                   0.0F, 30.0F, "%.2f");
			remove = ImGui::Button("Remove");
			ImGui::TreePop();
		}
		ImGui::PopID();
		if (remove) {
			point_lights.erase(point_lights.begin() + static_cast<std::ptrdiff_t>(i));
			storage_changed = true;
		} else {
			++i;
		}
	}
}

void drawSpotLightControls(bool& storage_changed) {
	if (ImGui::Button("Add spotlight")) {
		spot_lights.push_back({
			.position = {0.0F, 2.0F, 2.0F},
			.direction = {0.0F, -0.7F, -0.7F},
			.color = {1.0F, 1.0F, 1.0F},
			.intensity = 8.0F,
			.inner_angle = 15.0F,
			.outer_angle = 24.0F,
		});
		storage_changed = true;
	}
	for (size_t i = 0; i < spot_lights.size();) {
		ImGui::PushID(static_cast<int>(i));
		bool remove = false;
		const std::string title = "Spotlight " + std::to_string(i + 1);
		if (ImGui::TreeNode(title.c_str())) {
			drawVec3Control("Position", spot_lights[i].position,
			                0.02F, -10.0F, 10.0F);
			drawVec3Control("Direction", spot_lights[i].direction,
			                0.01F, -1.0F, 1.0F);
			ImGui::ColorEdit3("Color", spot_lights[i].color.data());
			ImGui::SliderFloat("Intensity", &spot_lights[i].intensity,
			                   0.0F, 40.0F, "%.2f");
			ImGui::SliderFloat("Inner angle", &spot_lights[i].inner_angle,
			                   1.0F, 79.0F, "%.1f deg");
			ImGui::SliderFloat("Outer angle", &spot_lights[i].outer_angle,
			                   1.1F, 80.0F, "%.1f deg");
			spot_lights[i].outer_angle = std::max(
				spot_lights[i].outer_angle,
				spot_lights[i].inner_angle + 0.1F);
			remove = ImGui::Button("Remove");
			ImGui::TreePop();
		}
		ImGui::PopID();
		if (remove) {
			spot_lights.erase(spot_lights.begin() + static_cast<std::ptrdiff_t>(i));
			storage_changed = true;
		} else {
			++i;
		}
	}
}

void bindMesh(const VkCommandBuffer command_buffer, const Mesh& mesh) {
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(command_buffer, 0, 1,
	                       &mesh.vertex_buffer.handle, &offset);
	vkCmdBindIndexBuffer(command_buffer, mesh.index_buffer.handle,
	                     0, VK_INDEX_TYPE_UINT32);
}

void pushAndDrawMarker(const VkCommandBuffer command_buffer,
	                  const Mat4& view_projection, const Mat4& model,
	                  const std::array<float, 3>& color, const Mesh& mesh) {
	const MarkerPushConstants push = {
		.mvp = multiply(view_projection, model),
		.color = {color[0], color[1], color[2], 1.0F},
	};
	vkCmdPushConstants(command_buffer, marker_pipeline_layout,
	                   VK_SHADER_STAGE_VERTEX_BIT |
	                   VK_SHADER_STAGE_FRAGMENT_BIT,
	                   0, sizeof(push), &push);
	vkCmdDrawIndexed(command_buffer, mesh.index_count, 1, 0, 0, 0);
}

} // namespace

bool initialize() {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	createTruncatedTetrahedron(vertices, indices);
	if (!createMesh(vertices, indices, object_mesh)) {
		std::cerr << "Failed to create object mesh\n";
		destroyResources();
		return false;
	}

	vertices.clear();
	indices.clear();
	createMarkerCube(vertices, indices);
	if (!createMesh(vertices, indices, marker_mesh)) {
		std::cerr << "Failed to create light marker mesh\n";
		destroyResources();
		return false;
	}

	vertices.clear();
	indices.clear();
	createConeLines(vertices, indices);
	if (!createMesh(vertices, indices, cone_mesh)) {
		std::cerr << "Failed to create spotlight cone mesh\n";
		destroyResources();
		return false;
	}

	if (!createDescriptorResources()) {
		std::cerr << "Failed to create descriptor resources\n";
		destroyResources();
		return false;
	}
	try {
		if (!createPipelines()) {
			std::cerr << "Failed to create graphics pipelines\n";
			destroyResources();
			return false;
		}
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		destroyResources();
		return false;
	}
	return true;
}

void shutdown() {
	vkQueueWaitIdle(graphics::internal::context.graphics_queue);
	destroyResources();
}

void update(const double time) {
	double delta_time = previous_frame_time == 0.0
		? 0.0
		: time - previous_frame_time;
	previous_frame_time = time;
	delta_time = std::clamp(delta_time, 0.0, 0.1);
	if (animation_playing) {
		animation_time += delta_time * static_cast<double>(animation_speed);
	}

	ImGui::SetNextWindowSize(ImVec2(500.0F, 700.0F), ImGuiCond_Once);
	ImGui::Begin("CG Lab 2 - Camera and Lighting");
	ImGui::TextUnformatted("Variant 4: truncated regular tetrahedron");
	ImGui::Text("Scene/model descriptor sets: 1 + %zu", scene_objects.size());
	ImGui::Text("Point lights: %zu | Spotlights: %zu",
	            point_lights.size(), spot_lights.size());

	if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::RadioButton("Perspective", &projection_mode,
		                   perspective_projection);
		ImGui::SameLine();
		ImGui::RadioButton("Orthographic", &projection_mode,
		                   orthographic_projection);
		drawVec3Control("Camera position", camera_position,
		                0.02F, -10.0F, 10.0F);
		ImGui::SliderFloat("Yaw", &camera_yaw, -180.0F, 180.0F, "%.1f deg");
		ImGui::SliderFloat("Pitch", &camera_pitch, -89.0F, 89.0F, "%.1f deg");
		if (projection_mode == perspective_projection) {
			ImGui::SliderFloat("Field of view", &camera_fov,
			                   20.0F, 100.0F, "%.1f deg");
		} else {
			ImGui::SliderFloat("Orthographic size", &orthographic_half_height,
			                   0.25F, 10.0F, "%.2f");
		}
		if (ImGui::Button("Reset camera")) {
			camera_position = {0.0F, 0.4F, 4.5F};
			camera_yaw = -90.0F;
			camera_pitch = -5.0F;
			camera_fov = 55.0F;
		}
	}

	if (ImGui::CollapsingHeader("Lab 1 transforms and animation")) {
		drawVec3Control("Common position", common_position,
		                0.02F, -5.0F, 5.0F);
		drawVec3Control("Common rotation", common_rotation,
		                0.5F, -180.0F, 180.0F);
		drawVec3Control("Common scale", common_scale,
		                0.01F, 0.05F, 3.0F);
		if (ImGui::Button("Reset common transform")) {
			common_position = {0.0F, 0.0F, 0.0F};
			common_rotation = {0.0F, 0.0F, 0.0F};
			common_scale = {1.0F, 1.0F, 1.0F};
		}
		if (ImGui::Button(animation_playing ? "Pause animation"
		                                  : "Play animation")) {
			animation_playing = !animation_playing;
		}
		ImGui::SameLine();
		if (ImGui::Button("Restart animation")) {
			animation_time = 0.0;
		}
		ImGui::SliderFloat("Animation speed", &animation_speed,
		                   0.0F, 3.0F, "%.2f");
		drawVec3Control("Path amplitude", trajectory_amplitude,
		                0.01F, 0.0F, 1.5F);
		drawVec3Control("Path frequency", trajectory_frequency,
		                0.01F, 0.1F, 3.0F);
		drawVec3Control("Rotation speed", rotation_speed,
		                0.5F, -180.0F, 180.0F);
		ImGui::ColorEdit3("Common tint", common_tint.data());
		ImGui::Text("Animation time: %.2f s", animation_time);
	}

	if (ImGui::CollapsingHeader("Ambient and directional light",
	                            ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::ColorEdit3("Ambient color", ambient_color.data());
		ImGui::SliderFloat("Ambient intensity", &ambient_intensity,
		                   0.0F, 2.0F, "%.2f");
		drawVec3Control("Light direction", directional_direction,
		                0.01F, -1.0F, 1.0F);
		ImGui::ColorEdit3("Directional color", directional_color.data());
		ImGui::SliderFloat("Directional intensity", &directional_intensity,
		                   0.0F, 5.0F, "%.2f");
	}

	if (ImGui::CollapsingHeader("Models and materials", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (size_t i = 0; i < scene_objects.size(); ++i) {
			drawObjectControls(scene_objects[i], i);
		}
	}

	bool storage_changed = false;
	if (ImGui::CollapsingHeader("Point lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		drawPointLightControls(storage_changed);
	}
	if (ImGui::CollapsingHeader("Spotlights", ImGuiTreeNodeFlags_DefaultOpen)) {
		drawSpotLightControls(storage_changed);
	}
	ImGui::Checkbox("Show light visualizers", &show_light_visualizers);
	ImGui::End();

	if (storage_changed && !rebuildLightStorageBuffers()) {
		std::cerr << "Failed to resize light storage buffers\n";
	}
}

void render(const graphics::internal::FrameData& fd) {
	auto& context = graphics::internal::context;
	const Mat4 view_projection = createViewProjection(context.swapchain_extent);
	const Vec3 light_direction = normalize(directional_direction);
	const SceneUniform scene_data = {
		.view_projection = view_projection,
		.camera_position = {camera_position.x, camera_position.y,
		                    camera_position.z, 1.0F},
		.ambient_color_intensity = {ambient_color[0], ambient_color[1],
		                            ambient_color[2], ambient_intensity},
		.directional_direction = {light_direction.x, light_direction.y,
		                          light_direction.z, 0.0F},
		.directional_color_intensity = {
			directional_color[0], directional_color[1], directional_color[2],
			directional_intensity,
		},
		.light_counts = {static_cast<int32_t>(point_lights.size()),
		                 static_cast<int32_t>(spot_lights.size()), 0, 0},
	};
	writeBuffer(scene_uniform_buffer, &scene_data, sizeof(scene_data));
	const std::vector<GpuPointLight> gpu_points = makeGpuPointLights();
	const std::vector<GpuSpotLight> gpu_spots = makeGpuSpotLights();
	writeBuffer(point_light_buffer, gpu_points.data(),
	            sizeof(GpuPointLight) * gpu_points.size());
	writeBuffer(spot_light_buffer, gpu_spots.data(),
	            sizeof(GpuSpotLight) * gpu_spots.size());

	vkResetCommandBuffer(fd.command_buffer, 0);
	const VkCommandBufferBeginInfo command_buffer_begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(fd.command_buffer, &command_buffer_begin);
	const VkClearValue clear_values[] = {
		{.color = {{0.025F, 0.030F, 0.045F, 1.0F}}},
		{.depthStencil = {1.0F, 0}},
	};
	const VkRenderPassBeginInfo render_pass_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = context.render_pass,
		.framebuffer = fd.framebuffer,
		.renderArea = {.extent = context.swapchain_extent},
		.clearValueCount = 2,
		.pClearValues = clear_values,
	};
	vkCmdBeginRenderPass(fd.command_buffer, &render_pass_begin,
	                     VK_SUBPASS_CONTENTS_INLINE);
	const VkViewport viewport = {
		.width = static_cast<float>(context.swapchain_extent.width),
		.height = static_cast<float>(context.swapchain_extent.height),
		.minDepth = 0.0F,
		.maxDepth = 1.0F,
	};
	const VkRect2D scissor = {.extent = context.swapchain_extent};
	vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
	vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);

	vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                  object_pipeline);
	bindMesh(fd.command_buffer, object_mesh);
	vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        object_pipeline_layout, 0, 1,
	                        &scene_descriptor_set, 0, nullptr);
	for (SceneObject& object : scene_objects) {
		const std::array<float, 4> tinted_albedo = {
			object.albedo[0] * common_tint[0],
			object.albedo[1] * common_tint[1],
			object.albedo[2] * common_tint[2],
			object.albedo[3] * common_tint[3],
		};
		const ModelUniform model_data = {
			.model = createModelMatrix(object),
			.normal_matrix = createNormalMatrix(object),
			.albedo = tinted_albedo,
			.specular_shininess = {object.specular[0], object.specular[1],
			                         object.specular[2], object.shininess},
		};
		writeBuffer(object.uniform_buffer, &model_data, sizeof(model_data));
		vkCmdBindDescriptorSets(fd.command_buffer,
		                        VK_PIPELINE_BIND_POINT_GRAPHICS,
		                        object_pipeline_layout, 1, 1,
		                        &object.descriptor_set, 0, nullptr);
		vkCmdDrawIndexed(fd.command_buffer, object_mesh.index_count,
		                 1, 0, 0, 0);
	}

	if (show_light_visualizers) {
		vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                  marker_pipeline);
		bindMesh(fd.command_buffer, marker_mesh);
		for (const PointLight& light : point_lights) {
			const Mat4 model = multiply(
				translation(light.position.x, light.position.y, light.position.z),
				scaling(0.075F, 0.075F, 0.075F));
			pushAndDrawMarker(fd.command_buffer, view_projection, model,
			                  light.color, marker_mesh);
		}
		for (const SpotLight& light : spot_lights) {
			const Mat4 model = multiply(
				translation(light.position.x, light.position.y, light.position.z),
				scaling(0.075F, 0.075F, 0.075F));
			pushAndDrawMarker(fd.command_buffer, view_projection, model,
			                  light.color, marker_mesh);
		}

		vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		                  cone_pipeline);
		bindMesh(fd.command_buffer, cone_mesh);
		for (const SpotLight& light : spot_lights) {
			constexpr float visualizer_length = 1.25F;
			const float radius = std::tan(radians(light.outer_angle)) *
			                     visualizer_length;
			const Mat4 model = directionTransform(
				light.position, light.direction, radius, visualizer_length);
			pushAndDrawMarker(fd.command_buffer, view_projection, model,
			                  {1.0F, 1.0F, 1.0F}, cone_mesh);
		}
	}

	vkCmdEndRenderPass(fd.command_buffer);
	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
