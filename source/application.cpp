#include "application.hpp"

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
	Vec3 color;
};

struct Buffer {
	VkBuffer handle = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
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

Mesh mesh;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE;
int projection_mode = 0;
std::array<float, 3> object_position = {0.0F, 0.0F, 0.0F};
std::array<float, 3> object_rotation = {-20.0F, 30.0F, 0.0F};
std::array<float, 3> object_scale = {1.0F, 1.0F, 1.0F};

constexpr int perspective_projection = 0;
constexpr int orthographic_projection = 1;

constexpr float pi = 3.14159265358979323846F;

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

Mat4 rotationX(float angle) {
	Mat4 result = identityMatrix();
	const float cosine = std::cos(angle);
	const float sine = std::sin(angle);
	result.values[5] = cosine;
	result.values[6] = sine;
	result.values[9] = -sine;
	result.values[10] = cosine;
	return result;
}

Mat4 rotationY(float angle) {
	Mat4 result = identityMatrix();
	const float cosine = std::cos(angle);
	const float sine = std::sin(angle);
	result.values[0] = cosine;
	result.values[2] = -sine;
	result.values[8] = sine;
	result.values[10] = cosine;
	return result;
}

Mat4 rotationZ(float angle) {
	Mat4 result = identityMatrix();
	const float cosine = std::cos(angle);
	const float sine = std::sin(angle);
	result.values[0] = cosine;
	result.values[1] = sine;
	result.values[4] = -sine;
	result.values[5] = cosine;
	return result;
}

Mat4 scaling(float x, float y, float z) {
	Mat4 result = identityMatrix();
	result.values[0] = x;
	result.values[5] = y;
	result.values[10] = z;
	return result;
}

Mat4 translation(float x, float y, float z) {
	Mat4 result = identityMatrix();
	result.values[12] = x;
	result.values[13] = y;
	result.values[14] = z;
	return result;
}

Mat4 perspective(float field_of_view, float aspect, float near_plane,
	             float far_plane) {
	const float focal_length = 1.0F / std::tan(field_of_view * 0.5F);
	Mat4 result{};
	result.values[0] = focal_length / aspect;
	result.values[5] = -focal_length;
	result.values[10] = far_plane / (near_plane - far_plane);
	result.values[11] = -1.0F;
	result.values[14] = -(far_plane * near_plane) /
	                    (far_plane - near_plane);
	return result;
}

Mat4 orthographic(float half_height, float aspect, float near_plane,
	              float far_plane) {
	const float half_width = half_height * aspect;
	Mat4 result = identityMatrix();
	result.values[0] = 1.0F / half_width;
	result.values[5] = -1.0F / half_height;
	result.values[10] = 1.0F / (near_plane - far_plane);
	result.values[14] = near_plane / (near_plane - far_plane);
	return result;
}

Vec3 normalize(Vec3 value) {
	const float length = std::sqrt(value.x * value.x +
	                               value.y * value.y +
	                               value.z * value.z);
	return { value.x / length, value.y / length, value.z / length };
}

Vec3 pointOnTruncatedEdge(const Vec3& from, const Vec3& to) {
	return {
		from.x + (to.x - from.x) / 3.0F,
		from.y + (to.y - from.y) / 3.0F,
		from.z + (to.z - from.z) / 3.0F,
	};
}

void appendFace(const std::vector<Vec3>& positions,
	            const std::vector<uint32_t>& face,
	            const Vec3& color,
	            std::vector<Vertex>& vertices,
	            std::vector<uint32_t>& indices) {
	const uint32_t first_vertex = static_cast<uint32_t>(vertices.size());
	for (const uint32_t position_index : face) {
		vertices.push_back({ positions[position_index], color });
	}

	for (uint32_t i = 1; i + 1 < face.size(); ++i) {
		indices.push_back(first_vertex);
		indices.push_back(first_vertex + i);
		indices.push_back(first_vertex + i + 1);
	}
}

void createTruncatedTetrahedron(std::vector<Vertex>& vertices,
	                           std::vector<uint32_t>& indices) {
	std::array<Vec3, 4> tetrahedron_vertices = {
		Vec3{ 1.0F,  1.0F,  1.0F },
		Vec3{ 1.0F, -1.0F, -1.0F },
		Vec3{-1.0F,  1.0F, -1.0F },
		Vec3{-1.0F, -1.0F,  1.0F },
	};
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

	const std::array<Vec3, 8> face_colors = {
		Vec3{0.90F, 0.25F, 0.22F},
		Vec3{0.20F, 0.70F, 0.30F},
		Vec3{0.20F, 0.45F, 0.90F},
		Vec3{0.90F, 0.65F, 0.15F},
		Vec3{0.70F, 0.25F, 0.85F},
		Vec3{0.15F, 0.75F, 0.75F},
		Vec3{0.95F, 0.45F, 0.65F},
		Vec3{0.55F, 0.70F, 0.20F},
	};

	// Four triangular faces around the original tetrahedron vertices.
	for (uint32_t center = 0; center < 4; ++center) {
		std::vector<uint32_t> face;
		for (uint32_t other = 0; other < 4; ++other) {
			if (other != center) {
				face.push_back(edge_indices[center][other]);
			}
		}
		appendFace(positions, face, face_colors[center], vertices, indices);
	}

	// Every original triangular face becomes a regular hexagon.
	const std::array<std::array<uint32_t, 3>, 4> original_faces = {{
		{0, 1, 2},
		{0, 3, 1},
		{0, 2, 3},
		{1, 3, 2},
	}};
	for (uint32_t face_index = 0; face_index < original_faces.size();
	     ++face_index) {
		const auto& face = original_faces[face_index];
		const uint32_t a = face[0];
		const uint32_t b = face[1];
		const uint32_t c = face[2];
		appendFace(positions,
			{ edge_indices[a][b], edge_indices[b][a],
			  edge_indices[b][c], edge_indices[c][b],
			  edge_indices[c][a], edge_indices[a][c] },
			face_colors[4 + face_index], vertices, indices);
	}
}

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
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

