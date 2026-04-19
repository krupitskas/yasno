module;

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl.h>
#include <d3dx12.h>
#include <wil/com.h>
#include <tiny_gltf.h>

#include <shader_structs.h>

export module system.gltf_loader;

import std;
import graphics.render_scene;
import graphics.primitive;
import renderer.dx_renderer;
import renderer.gpu_texture;
import renderer.command_queue;
import renderer.dx_types;
import system.string_helpers;
import system.math;
import system.application;
import system.logger;
import system.asserts;

export namespace ysn
{
	struct LoadingParameters
	{
		DirectX::XMMATRIX model_modifier = DirectX::XMMatrixIdentity(); // Applies matrix modifier for nodes and RTX BVH generation
	};

	bool LoadGltfFromFile(RenderScene& render_scene, const std::wstring& path, const LoadingParameters& loading_parameters);
}

module :private;

using namespace Microsoft::WRL;

struct LoadGltfContext
{
	wil::com_ptr<DxGraphicsCommandList> copy_cmd_list;
	std::vector<wil::com_ptr<ID3D12Resource>> staging_resources;
};

struct BuildMeshResult
{
	uint32_t mesh_indices_count = 0;
	uint32_t mesh_vertices_count = 0;
	uint32_t primitives_count = 0;
};

static DXGI_FORMAT GetSrgbFormat(DXGI_FORMAT format)
{
	switch (format)
	{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

		case DXGI_FORMAT_BC1_UNORM:
			return DXGI_FORMAT_BC1_UNORM_SRGB;

		case DXGI_FORMAT_BC2_UNORM:
			return DXGI_FORMAT_BC2_UNORM_SRGB;

		case DXGI_FORMAT_BC3_UNORM:
			return DXGI_FORMAT_BC3_UNORM_SRGB;

		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

		case DXGI_FORMAT_B8G8R8X8_UNORM:
			return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;

		case DXGI_FORMAT_BC7_UNORM:
			return DXGI_FORMAT_BC7_UNORM_SRGB;

		default:
			return format;
	}
}

// TODO: Move to ColorBuffer
// Compute the number of texture levels needed to reduce to 1x1.  This uses
// _BitScanReverse to find the highest set bit.  Each dimension reduces by
// half and truncates bits.  The dimension 256 (0x100) has 9 mip levels, same
// as the dimension 511 (0x1FF).
static inline uint32_t ComputeNumMips(uint32_t Width, uint32_t Height)
{
	uint32_t HighBit;
	_BitScanReverse((unsigned long*)&HighBit, Width | Height);
	return HighBit + 1;
}

void FindAllSrgbTextures(std::unordered_set<int>& srgb_textures, const tinygltf::Model& gltf_model)
{
	for (const tinygltf::Material& gltf_material : gltf_model.materials)
	{
		const tinygltf::PbrMetallicRoughness& pbr_material = gltf_material.pbrMetallicRoughness;

		const tinygltf::TextureInfo& base_color = pbr_material.baseColorTexture;

		if (base_color.index >= 0)
		{
			const tinygltf::Texture& texture = gltf_model.textures[base_color.index];

			if (srgb_textures.contains(texture.source))
			{
				ysn::LogError << "Base color texture sRGB search collision\n";
			}

			srgb_textures.emplace(texture.source);
		}

		const tinygltf::TextureInfo& emissive = gltf_material.emissiveTexture;
		if (emissive.index >= 0)
		{
			const tinygltf::Texture& texture = gltf_model.textures[emissive.index];

			if (srgb_textures.contains(texture.source))
			{
				ysn::LogError << "Emissive texture sRGB search collision\n";
			}

			srgb_textures.emplace(texture.source);
		}
	}
}

static bool BuildImages(ysn::Model& model, LoadGltfContext& build_context, const tinygltf::Model& gltf_model)
{
	auto dx_renderer = ysn::Application::Get().GetRenderer();

	HRESULT hr = S_OK;

	// Mark all albedo and emissive images as srgb
	std::unordered_set<int> srgb_textures;
	FindAllSrgbTextures(srgb_textures, gltf_model);

	for (int i = 0; i < gltf_model.images.size(); i++)
	{
		const tinygltf::Image& image = gltf_model.images[i];

		const uint32_t num_mips = ComputeNumMips(image.width, image.height);
		bool is_srgb = srgb_textures.contains(i);

		ID3D12Resource* dst_texture = nullptr;
		{
			D3D12_HEAP_PROPERTIES heap_properties = {};
			heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
			heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			// TODO: HDR textures?
			D3D12_RESOURCE_DESC resource_desc = {};
			resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			resource_desc.Alignment = 0;
			resource_desc.Width = image.width;
			resource_desc.Height = image.height;
			resource_desc.DepthOrArraySize = 1;
			resource_desc.MipLevels = static_cast<UINT16>(num_mips);
			resource_desc.Format = is_srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; // GetSrgbFormat
			resource_desc.SampleDesc = { 1, 0 };
			resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // FIXME: UAV / SRV?

			hr = dx_renderer->GetDevice()->CreateCommittedResource(
				&heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dst_texture));

			if (hr != S_OK)
			{
				ysn::LogError << "Can't allocate GLTF dst texture\n";
				return false;
			}

			ysn::GpuTexture new_texture(dst_texture);
			new_texture.SetName(image.uri);
			new_texture.is_srgb = is_srgb;
			new_texture.num_mips = num_mips;
			new_texture.width = image.width;
			new_texture.height = image.height;

			const auto srv_handle = dx_renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();

			D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.Format = is_srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
			srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv_desc.Texture2D.MipLevels = num_mips;

			dx_renderer->GetDevice()->CreateShaderResourceView(new_texture.Resource(), &srv_desc, srv_handle.cpu);

			new_texture.descriptor_handle = srv_handle;

			model.textures.push_back(new_texture);
		}

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;

		UINT row_count = 0;
		UINT64 row_size = 0;
		UINT64 size = 0;

		const D3D12_RESOURCE_DESC texture_desc = dst_texture->GetDesc();

		dx_renderer->GetDevice()->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, &row_count, &row_size, &size);

		wil::com_ptr<ID3D12Resource> src_resource;
		{
			D3D12_HEAP_PROPERTIES heap_properties = {};
			heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
			heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC resource_desc = {};
			resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			resource_desc.Alignment = 0;
			resource_desc.Width = size;
			resource_desc.Height = 1;
			resource_desc.DepthOrArraySize = 1;
			resource_desc.MipLevels = 1;
			resource_desc.Format = DXGI_FORMAT_UNKNOWN;
			resource_desc.SampleDesc = { 1, 0 };
			resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

			hr = dx_renderer->GetDevice()->CreateCommittedResource(
				&heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&src_resource));

			if (hr != S_OK)
			{
				ysn::LogError << "Can't allocate GLTF src texture resource\n";
				return false;
			}

			build_context.staging_resources.push_back(src_resource);

			void* data_ptr;
			hr = src_resource->Map(0, nullptr, &data_ptr);

			if (hr != S_OK)
			{
				ysn::LogError << "Can't map GLTF src texture resource\n";
				return false;
			}

			// Move data from gltf texture to resource
			for (UINT row = 0; row != row_count; row++)
			{
				memcpy(
					static_cast<uint8_t*>(data_ptr) + row_size * row,
					&image.image[0] + image.width * image.component * row,
					image.width * image.component);
			}

			// TODO: try to unmap here?
		}

		D3D12_TEXTURE_COPY_LOCATION dst_copy_location = {};
		dst_copy_location.pResource = dst_texture;
		dst_copy_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst_copy_location.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION src_copy_location = {};
		src_copy_location.pResource = src_resource.get();
		src_copy_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src_copy_location.PlacedFootprint = footprint;

		// Copy texture to GPU
		build_context.copy_cmd_list->CopyTextureRegion(&dst_copy_location, 0, 0, 0, &src_copy_location, nullptr);
	}

	return true;
}