	std::memcpy(created_allocation_info.pMappedData, data,
	            static_cast<size_t>(size));
	vmaFlushAllocation(context.allocator, result.allocation, 0, size);
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
	auto& context = graphics::internal::context;
	const VkShaderModuleCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code.size() * sizeof(uint32_t),
		.pCode = code.data(),
	};
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(context.device, &create_info, nullptr,
	                         &module) != VK_SUCCESS) {
		throw std::runtime_error("Failed to create Vulkan shader module");
	}
	return module;
}

bool createPipeline() {
	auto& context = graphics::internal::context;
	const std::vector<uint32_t> vertex_code =
		readShader("shaders/object.vert.spv");
	const std::vector<uint32_t> fragment_code =
		readShader("shaders/object.frag.spv");

	VkShaderModule vertex_module = VK_NULL_HANDLE;
	VkShaderModule fragment_module = VK_NULL_HANDLE;
	try {
		vertex_module = createShaderModule(vertex_code);
		fragment_module = createShaderModule(fragment_code);
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

	const VkVertexInputBindingDescription vertex_binding = {
		.binding = 0,
		.stride = sizeof(Vertex),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};
	const std::array<VkVertexInputAttributeDescription, 2> attributes = {{
		{
			.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, position),
		},
		{
			.location = 1,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Vertex, color),
		},
	}};
	const VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &vertex_binding,
		.vertexAttributeDescriptionCount =
			static_cast<uint32_t>(attributes.size()),
		.pVertexAttributeDescriptions = attributes.data(),
	};
	const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE,
	};
	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	const VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.depthClampEnable = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthBiasEnable = VK_FALSE,
		.lineWidth = 1.0F,
	};
	const VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = VK_FALSE,
	};
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable = VK_FALSE,
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
		.logicOpEnable = VK_FALSE,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};
	const std::array<VkDynamicState, 2> dynamic_states = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	const VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
		.pDynamicStates = dynamic_states.data(),
	};
	const VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(Mat4),
	};
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range,
	};
	if (vkCreatePipelineLayout(context.device, &layout_info, nullptr,
	                           &pipeline_layout) != VK_SUCCESS) {
		vkDestroyShaderModule(context.device, fragment_module, nullptr);
		vkDestroyShaderModule(context.device, vertex_module, nullptr);
		return false;
	}

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
		.layout = pipeline_layout,
		.renderPass = context.render_pass,
		.subpass = 0,
		.basePipelineHandle = VK_NULL_HANDLE,
		.basePipelineIndex = -1,
	};
	const VkResult result = vkCreateGraphicsPipelines(
		context.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);

	vkDestroyShaderModule(context.device, fragment_module, nullptr);
	vkDestroyShaderModule(context.device, vertex_module, nullptr);

	if (result != VK_SUCCESS) {
		vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
		pipeline_layout = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

void destroyResources() {
	auto& context = graphics::internal::context;
	if (pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(context.device, pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
	}
	if (pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
		pipeline_layout = VK_NULL_HANDLE;
	}
	if (mesh.index_buffer.handle != VK_NULL_HANDLE) {
		vmaDestroyBuffer(context.allocator, mesh.index_buffer.handle,
		                 mesh.index_buffer.allocation);
	}
	if (mesh.vertex_buffer.handle != VK_NULL_HANDLE) {
		vmaDestroyBuffer(context.allocator, mesh.vertex_buffer.handle,
		                 mesh.vertex_buffer.allocation);
	}
	mesh = {};
}

Mat4 createMvp(const VkExtent2D extent) {
	const float aspect = static_cast<float>(extent.width) /
	                     static_cast<float>(extent.height);
	const Mat4 scale = scaling(object_scale[0], object_scale[1],
	                           object_scale[2]);
	const Mat4 rotation = multiply(
		rotationZ(object_rotation[2] * pi / 180.0F),
		multiply(rotationY(object_rotation[1] * pi / 180.0F),
		         rotationX(object_rotation[0] * pi / 180.0F)));
	const Mat4 model = multiply(
		translation(object_position[0], object_position[1], object_position[2]),
		multiply(rotation, scale));
	const Mat4 view = translation(0.0F, 0.0F, -3.0F);
	const Mat4 projection = projection_mode == perspective_projection
		? perspective(55.0F * pi / 180.0F, aspect, 0.1F, 100.0F)
		: orthographic(1.5F, aspect, 0.1F, 100.0F);
	return multiply(projection, multiply(view, model));
}

} // namespace

bool initialize() {
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	createTruncatedTetrahedron(vertices, indices);

	if (!createBuffer(sizeof(Vertex) * vertices.size(),
	                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                  vertices.data(), mesh.vertex_buffer)) {
		std::cerr << "Failed to create vertex buffer\n";
		return false;
	}
	if (!createBuffer(sizeof(uint32_t) * indices.size(),
	                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                  indices.data(), mesh.index_buffer)) {
		std::cerr << "Failed to create index buffer\n";
		destroyResources();
		return false;
	}
	mesh.index_count = static_cast<uint32_t>(indices.size());

	try {
		if (!createPipeline()) {
			std::cerr << "Failed to create graphics pipeline\n";
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
	auto& context = graphics::internal::context;
	vkQueueWaitIdle(context.graphics_queue);
	destroyResources();
}

void update([[maybe_unused]] double time) {
	ImGui::SetNextWindowSize(ImVec2(420.0F, 330.0F), ImGuiCond_Once);
	ImGui::Begin("CG Lab 1");
	ImGui::TextUnformatted("Variant 4");
	ImGui::TextUnformatted("Truncated regular tetrahedron");
	ImGui::Separator();
	ImGui::TextUnformatted("Projection:");
	ImGui::RadioButton("Perspective", &projection_mode,
	                   perspective_projection);
	ImGui::SameLine();
	ImGui::RadioButton("Orthographic", &projection_mode,
	                   orthographic_projection);
	ImGui::Separator();
	ImGui::TextUnformatted("Object transform:");
	ImGui::SliderFloat3("Position", object_position.data(), -2.0F, 2.0F,
	                    "%.2f");
	ImGui::SliderFloat3("Rotation", object_rotation.data(), -180.0F, 180.0F,
	                    "%.1f deg");
	ImGui::SliderFloat3("Scale", object_scale.data(), 0.1F, 2.5F, "%.2f");
	if (ImGui::Button("Reset transform")) {
		object_position = {0.0F, 0.0F, 0.0F};
		object_rotation = {-20.0F, 30.0F, 0.0F};
		object_scale = {1.0F, 1.0F, 1.0F};
	}
	ImGui::Separator();
	ImGui::Text("Logical vertices: %d", 12);
	ImGui::Text("Faces: %d triangles + %d hexagons", 4, 4);
	ImGui::Text("Rendered triangles: %u", mesh.index_count / 3);
	ImGui::End();
}

void render(const graphics::internal::FrameData& fd) {
	auto& context = graphics::internal::context;

	vkResetCommandBuffer(fd.command_buffer, 0);
	const VkCommandBufferBeginInfo command_buffer_begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(fd.command_buffer, &command_buffer_begin);

	const VkClearValue clear_values[] = {
		{ .color = {{ 0.055F, 0.065F, 0.085F, 1.0F }} },
		{ .depthStencil = { 1.0F, 0 } },
	};
	const VkRenderPassBeginInfo render_pass_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = context.render_pass,
		.framebuffer = fd.framebuffer,
		.renderArea = { .extent = context.swapchain_extent },
		.clearValueCount = sizeof(clear_values) / sizeof(clear_values[0]),
		.pClearValues = clear_values,
	};
	vkCmdBeginRenderPass(fd.command_buffer, &render_pass_begin,
	                     VK_SUBPASS_CONTENTS_INLINE);

	const VkViewport viewport = {
		.x = 0.0F,
		.y = 0.0F,
		.width = static_cast<float>(context.swapchain_extent.width),
		.height = static_cast<float>(context.swapchain_extent.height),
		.minDepth = 0.0F,
		.maxDepth = 1.0F,
	};
	const VkRect2D scissor = { .extent = context.swapchain_extent };
	vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
	vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);

	vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                  pipeline);
	const VkDeviceSize vertex_offset = 0;
	vkCmdBindVertexBuffers(fd.command_buffer, 0, 1,
	                       &mesh.vertex_buffer.handle, &vertex_offset);
	vkCmdBindIndexBuffer(fd.command_buffer, mesh.index_buffer.handle,
	                     0, VK_INDEX_TYPE_UINT32);
	const Mat4 mvp = createMvp(context.swapchain_extent);
	vkCmdPushConstants(fd.command_buffer, pipeline_layout,
	                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), &mvp);
	vkCmdDrawIndexed(fd.command_buffer, mesh.index_count, 1, 0, 0, 0);

	vkCmdEndRenderPass(fd.command_buffer);
	vkEndCommandBuffer(fd.command_buffer);
}

} // namespace application