static void BuildSamplerDescs(ysn::Model& model, const tinygltf::Model& gltf_model)
{
	model.sampler_descs.reserve(gltf_model.samplers.size());

	for (const tinygltf::Sampler& gltf_sampler : gltf_model.samplers)
	{
		D3D12_SAMPLER_DESC sampler_desc = {};

		switch (gltf_sampler.minFilter)
		{
			case TINYGLTF_TEXTURE_FILTER_NEAREST:
				if (gltf_sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
				}
				else
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				}
				break;
			case TINYGLTF_TEXTURE_FILTER_LINEAR:
				if (gltf_sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
				}
				else
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
				}
				break;
			case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
				if (gltf_sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
				}
				else
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
				}
				break;
			case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:
				if (gltf_sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
				}
				else
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
				}
				break;
			case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
				if (gltf_sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
				}
				else
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
				}
				break;
			case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR:
				if (gltf_sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST)
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
				}
				else
				{
					sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				}
				break;
			default:
			{
				sampler_desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
			}
			break;
		}

		auto to_texture_address_wrap = [](int wrap)
		{
			switch (wrap)
			{
				case TINYGLTF_TEXTURE_WRAP_REPEAT:
				{
					return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				}
				case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
				{
					return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				}
				case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
				{
					return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
				}
				default:
				{
					ysn::LogInfo << "GLTF sampler desc found incorrect wrap mode\n";
					return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				}
			}
		};

		sampler_desc.AddressU = to_texture_address_wrap(gltf_sampler.wrapS);
		sampler_desc.AddressV = to_texture_address_wrap(gltf_sampler.wrapT);
		sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler_desc.MaxLOD = 256;

		model.sampler_descs.push_back(sampler_desc);
	}
}

static bool BuildPrimitiveTopology(ysn::Primitive& primitive, int gltf_primitive_mode)
{
	switch (gltf_primitive_mode)
	{
		case TINYGLTF_MODE_POINTS:
			primitive.topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
			break;
		case TINYGLTF_MODE_LINE:
			primitive.topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
			break;
		case TINYGLTF_MODE_LINE_STRIP:
			primitive.topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
			break;
		case TINYGLTF_MODE_TRIANGLES:
			primitive.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			break;
		case TINYGLTF_MODE_TRIANGLE_STRIP:
			primitive.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
			break;
		default:
			ysn::LogError << "GLTF has unknown primitive topology\n";
			return false;
	}

	return true;
}

static uint32_t BuildIndexBuffer(ysn::Primitive& primitive, int indices_index, const tinygltf::Model& gltf_model)
{
	if (indices_index >= 0)
	{
		const tinygltf::Accessor& index_accessor = gltf_model.accessors[indices_index];

		primitive.indices.resize(index_accessor.count);

		const tinygltf::BufferView& index_buffer_view = gltf_model.bufferViews[index_accessor.bufferView];
		const tinygltf::Buffer& index_buffer = gltf_model.buffers[index_buffer_view.buffer];

		const UINT8* index_buffer_address = index_buffer.data.data();
		const UINT8* base_address = index_buffer_address + index_buffer_view.byteOffset + index_accessor.byteOffset;

		const int index_stride =
			tinygltf::GetComponentSizeInBytes(index_accessor.componentType) * tinygltf::GetNumComponentsInType(index_accessor.type);

		if (index_stride == 1)
		{
			std::vector<UINT8> quarter;
			quarter.resize(index_accessor.count);

			memcpy(quarter.data(), base_address, (index_accessor.count * index_stride));

			// Convert quarter precision indices to full precision
			for (size_t i = 0; i < index_accessor.count; i++)
			{
				primitive.indices[i] = quarter[i];
			}
		}
		else if (index_stride == 2)
		{
			std::vector<UINT16> half;
			half.resize(index_accessor.count);

			memcpy(half.data(), base_address, (index_accessor.count * index_stride));

			// Convert half precision indices to full precision
			for (size_t i = 0; i < index_accessor.count; i++)
			{
				primitive.indices[i] = half[i];
			}
		}
		else
		{
			memcpy(primitive.indices.data(), base_address, (index_accessor.count * index_stride));
		}

		return static_cast<uint32_t>(index_accessor.count);
	}
	else
	{
		ysn::LogError << "GLTF primitive don't have indices\n";
	}

	return 0;
}

static uint32_t BuildVertexBuffer(ysn::Primitive& primitive, const tinygltf::Primitive& gltf_primitive, const tinygltf::Model& gltf_model)
{
	int position_index = -1;
	int normal_index = -1;
	//int tangent_index = -1;
	int uv0_index = -1;

	if (gltf_primitive.attributes.contains("POSITION"))
	{
		position_index = gltf_primitive.attributes.at("POSITION");
	}

	if (gltf_primitive.attributes.contains("NORMAL"))
	{
		normal_index = gltf_primitive.attributes.at("NORMAL");
	}

	//if (gltf_primitive.attributes.contains("TANGENT"))
	//{
	//	tangent_index = gltf_primitive.attributes.at("TANGENT");
	//}

	if (gltf_primitive.attributes.contains("TEXCOORD_0"))
	{
		uv0_index = gltf_primitive.attributes.at("TEXCOORD_0");
	}

	// AABB
	if (position_index > -1)
	{
		const std::vector<double>& min = gltf_model.accessors[position_index].minValues;
		const std::vector<double>& max = gltf_model.accessors[position_index].maxValues;

		primitive.bbox.min = { (float)min[0], (float)min[1], (float)min[2] };
		primitive.bbox.max = { (float)max[0], (float)max[1], (float)max[2] };
	}

	// Vertex positions
	const tinygltf::Accessor& position_accessor = gltf_model.accessors[position_index];
	const tinygltf::BufferView& position_buffer_view = gltf_model.bufferViews[position_accessor.bufferView];
	const tinygltf::Buffer& position_buffer = gltf_model.buffers[position_buffer_view.buffer];
	const UINT8* position_buffer_address = position_buffer.data.data();
	const int position_stride = tinygltf::GetComponentSizeInBytes(position_accessor.componentType) *
		tinygltf::GetNumComponentsInType(position_accessor.type);
	ysn::AssertMsg(position_stride == 12, "GLTF model vertex position stride not equals 12");

	// Vertex normals
	tinygltf::Accessor normal_accessor;
	tinygltf::BufferView normal_buffer_view;
	const UINT8* normal_buffer_address = nullptr;
	int normal_stride = -1;
	if (normal_index > -1)
	{
		normal_accessor = gltf_model.accessors[normal_index];
		normal_buffer_view = gltf_model.bufferViews[normal_accessor.bufferView];
		const tinygltf::Buffer& normal_buffer = gltf_model.buffers[normal_buffer_view.buffer];
		normal_buffer_address = normal_buffer.data.data();
		normal_stride = tinygltf::GetComponentSizeInBytes(normal_accessor.componentType) *
			tinygltf::GetNumComponentsInType(normal_accessor.type);
		ysn::AssertMsg(normal_stride == 12, "GLTF model vertex normal stride not equals 12");
	}

	// Vertex tangents
	//tinygltf::Accessor tangent_accessor;
	//tinygltf::BufferView tangent_buffer_view;
	//const UINT8* tangent_buffer_address = nullptr;
	//int tangent_stride = -1;
	//if (tangent_index > -1)
	//{
	//	tangent_accessor = gltf_model.accessors[tangent_index];
	//	tangent_buffer_view = gltf_model.bufferViews[tangent_accessor.bufferView];
	//	const tinygltf::Buffer& tangent_buffer = gltf_model.buffers[tangent_buffer_view.buffer];
	//	tangent_buffer_address = tangent_buffer.data.data();
	//	tangent_stride = tinygltf::GetComponentSizeInBytes(tangent_accessor.componentType) *
	//		tinygltf::GetNumComponentsInType(tangent_accessor.type);
	//	ysn::AssertMsg(tangent_stride == 16, "GLTF model vertex tangent stride not equals 16");
	//}

	// Vertex texture coordinates
	tinygltf::Accessor uv0_accessor;
	tinygltf::BufferView uv0_buffer_view;
	const UINT8* uv0_buffer_address = nullptr;
	int uv0_stride = -1;
	if (uv0_index > -1)
	{
		uv0_accessor = gltf_model.accessors[uv0_index];
		uv0_buffer_view = gltf_model.bufferViews[uv0_accessor.bufferView];
		const tinygltf::Buffer& uv0Buffer = gltf_model.buffers[uv0_buffer_view.buffer];
		uv0_buffer_address = uv0Buffer.data.data();
		uv0_stride =
			tinygltf::GetComponentSizeInBytes(uv0_accessor.componentType) * tinygltf::GetNumComponentsInType(uv0_accessor.type);
		ysn::AssertMsg(uv0_stride == 8, "GLTF model vertex uv0 stride not equals 12");
	}

	primitive.vertices.reserve(position_accessor.count);

	// Get the vertex data
	for (size_t vertex_index = 0; vertex_index < position_accessor.count; vertex_index++)
	{
		ysn::Vertex v;

		// position
		{
			const UINT8* address = position_buffer_address + position_buffer_view.byteOffset + position_accessor.byteOffset +
				(vertex_index * position_stride);
			memcpy(&v.position, address, position_stride);
		}

		if (normal_index > -1)
		{
			const UINT8* address =
				normal_buffer_address + normal_buffer_view.byteOffset + normal_accessor.byteOffset + (vertex_index * normal_stride);
			memcpy(&v.normal, address, normal_stride);
		}

		//if (tangent_index > -1)
		//{
		//	const UINT8* address = tangent_buffer_address + tangent_buffer_view.byteOffset + tangent_accessor.byteOffset +
		//		(vertex_index * tangent_stride);
		//	memcpy(&v.tangent, address, tangent_stride);
		//}

		if (uv0_index > -1)
		{
			const UINT8* address =
				uv0_buffer_address + uv0_buffer_view.byteOffset + uv0_accessor.byteOffset + (vertex_index * uv0_stride);
			memcpy(&v.uv0, address, uv0_stride);

			// TODO: Add UV adj
		}

		primitive.vertices.push_back(v);
	}

	return static_cast<uint32_t>(position_accessor.count);
}

static BuildMeshResult BuildMeshes(ysn::Model& model, const tinygltf::Model& gltf_model)
{
	BuildMeshResult result;

	uint32_t primitive_index_count = 0;

	for (const tinygltf::Mesh& gltf_mesh : gltf_model.meshes)
	{
		ysn::Mesh mesh;
		mesh.name = gltf_mesh.name;

		for (const tinygltf::Primitive& gltf_primitive : gltf_mesh.primitives)
		{
			ysn::Primitive primitive;
			result.mesh_vertices_count += BuildVertexBuffer(primitive, gltf_primitive, gltf_model);
			result.mesh_indices_count += BuildIndexBuffer(primitive, gltf_primitive.indices, gltf_model);
			BuildPrimitiveTopology(primitive, gltf_primitive.mode);

			primitive.material_id = gltf_primitive.material;
			primitive.index = primitive_index_count;

			mesh.primitives.push_back(primitive);

			primitive_index_count++;
		}

		result.primitives_count += static_cast<uint32_t>(gltf_mesh.primitives.size());

		model.meshes.push_back(mesh);
	}

	return result;
}

static void BuildNodes(ysn::Model& model, const tinygltf::Model& gltf_model, const ysn::LoadingParameters& loading_parameters)
{
	auto dx_renderer = ysn::Application::Get().GetRenderer();

	for (const tinygltf::Node& gltf_node : gltf_model.nodes)
	{
		if(gltf_node.mesh == -1)
		{
			// TODO: Support cameras and other shit
			continue;
		}

		DirectX::XMMATRIX matrix_result;

		if (gltf_node.matrix.empty())
		{
			DirectX::XMVECTOR quaternion = DirectX::XMQuaternionIdentity();
			DirectX::XMVECTOR translation = DirectX::XMVectorZero();
			DirectX::XMVECTOR scaling = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);

			if (gltf_node.rotation.size() == 4) {
				quaternion = DirectX::XMVectorSet(
					static_cast<float>(gltf_node.rotation[0]),
					static_cast<float>(gltf_node.rotation[1]),
					static_cast<float>(gltf_node.rotation[2]),
					static_cast<float>(gltf_node.rotation[3])
				);
			}

			if (gltf_node.translation.size() == 3) {
				translation = DirectX::XMVectorSet(
					static_cast<float>(gltf_node.translation[0]),
					static_cast<float>(gltf_node.translation[1]),
					static_cast<float>(gltf_node.translation[2]),
					1.0f
				);
			}

			if (gltf_node.scale.size() == 3) {
				scaling = DirectX::XMVectorSet(
					static_cast<float>(gltf_node.scale[0]),
					static_cast<float>(gltf_node.scale[1]),
					static_cast<float>(gltf_node.scale[2]),
					0.0f
				);
			}

			// Compute matrices
			DirectX::XMMATRIX rotation_matrix = DirectX::XMMatrixRotationQuaternion(quaternion);
			DirectX::XMMATRIX scaling_matrix = DirectX::XMMatrixScalingFromVector(scaling);
			DirectX::XMMATRIX translation_matrix = DirectX::XMMatrixTranslationFromVector(translation);

			matrix_result = scaling_matrix * rotation_matrix * translation_matrix;

			model.transforms.push_back(matrix_result * loading_parameters.model_modifier);
		}
		else
		{
			std::array<float, 16> float_array;

			// Convert to floats
			for (int i = 0; i < gltf_node.matrix.size(); i++)
			{
				float_array[i] = static_cast<float>(gltf_node.matrix[i]);
			}

			DirectX::XMMATRIX model_matrix(float_array.data());

			matrix_result = model_matrix;

			model.transforms.push_back(model_matrix * loading_parameters.model_modifier);
		}
	}

	for (int i = 0; i < gltf_model.nodes.size(); i++)
	{
		for(const auto& child_index : gltf_model.nodes[i].children)
		{
			// TODO: Support cameras and other shit
			model.transforms[gltf_model.nodes[child_index].mesh] *= model.transforms[i];
		}
	}
}

static uint32_t BuildMaterials(ysn::Model& model, const tinygltf::Model& gltf_model)
{
	auto dx_renderer = ysn::Application::Get().GetRenderer();

	for (const tinygltf::Material& gltf_material : gltf_model.materials)
	{
		ysn::Material material(gltf_material.name);

		if (gltf_material.alphaMode == "BLEND")
		{
			material.blend_desc.RenderTarget[0].BlendEnable = true;
			material.blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
			material.blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			material.blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
			material.blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
			material.blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
			material.blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
			material.blend_desc.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
		}
		else if (gltf_material.alphaMode == "MASK")
		{
			// TODO(gltf): Check other alpha modes
			// assert( false );
		}
		else if (gltf_material.alphaMode == "OPAQUE")
		{
			material.blend_desc.RenderTarget[0].BlendEnable = false;

			// TODO(gltf): Check other alpha modes
			// assert( false );
		}

		material.blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		if (gltf_material.doubleSided)
		{
			material.rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE;
		}
		else
		{
			material.rasterizer_desc.CullMode = D3D12_CULL_MODE_BACK;
		}

		material.rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
		material.rasterizer_desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		material.rasterizer_desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		material.rasterizer_desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		material.rasterizer_desc.ForcedSampleCount = 0;
		material.rasterizer_desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
		material.rasterizer_desc.FrontCounterClockwise = TRUE; 

		const tinygltf::PbrMetallicRoughness& gltf_pbr_material = gltf_material.pbrMetallicRoughness;
		SurfaceShaderParameters shader_parameters = {};

		DirectX::XMFLOAT4& base_color_factor = shader_parameters.base_color_factor;
		base_color_factor.x = static_cast<float>(gltf_pbr_material.baseColorFactor[0]);
		base_color_factor.y = static_cast<float>(gltf_pbr_material.baseColorFactor[1]);
		base_color_factor.z = static_cast<float>(gltf_pbr_material.baseColorFactor[2]);
		base_color_factor.w = static_cast<float>(gltf_pbr_material.baseColorFactor[3]);

		shader_parameters.metallic_factor = static_cast<float>(gltf_pbr_material.metallicFactor);
		shader_parameters.roughness_factor = static_cast<float>(gltf_pbr_material.roughnessFactor);

		if (gltf_pbr_material.baseColorTexture.index >= 0)
		{
			shader_parameters.texture_enable_bitmask |= 1 << ALBEDO_ENABLED_BIT;

			const tinygltf::Texture& gltf_texture = gltf_model.textures[gltf_pbr_material.baseColorTexture.index];

			ysn::GpuTexture& texture = model.textures[gltf_texture.source]; // TODO: use it for srgb?
			texture.is_srgb = true;

			shader_parameters.albedo_texture_index =
				dx_renderer->GetCbvSrvUavDescriptorHeap()->GetDescriptorIndex(texture.descriptor_handle);

			texture.SetName("Albedo Texture");
		}

		if (gltf_material.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0)
		{
			shader_parameters.texture_enable_bitmask |= 1 << METALLIC_ROUGHNESS_ENABLED_BIT;

			const tinygltf::Texture& gltfTexture =
				gltf_model.textures[gltf_material.pbrMetallicRoughness.metallicRoughnessTexture.index];
			ysn::GpuTexture& texture = model.textures[gltfTexture.source];

			shader_parameters.metallic_roughness_texture_index =
				dx_renderer->GetCbvSrvUavDescriptorHeap()->GetDescriptorIndex(texture.descriptor_handle);

			texture.SetName("Metallic Roughness Texture");
		}

		if (gltf_material.normalTexture.index >= 0)
		{
			shader_parameters.texture_enable_bitmask |= 1 << NORMAL_ENABLED_BIT;

			const tinygltf::Texture& gltf_texture = gltf_model.textures[gltf_material.normalTexture.index];
			ysn::GpuTexture& texture = model.textures[gltf_texture.source];

			shader_parameters.normal_texture_index =
				dx_renderer->GetCbvSrvUavDescriptorHeap()->GetDescriptorIndex(texture.descriptor_handle);

			texture.SetName("Normals Texture");
		}

		if (gltf_material.occlusionTexture.index >= 0)
		{
			shader_parameters.texture_enable_bitmask |= 1 << OCCLUSION_ENABLED_BIT;

			const tinygltf::Texture& gltf_texture = gltf_model.textures[gltf_material.occlusionTexture.index];
			ysn::GpuTexture& texture = model.textures[gltf_texture.source];

			shader_parameters.occlusion_texture_index =
				dx_renderer->GetCbvSrvUavDescriptorHeap()->GetDescriptorIndex(texture.descriptor_handle);

			texture.SetName("Occlusion Texture");
		}

		if (gltf_material.emissiveTexture.index >= 0)
		{
			shader_parameters.texture_enable_bitmask |= 1 << EMISSIVE_ENABLED_BIT;

			const tinygltf::Texture& gltf_texture = gltf_model.textures[gltf_material.emissiveTexture.index];
			ysn::GpuTexture& texture = model.textures[gltf_texture.source];

			shader_parameters.emissive_texture_index =
				dx_renderer->GetCbvSrvUavDescriptorHeap()->GetDescriptorIndex(texture.descriptor_handle);

			texture.SetName("Emissive Texture");
		}

		model.materials.push_back(material);
		model.shader_parameters.push_back(shader_parameters);
	}

	return static_cast<uint32_t>(gltf_model.materials.size());
}

namespace ysn
{
	bool ReadModel(RenderScene& render_scene, Model& model, const tinygltf::Model& gltf_model, const LoadingParameters& loading_parameters)
	{
		auto dx_renderer = Application::Get().GetRenderer();
		auto command_queue = Application::Get().GetDirectQueue();

		const auto cmd_list_result = command_queue->GetCommandList("GLTF upload");

		if (!cmd_list_result.has_value())
			return false;

		LoadGltfContext load_gltf_context;
		load_gltf_context.staging_resources.reserve(256);
		load_gltf_context.copy_cmd_list = cmd_list_result.value();

		if (!BuildImages(model, load_gltf_context, gltf_model))
		{
			ysn::LogError << "GLTF loader can't build materials\n";
			return false;
		}

		render_scene.materials_count += BuildMaterials(model, gltf_model);

		BuildSamplerDescs(model, gltf_model);
		const BuildMeshResult mesh_result = BuildMeshes(model, gltf_model);
		BuildNodes(model, gltf_model, loading_parameters);

		command_queue->CloseCommandList(load_gltf_context.copy_cmd_list);

		auto fence_value = command_queue->ExecuteCommandLists();

		if (!fence_value.has_value())
		{
			return false;
		}

		command_queue->WaitForFenceValue(fence_value.value());

		render_scene.indices_count += mesh_result.mesh_indices_count;
		render_scene.vertices_count += mesh_result.mesh_vertices_count;
		render_scene.primitives_count += mesh_result.primitives_count;

		return true;
	}

	bool LoadGltfFromFile(RenderScene& render_scene, const std::wstring& load_path, const LoadingParameters& loading_parameters)
	{
		tinygltf::Model gltf_model;
		tinygltf::TinyGLTF gltf_loader;

		std::string error_str;
		std::string warning_str;
		const std::string load_path_str = ysn::WStringToString(load_path);

		const bool result = gltf_loader.LoadASCIIFromFile(&gltf_model, &error_str, &warning_str, load_path_str.c_str());

		ysn::LogInfo << "GLTF loading: " << load_path_str << "\n";

		if (!warning_str.empty())
		{
			ysn::LogInfo << "GLTF loading: " << warning_str.c_str() << "\n";
		}

		if (!error_str.empty())
		{
			ysn::LogError << "GLTF loading: " << error_str.c_str() << "\n";
		}

		if (!result)
		{
			ysn::LogError << "Failed to read: " << load_path_str << "\n";
			return false;
		}

		Model model;

		if (!ReadModel(render_scene, model, gltf_model, loading_parameters))
		{
			ysn::LogInfo << "GLTF can't load model\n";
			return false;
		}

		render_scene.models.push_back(model);

		ysn::LogInfo << "GLTF loaded successfully: " << load_path_str << "\n";

		return true;
	}
}
