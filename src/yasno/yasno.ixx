module;

#include <DirectXMath.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3dx12.h>
#include <pix3.h>

#include <shader_structs.h>

#include <imgui.h>
#include <ImGuizmo.h>

export module yasno;

import std;
import yasno.camera_controller;
import graphics.techniques.convert_to_cubemap;
import graphics.engine_stats;
import graphics.techniques.shadow_map_pass;
import graphics.techniques.tonemap_pass;
import graphics.techniques.raytracing_pass;
import graphics.techniques.skybox_pass;
import graphics.techniques.forward_pass;
import graphics.techniques.generate_mips_pass;
import graphics.techniques.debug_renderer;
import graphics.techniques.convolve_cubemap;
import graphics.techniques.workgraph;
import graphics.techniques.volumetric_fog;
import graphics.render_scene;
import graphics.mesh;
import renderer.dx_renderer;
import renderer.gpu_buffer;
import renderer.command_queue;
import renderer.gpu_texture;
import renderer.rtx_context;
import renderer.command_queue;
import renderer.gpu_pixel_buffer;
import system.math;
import system.filesystem;
import system.application;
import system.gltf_loader;
import system.gui;
import system.game_input;
import system.helpers;
import system.logger;
import system.asserts;
import system.string_helpers;
import system.profiler;

enum class SkyboxCubemap : uint8_t
{
	Cubemap,
	Irradiance,
	Radiance
};

export namespace ysn
{
	class Yasno : public Game
	{
	public:
		Yasno(const std::wstring& name, int width, int height, bool vsync = false);

		std::expected<bool, std::string> LoadContent() override;
		void UnloadContent() override;
		void Destroy() override;

	protected:
		void OnUpdate(UpdateEventArgs& e) override;
		void OnRender(RenderEventArgs& e) override;
		void RenderUi();
		void OnResize(ResizeEventArgs& e) override;

	private:
		GameInput game_input;
		RenderScene m_render_scene;

		bool CreateGpuCameraBuffer();
		bool CreateGpuSceneParametersBuffer();

		void UpdateGpuCameraBuffer();
		void UpdateGpuSceneParametersBuffer();

		bool UpdateBufferResource(
			wil::com_ptr<ID3D12GraphicsCommandList2> commandList,
			ID3D12Resource** pDestinationResource,
			ID3D12Resource** pIntermediateResource,
			size_t numElements,
			size_t elementSize,
			const void* bufferData,
			D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);

		bool ResizeDepthBuffer(int width, int height);
		bool ResizeHdrBuffer(int width, int height);
		bool ResizeBackBuffer(int width, int height);

		uint64_t m_fence_values[Window::BufferCount] = {};

		wil::com_ptr<ID3D12Resource> m_depth_buffer;
		wil::com_ptr<ID3D12Resource> m_back_buffer;
		// Two HDR buffers for history
		wil::com_ptr<ID3D12Resource> m_scene_color_buffer;
		wil::com_ptr<ID3D12Resource> m_scene_color_buffer_1;

		//wil::com_ptr<ID3D12Resource> m_velocity_buffer;
		DescriptorHandle m_velocity_descriptor_handle;

		DescriptorHandle m_hdr_uav_descriptor_handle;
		DescriptorHandle m_scene_color_buffer_1_handle;
		DescriptorHandle m_backbuffer_uav_descriptor_handle;
		DescriptorHandle m_depth_dsv_descriptor_handle;
		DescriptorHandle m_depth_srv_descriptor_handle;
		DescriptorHandle m_hdr_rtv_descriptor_handle;

		RtxContext m_rtx_context;

		D3D12_VIEWPORT m_viewport;
		D3D12_RECT m_scissors_rect;

		GpuTexture m_environment_texture;

		// Should be single struct
		GpuPixelBuffer3D m_cubemap_texture;
		GpuPixelBuffer3D m_irradiance_cubemap_texture;
		GpuPixelBuffer3D m_radiance_cubemap_texture;
		SkyboxCubemap m_active_skybox_cubemap = SkyboxCubemap::Cubemap;

		wil::com_ptr<ID3D12Resource> m_camera_gpu_buffer;
		wil::com_ptr<ID3D12Resource> m_scene_parameters_gpu_buffer;

		bool m_is_raster = true;
		bool m_is_indirect = false;
		bool m_is_raster_pressed = false;

		bool m_is_content_loaded = false;
		bool m_is_first_frame = true;
		bool m_reset_rtx_accumulation = true;
		bool m_is_rtx_accumulation_enabled = false;
		uint32_t m_pathtracing_frame_number = 0;

		// Techniques
		ForwardPass m_forward_pass;
		ShadowMapPass m_shadow_pass;
		TonemapPostprocess m_tonemap_pass;
		PathtracingPass m_ray_tracing_pass;
		ConvertToCubemap m_convert_to_cubemap_pass;
		SkyboxPass m_skybox_pass;
		GenerateMipsPass m_generate_mips_pass;
		DebugRenderer m_debug_renderer;
		ConvolveCubemap m_cubemap_filter_pass;
		VolumetricFog m_volumetric_fog;
	};
}

module :private;

namespace ysn
{
	std::expected<GpuPixelBuffer3D, std::string> CreateCubemapTexture(std::string name, uint32_t size, bool create_mips = false)
	{
		auto renderer = Application::Get().GetRenderer();

		const auto format = DXGI_FORMAT_R16G16B16A16_FLOAT;

		D3D12_RESOURCE_DESC texture_desc = {};
		texture_desc.Format = format;
		texture_desc.Width = size;
		texture_desc.Height = size;
		texture_desc.MipLevels = create_mips ? static_cast<UINT16>(ComputeNumMips(size, size)) : 1;
		texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		texture_desc.DepthOrArraySize = 6;
		texture_desc.SampleDesc.Count = 1;
		texture_desc.SampleDesc.Quality = 0;
		texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

		const auto heap_properties_default = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

		ID3D12Resource* result = nullptr;

		if (renderer->GetDevice()->CreateCommittedResource(
			&heap_properties_default, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&result)) !=
			S_OK)
		{
			return std::unexpected("Can't create cubemap resource\n");
		}

		GpuPixelBuffer3D cubemap_gpu_texture;
		cubemap_gpu_texture.SetResource(result);
		cubemap_gpu_texture.SetName(name);
		cubemap_gpu_texture.GenerateRTVs();

		return cubemap_gpu_texture;
	}

	Yasno::Yasno(const std::wstring& name, int width, int height, bool vsync) :
		Game(name, width, height, vsync),
		m_viewport(CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height))),
		m_scissors_rect(CD3DX12_RECT(0, 0, LONG_MAX, LONG_MAX))
	{
	}

	void Yasno::UnloadContent()
	{
		m_is_content_loaded = false;
	}

	void Yasno::Destroy()
	{
		Game::Destroy();
		ShutdownImgui();
	}

	bool Yasno::UpdateBufferResource(
		wil::com_ptr<ID3D12GraphicsCommandList2> commandList,
		ID3D12Resource** destination_resource,
		ID3D12Resource** intermediate_resource,
		size_t num_elements,
		size_t element_size,
		const void* buffer_data,
		D3D12_RESOURCE_FLAGS flags)
	{
		auto device = Application::Get().GetDevice();

		size_t bufferSize = num_elements * element_size;

		const auto gpuBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize, flags);
		const auto heap_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

		// Create a committed resource for the GPU resource in a default heap.
		if (auto result = device->CreateCommittedResource(
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&gpuBufferDesc,
			D3D12_RESOURCE_STATE_COMMON, // TODO(task): should be here
			// D3D12_RESOURCE_STATE_COPY_DEST?
			nullptr,
			IID_PPV_ARGS(destination_resource));
			result != S_OK)
		{
			LogFatal << "Can't create buffer " << ConvertHrToString(result) << " \n";
			return false;
		}

		// Create an committed resource for the upload.
		if (buffer_data)
		{
			const auto gpuUploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
			const auto uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

			if (auto result = device->CreateCommittedResource(
				&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &gpuUploadBufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(intermediate_resource));
				result != S_OK)
			{
				LogFatal << "Can't create upload buffer " << ConvertHrToString(result) << " \n";
				return false;
			}

			D3D12_SUBRESOURCE_DATA subresource_ata = {};
			subresource_ata.pData = buffer_data;
			subresource_ata.RowPitch = bufferSize;
			subresource_ata.SlicePitch = bufferSize;

			UpdateSubresources(commandList.get(), *destination_resource, *intermediate_resource, 0, 0, 1, &subresource_ata);
		}

		return true;
	}

	bool Yasno::CreateGpuCameraBuffer()
	{
		const auto device = Application::Get().GetDevice();

		D3D12_HEAP_PROPERTIES heap_properties = {};
		heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
		heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC resource_desc = {};
		resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resource_desc.Width = GetGpuSize<CameraParameters>();
		resource_desc.Height = 1;
		resource_desc.DepthOrArraySize = 1;
		resource_desc.MipLevels = 1;
		resource_desc.Format = DXGI_FORMAT_UNKNOWN;
		resource_desc.SampleDesc = { 1, 0 };
		resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		auto hr = device->CreateCommittedResource(
			&heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_camera_gpu_buffer));

		if (hr != S_OK)
		{
			LogError << "Can't create camera GPU buffer\n";
			return false;
		}

		return true;
	}

	bool Yasno::CreateGpuSceneParametersBuffer()
	{
		const auto device = Application::Get().GetDevice();

		D3D12_HEAP_PROPERTIES heap_properties = {};
		heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
		heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC resource_desc = {};
		resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resource_desc.Width = GetGpuSize<GpuSceneParameters>();
		resource_desc.Height = 1;
		resource_desc.DepthOrArraySize = 1;
		resource_desc.MipLevels = 1;
		resource_desc.Format = DXGI_FORMAT_UNKNOWN;
		resource_desc.SampleDesc = { 1, 0 };
		resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		auto hr = device->CreateCommittedResource(
			&heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_scene_parameters_gpu_buffer));

		if (hr != S_OK)
		{
			LogError << "Can't create scene parameters GPU buffer\n";
			return false;
		}

		return true;
	}

	void Yasno::UpdateGpuCameraBuffer()
	{
		void* data;
		m_camera_gpu_buffer->Map(0, nullptr, &data);

		auto* camera_data = static_cast<CameraParameters*>(data);

		DirectX::XMMATRIX view_projection = XMMatrixMultiply(m_render_scene.camera->GetViewMatrix(), m_render_scene.camera->GetProjectionMatrix());

		camera_data->view_projection = view_projection;
		camera_data->view = m_render_scene.camera->GetViewMatrix();
		camera_data->previous_view = m_render_scene.camera->GetPrevViewMatrix();
		camera_data->projection = m_render_scene.camera->GetProjectionMatrix();
		camera_data->view_inverse = XMMatrixInverse(nullptr, m_render_scene.camera->GetViewMatrix());
		camera_data->projection_inverse = XMMatrixInverse(nullptr, m_render_scene.camera->GetProjectionMatrix());
		camera_data->position = m_render_scene.camera->GetPosition();
		camera_data->frame_number = m_pathtracing_frame_number;
		camera_data->rtx_frames_accumulated = m_rtx_frames_accumulated;
		camera_data->reset_accumulation = m_reset_rtx_accumulation ? 1 : 0;
		camera_data->accumulation_enabled = m_is_rtx_accumulation_enabled ? 1 : 0;

		m_camera_gpu_buffer->Unmap(0, nullptr);
	}

	void Yasno::UpdateGpuSceneParametersBuffer()
	{
		void* data;
		m_scene_parameters_gpu_buffer->Map(0, nullptr, &data);

		auto* scene_parameters_data = static_cast<GpuSceneParameters*>(data);

		scene_parameters_data->shadow_matrix = m_shadow_pass.shadow_matrix;
		scene_parameters_data->directional_light_color = m_render_scene.directional_light.color;
		scene_parameters_data->directional_light_direction = m_render_scene.directional_light.direction;
		scene_parameters_data->directional_light_intensity = m_render_scene.directional_light.intensity;
		scene_parameters_data->ambient_light_intensity = m_render_scene.environment_light.intensity;
		scene_parameters_data->shadows_enabled = (uint32_t)m_render_scene.directional_light.cast_shadow;

		m_scene_parameters_gpu_buffer->Unmap(0, nullptr);
	}

	std::expected<bool, std::string> Yasno::LoadContent()
	{
		const auto init_start_time = std::chrono::high_resolution_clock::now();

		const auto device = Application::Get().GetDevice();
		auto renderer = Application::Get().GetRenderer();

		D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0; // TODO: WHY NOT D3D_ROOT_SIGNATURE_VERSION_1_2?????

		if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
		{
			featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
		}

		// auto command_queue = Application::Get().GetCopyQueue();
		auto command_queue = Application::Get().GetDirectQueue();

		const auto cmd_list_res = command_queue->GetCommandList("Load Content");

		if (!cmd_list_res)
			return false;

		auto command_list = cmd_list_res.value();

		bool capture_loading_pix = false;

		if (capture_loading_pix)
		{
			PIXCaptureParameters pix_capture_parameters;
			pix_capture_parameters.GpuCaptureParameters.FileName = L"Yasno.pix";
			PIXSetTargetWindow(m_window->GetWindowHandle());
			AssertMsg(PIXBeginCapture2(PIX_CAPTURE_GPU, &pix_capture_parameters) != S_OK, "PIX capture failed!");
		}

		if (!m_generate_mips_pass.Initialize(renderer))
			return "Can't initialize generate mips pass";

		// Setup camera
		if (!CreateGpuCameraBuffer())
		{
			LogError << "Yasno app can't create GPU camera buffer\n";
			return false;
		}

		m_render_scene.camera = std::make_shared<ysn::Camera>(DirectX::XMVectorSet(0, 0, 23, 1));
		m_render_scene.camera_controler.p_camera = m_render_scene.camera;

		bool load_result = false;

		// Simplify simple primitive creation
		int rows = 7;
		int columns = 7;
		int sphere_counter = 0;
		float spacing = 2.5;

		if (false)
		{
			Model model;
			model.name = "Sphere";

			for (int row = 0; row < rows; row++)
			{
				for (int column = 0; column < columns; column++)
				{
					ysn::Material material("Sphere Material");
					material.blend_desc.RenderTarget[0].BlendEnable = false;
					material.blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
					material.rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE;
					material.rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
					material.rasterizer_desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
					material.rasterizer_desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
					material.rasterizer_desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
					material.rasterizer_desc.ForcedSampleCount = 0;
					material.rasterizer_desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
					material.rasterizer_desc.FrontCounterClockwise = TRUE;

					SurfaceShaderParameters shader_params;
					shader_params.base_color_factor = DirectX::XMFLOAT4(1.0, 1.0, 1.0, 1.0);
					shader_params.metallic_factor = float(row) / float(rows);
					shader_params.roughness_factor = std::min(1.0f, std::max(0.05f, float(column) / float(columns)));
					shader_params.texture_enable_bitmask = 0;

					ysn::Primitive sphere_primitive = ConstructSphere(1.0, 32, 32);
					sphere_primitive.material_id = sphere_counter;

					Mesh mesh;
					mesh.name = "Sphere Mesh";
					mesh.primitives.push_back(sphere_primitive);

					model.meshes.push_back(mesh);
					model.materials.push_back(material);
					model.shader_parameters.push_back(shader_params);
					model.transforms.push_back(DirectX::XMMatrixTranslation((float)(column - (columns / 2.0f)) * spacing, (float)(row - (rows / 2.0f)) * spacing, 0.0f));


					m_render_scene.indices_count += sphere_primitive.index_count;
					m_render_scene.vertices_count += sphere_primitive.vertex_count;
					m_render_scene.primitives_count += 1;
					m_render_scene.materials_count += 1;

					sphere_counter += 1;

					load_result = true;
				}
			}
			m_render_scene.models.push_back(model);
		}

		{
			LoadingParameters loading_parameters;
			//load_result = LoadGltfFromFile(m_render_scene, VfsPath(L"assets/Sponza/Sponza.gltf"), loading_parameters);
		}

		{
			LoadingParameters loading_parameters;
			load_result = LoadGltfFromFile(m_render_scene, VfsPath(L"assets/DamagedHelmet/DamagedHelmet.gltf"), loading_parameters);
		}

		{
			LoadingParameters loading_parameters;
			//loading_parameters.model_modifier = XMMatrixScaling(0.01f, 0.01f, 0.01f);
			//load_result = LoadGltfFromFile(m_render_scene, VfsPath(L"assets/Bistro/Bistro.gltf"), loading_parameters);
		}

		//{
		//	LoadingParameters loading_parameters;
		//	load_result = LoadGltfFromFile(m_render_scene, VfsPath(L"assets/Sponza_New/NewSponza_Main_glTF_002.gltf"), loading_parameters);
		//}

		// TODO: Verify all of this tests
		{

			LoadingParameters loading_parameters;
			// load_result = LoadGltfFromFile(m_render_scene, VfsPath(L"assets/TextureCoordinateTest/glTF/TextureCoordinateTest.gltf"), loading_parameters); // Texture coordinate test
			//load_result = LoadGltfFromFile(m_render_scene, VfsPath(L"assets/EnvironmentTest/glTF/EnvironmentTest.gltf"), loading_parameters); // Env test

			//loading_parameters.model_modifier = DirectX::XMMatrixScaling(10.f, 10.f, 10.f);
			//load_result = LoadGltfFromFile(m_render_scene, VfsPath(L"assets/BoomBoxWithAxes/glTF/BoomBoxWithAxes.gltf"), loading_parameters); // Boombox with axes
		}

		if (!load_result)
		{
			return std::unexpected("Can't load scenes\n");
		}

		// Build mips
		for (Model& model : m_render_scene.models)
			for (GpuTexture& texture : model.textures)
				m_generate_mips_pass.GenerateMips(renderer, command_list, texture);

		// Build scene buffers
		{
			// Index buffer
			{
				const uint32_t indices_buffer_size = m_render_scene.indices_count * sizeof(uint32_t);

				GpuBufferCreateInfo create_info{
					.size = indices_buffer_size, .heap_type = D3D12_HEAP_TYPE_DEFAULT, .state = D3D12_RESOURCE_STATE_COPY_DEST };

				const auto indices_buffer_result = CreateGpuBuffer(create_info, "Indices Buffer");

				if (!indices_buffer_result)
				{
					LogError << "Can't create indices buffer\n";
					return false;
				}

				m_render_scene.indices_buffer = indices_buffer_result.value();

				std::vector<uint32_t> all_indices_buffer;
				all_indices_buffer.reserve(m_render_scene.indices_count);

				for (auto& model : m_render_scene.models)
				{
					for (auto& mesh : model.meshes)
					{
						for (auto& primitive : mesh.primitives)
						{
							primitive.index_buffer_view.BufferLocation =
								m_render_scene.indices_buffer.GPUVirtualAddress() + all_indices_buffer.size() * sizeof(uint32_t);
							primitive.index_buffer_view.SizeInBytes = static_cast<uint32_t>(primitive.indices.size()) * sizeof(uint32_t);
							primitive.index_buffer_view.Format = DXGI_FORMAT_R32_UINT;

							primitive.index_count = static_cast<uint32_t>(primitive.indices.size());

							// Append indices
							all_indices_buffer.insert(all_indices_buffer.end(), primitive.indices.begin(), primitive.indices.end());
						}
					}
				}

				UploadToGpuBuffer(command_list, m_render_scene.indices_buffer, all_indices_buffer.data(), {}, D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			}

			// Vertex Buffer
			{
				const uint32_t vertices_buffer_size = m_render_scene.vertices_count * sizeof(Vertex);

				GpuBufferCreateInfo create_info{
					.size = vertices_buffer_size, .heap_type = D3D12_HEAP_TYPE_DEFAULT, .state = D3D12_RESOURCE_STATE_COPY_DEST };

				const auto vertices_buffer_result = CreateGpuBuffer(create_info, "Vertices Buffer");

				if (!vertices_buffer_result)
				{
					LogError << "Can't create vertices buffer\n";
					return false;
				}

				m_render_scene.vertices_buffer = vertices_buffer_result.value();

				std::vector<Vertex> all_vertices_buffer;
				all_vertices_buffer.reserve(m_render_scene.vertices_count);

				for (auto& model : m_render_scene.models)
				{
					for (auto& mesh : model.meshes)
					{
						for (auto& primitive : mesh.primitives)
						{
							primitive.vertex_buffer_view.BufferLocation = m_render_scene.vertices_buffer.GPUVirtualAddress() + all_vertices_buffer.size() * sizeof(Vertex);
							primitive.vertex_buffer_view.SizeInBytes = UINT(primitive.vertices.size() * sizeof(Vertex));
							primitive.vertex_buffer_view.StrideInBytes = sizeof(Vertex);

							primitive.vertex_count = static_cast<uint32_t>(primitive.vertices.size());

							// Append vertices
							all_vertices_buffer.insert(all_vertices_buffer.end(), primitive.vertices.begin(), primitive.vertices.end());
						}
					}
				}

				UploadToGpuBuffer(command_list, m_render_scene.vertices_buffer, all_vertices_buffer.data(), {}, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			}

			// Material buffer
			{
				GpuBufferCreateInfo create_info{
					.size = m_render_scene.materials_count * sizeof(SurfaceShaderParameters),
					.heap_type = D3D12_HEAP_TYPE_DEFAULT,
					.state = D3D12_RESOURCE_STATE_COPY_DEST };

				const auto materials_buffer_result = CreateGpuBuffer(create_info, "Materials Buffer");

				if (!materials_buffer_result)
				{
					LogError << "Can't create materials buffer\n";
					return false;
				}

				m_render_scene.materials_buffer = materials_buffer_result.value();

				std::vector<SurfaceShaderParameters> all_materials_buffer;
				all_materials_buffer.reserve(m_render_scene.materials_count);

				for (auto& model : m_render_scene.models)
				{
					for (auto& material : model.shader_parameters)
					{
						all_materials_buffer.push_back(material);
					}
				}

				UploadToGpuBuffer(command_list, m_render_scene.materials_buffer, all_materials_buffer.data(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

				// Create SRV
				D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
				srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv_desc.Buffer.FirstElement = 0;
				srv_desc.Buffer.NumElements = static_cast<UINT>(all_materials_buffer.size());
				srv_desc.Buffer.StructureByteStride = sizeof(SurfaceShaderParameters);
				srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
				srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

				m_render_scene.materials_buffer_srv = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
				renderer->GetDevice()->CreateShaderResourceView(
					m_render_scene.materials_buffer.Resource(), &srv_desc, m_render_scene.materials_buffer_srv.cpu);
			}

			// Instance buffer
			{
				GpuBufferCreateInfo create_info{
					.size = m_render_scene.primitives_count * sizeof(PerInstanceData),
					.heap_type = D3D12_HEAP_TYPE_DEFAULT,
					.state = D3D12_RESOURCE_STATE_COPY_DEST };

				const auto per_instance_buffer_result = CreateGpuBuffer(create_info, "RenderInstance Buffer");

				if (!per_instance_buffer_result)
				{
					LogError << "Can't create RenderInstance buffer\n";
					return false;
				}

				m_render_scene.instance_buffer = per_instance_buffer_result.value();

				std::vector<PerInstanceData> per_instance_data_buffer;
				per_instance_data_buffer.reserve(m_render_scene.primitives_count);

				uint32_t total_indices = 0;
				uint32_t total_vertices = 0;

				for (Model& model : m_render_scene.models)
				{
					for (int mesh_id = 0; mesh_id < model.meshes.size(); mesh_id++)
					{
						Mesh& mesh = model.meshes[mesh_id];

						for (Primitive& primitive : mesh.primitives)
						{
							PerInstanceData instance_data;
							if (primitive.material_id == -1)
							{
								LogInfo << "Material ID is negative when filling instance data buffer - investigate";
								// TODO: Assign "broken" material
								instance_data.material_id = 0;
							}
							else
							{
								instance_data.material_id = primitive.material_id;
							}

							instance_data.model_matrix = model.transforms[mesh_id];
							instance_data.vertices_before = total_vertices;
							instance_data.indices_before = total_indices;

							// Need for indirect commands filling later
							primitive.global_vertex_offset = instance_data.vertices_before;
							primitive.global_index_offset = instance_data.indices_before;

							total_indices += primitive.index_count;
							total_vertices += primitive.vertex_count;

							per_instance_data_buffer.push_back(instance_data);
						}
					}
				}

				UploadToGpuBuffer(
					command_list, m_render_scene.instance_buffer, per_instance_data_buffer.data(), {}, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				// Create SRV
				D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
				srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				srv_desc.Buffer.FirstElement = 0;
				srv_desc.Buffer.NumElements = static_cast<UINT>(m_render_scene.primitives_count);
				srv_desc.Buffer.StructureByteStride = sizeof(PerInstanceData);
				srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
				srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

				m_render_scene.instance_buffer_srv = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
				renderer->GetDevice()->CreateShaderResourceView(
					m_render_scene.instance_buffer.Resource(), &srv_desc, m_render_scene.instance_buffer_srv.cpu);
			}
		}

		for (auto& model : m_render_scene.models)
		{
			for (auto& mesh : model.meshes)
			{
				for (auto& primitive : mesh.primitives)
				{
					m_forward_pass.CompilePrimitivePso(primitive, model.materials);
					m_shadow_pass.CompilePrimitivePso(primitive, model.materials);
				}
			}
		}

		m_rtx_context.CreateAccelerationStructures(command_list, m_render_scene);

		if (!m_tonemap_pass.Initialize())
		{
			LogError << "Can't initialize tonemap pass\n";
			return false;
		}

		if (!m_convert_to_cubemap_pass.Initialize())
		{
			LogError << "Can't initialize convert to cubemap pass\n";
			return false;
		}

		if (!m_cubemap_filter_pass.Initialize())
		{
			LogError << "Can't initialize cubemap filter pass\n";
			return false;
		}

		if (!m_skybox_pass.Initialize())
			return std::unexpected("Can't initialize skybox pass");

		InitializeImgui(m_window, renderer);

		const auto enviroment_hdr_descriptor_handle = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();

		{
			LoadTextureParameters parameter;
			parameter.filename = "assets/HDRI/newport_loft.hdr";
			parameter.command_list = command_list;
			parameter.generate_mips = false;
			parameter.is_srgb = false;
			const auto env_texture = LoadTextureFromFile(parameter);

			if (!env_texture)
			{
				return false;
			}

			m_environment_texture = *env_texture;
		}

		m_hdr_uav_descriptor_handle = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
		m_scene_color_buffer_1_handle = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
		m_backbuffer_uav_descriptor_handle = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
		m_depth_dsv_descriptor_handle = renderer->GetDsvDescriptorHeap()->GetNewHandle();
		m_depth_srv_descriptor_handle = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
		m_velocity_descriptor_handle = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();

		if (!CreateGpuSceneParametersBuffer())
		{
			LogError << "Yasno app can't create GPU scene parameters buffer\n";
			return false;
		}

		if (!m_forward_pass.Initialize(
			m_render_scene, m_camera_gpu_buffer, m_scene_parameters_gpu_buffer, m_render_scene.instance_buffer, command_list))
		{
			LogError << "Can't initialize forward pass\n";
			return false;
		}

		if (!m_debug_renderer.Initialize(command_list, m_camera_gpu_buffer))
		{
			LogError << "Can't initialize debug renderer\n";
			return false;
		}

		if (!m_volumetric_fog.Initialize())
		{
			LogError << "Can't initialize volumetric renderer\n";
			return false;
		}
		

		command_queue->CloseCommandList(command_list);

		const auto fence_res = command_queue->ExecuteCommandLists();

		if (!fence_res)
			return false;

		command_queue->WaitForFenceValue(fence_res.value());

		if (capture_loading_pix)
		{
			AssertMsg(PIXEndCapture(false) != S_OK, "PIXEndCapture failed");
		}

		{
			D3D12_INDEX_BUFFER_VIEW index_buffer_view;
			index_buffer_view.BufferLocation = m_render_scene.indices_buffer.GPUVirtualAddress();
			index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
			index_buffer_view.SizeInBytes = m_render_scene.indices_count * sizeof(uint32_t);

			m_render_scene.index_buffer_view = index_buffer_view;

			D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view;
			vertex_buffer_view.BufferLocation = m_render_scene.vertices_buffer.GPUVirtualAddress();
			vertex_buffer_view.StrideInBytes = sizeof(Vertex);
			vertex_buffer_view.SizeInBytes = m_render_scene.vertices_count * sizeof(Vertex);

			m_render_scene.vertex_buffer_view = vertex_buffer_view;
		}

		m_is_content_loaded = true;

		// Resize/Create the depth buffer.
		ResizeDepthBuffer(GetClientWidth(), GetClientHeight());
		ResizeHdrBuffer(GetClientWidth(), GetClientHeight());
		ResizeBackBuffer(GetClientWidth(), GetClientHeight());

		// Setup techniques
		m_shadow_pass.Initialize(Application::Get().GetRenderer());

		game_input.Initialize(m_window->GetWindowHandle());

		// Post init
		const auto cubemap_texture_result = CreateCubemapTexture("Cubemap Source", 512);
		const auto irradiance_result = CreateCubemapTexture("Irradiance Cubemap", 32);
		const auto radiance_result = CreateCubemapTexture("Radiance cubemap", 128, true);

		if (!cubemap_texture_result)
		{
			LogError << "Can't create cubemap\n";
			return false;
		}

		if (!irradiance_result)
		{
			LogError << "Can't create irradiance cubemap\n";
			return false;
		}

		if (!radiance_result)
		{
			LogError << "Can't create radiance cubemap\n";
			return false;
		}

		m_cubemap_texture = cubemap_texture_result.value();
		m_irradiance_cubemap_texture = irradiance_result.value();
		m_radiance_cubemap_texture = radiance_result.value();

		if (!m_ray_tracing_pass.Initialize(
			Application::Get().GetRenderer(),
			m_scene_color_buffer,
			m_scene_color_buffer_1,
			m_rtx_context,
			m_camera_gpu_buffer,
			m_render_scene.vertices_buffer.Resource(),
			m_render_scene.indices_buffer.Resource(),
			m_render_scene.materials_buffer.Resource(),
			m_render_scene.instance_buffer.Resource(),
			m_render_scene.vertices_count,
			m_render_scene.indices_count,
			m_render_scene.materials_count,
			m_render_scene.primitives_count))
		{
			LogError << "Can't initialize raytracing pass\n";
			return false;
		}

		const auto init_end_time = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(init_end_time - init_start_time);

		LogInfo << "LoadContent finished, duration: " << std::to_string(duration.count()) << " ms\n";

		return true;
	}

	bool Yasno::ResizeDepthBuffer(int width, int height)
	{
		if (m_is_content_loaded)
		{
			// Flush any GPU commands that might be referencing the depth buffer.
			Application::Get().Flush();

			width = std::max(1, width);
			height = std::max(1, height);

			const auto device = Application::Get().GetDevice();

			// Resize screen dependent resources.
			// Create a depth buffer.
			D3D12_CLEAR_VALUE optimized_clear_valuie = {};
			optimized_clear_valuie.Format = DXGI_FORMAT_D32_FLOAT;
			optimized_clear_valuie.DepthStencil = { 1.0f, 0 };

			const CD3DX12_HEAP_PROPERTIES heap_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

			const CD3DX12_RESOURCE_DESC resource_desc =
				CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, width, height, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

			if (auto result = device->CreateCommittedResource(
				&heap_properties,
				D3D12_HEAP_FLAG_NONE,
				&resource_desc,
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				&optimized_clear_valuie,
				IID_PPV_ARGS(&m_depth_buffer));
				result != S_OK)
			{
				LogFatal << "Can't create depth buffer" << ConvertHrToString(result) << " \n";
				return false;
			}

			// Update the depth-stencil view.
			D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
			dsv.Format = DXGI_FORMAT_D32_FLOAT;
			dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			dsv.Texture2D.MipSlice = 0;
			dsv.Flags = D3D12_DSV_FLAG_NONE;

			device->CreateDepthStencilView(m_depth_buffer.get(), &dsv, m_depth_dsv_descriptor_handle.cpu);

			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.Format = DXGI_FORMAT_R32_FLOAT;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			srv.Texture2D.MostDetailedMip = 0;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

			device->CreateShaderResourceView(m_depth_buffer.get(), &srv, m_depth_srv_descriptor_handle.cpu);
		}

		return true;
	}

	bool Yasno::ResizeHdrBuffer(int width, int height)
	{
		if (m_is_content_loaded)
		{
			// Flush any GPU commands that might be referencing the depth buffer.
			Application::Get().Flush();

			width = std::max(1, width);
			height = std::max(1, height);

			auto device = Application::Get().GetDevice();
			auto renderer = Application::Get().GetRenderer();

			D3D12_CLEAR_VALUE optimized_clear_valuie = {};
			optimized_clear_valuie.Format = Application::Get().GetRenderer()->GetHdrFormat();
			optimized_clear_valuie.Color[0] = 0.0f;
			optimized_clear_valuie.Color[1] = 0.0f;
			optimized_clear_valuie.Color[2] = 0.0f;
			optimized_clear_valuie.Color[3] = 0.0f;

			const auto heap_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

			const auto resource_desc = CD3DX12_RESOURCE_DESC::Tex2D(
				Application::Get().GetRenderer()->GetHdrFormat(), width, height, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

			if (auto result = device->CreateCommittedResource(
				&heap_properties,
				D3D12_HEAP_FLAG_NONE,
				&resource_desc,
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				&optimized_clear_valuie,
				IID_PPV_ARGS(&m_scene_color_buffer));
				result != S_OK)
			{
				LogFatal << "Can't create scene color buffer\n";
				return false;
			}

			if (auto result = device->CreateCommittedResource(
				&heap_properties,
				D3D12_HEAP_FLAG_NONE,
				&resource_desc,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				&optimized_clear_valuie,
				IID_PPV_ARGS(&m_scene_color_buffer_1));
				result != S_OK)
			{
				LogFatal << "Can't create scene color buffer 1\n";
				return false;
			}

		#ifndef YSN_RELEASE
			m_scene_color_buffer->SetName(L"Scene Color 0");
			m_scene_color_buffer_1->SetName(L"Scene Color 1");
		#endif

			D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
			rtv.Format = Application::Get().GetRenderer()->GetHdrFormat();
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			rtv.Texture2D.MipSlice = 0;

			m_hdr_rtv_descriptor_handle = renderer->GetRtvDescriptorHeap()->GetNewHandle();

			device->CreateRenderTargetView(m_scene_color_buffer.get(), &rtv, m_hdr_rtv_descriptor_handle.cpu);

			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = Application::Get().GetRenderer()->GetHdrFormat();
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

				device->CreateUnorderedAccessView(m_scene_color_buffer.get(), nullptr, &uavDesc, m_hdr_uav_descriptor_handle.cpu);
			}

			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
				uav_desc.Format = Application::Get().GetRenderer()->GetHdrFormat();
				uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

				device->CreateUnorderedAccessView(m_scene_color_buffer_1.get(), nullptr, &uav_desc, m_scene_color_buffer_1_handle.cpu);
			}
		}

		return true;
	}

	bool Yasno::ResizeBackBuffer(int width, int height)
	{
		if (m_is_content_loaded)
		{
			// Flush any GPU commands that might be referencing the depth buffer.
			Application::Get().Flush();

			width = std::max(1, width);
			height = std::max(1, height);

			auto device = Application::Get().GetDevice();

			D3D12_CLEAR_VALUE optimized_clear_valuie = {};
			optimized_clear_valuie.Format = Application::Get().GetRenderer()->GetBackBufferFormat();
			optimized_clear_valuie.Color[0] = 0.0f;
			optimized_clear_valuie.Color[1] = 0.0f;
			optimized_clear_valuie.Color[2] = 0.0f;
			optimized_clear_valuie.Color[3] = 0.0f;

			const auto heap_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

			const auto resource_desc = CD3DX12_RESOURCE_DESC::Tex2D(
				Application::Get().GetRenderer()->GetBackBufferFormat(),
				width,
				height,
				1,
				0,
				1,
				0,
				D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

			if (auto result = device->CreateCommittedResource(
				&heap_properties,
				D3D12_HEAP_FLAG_NONE,
				&resource_desc,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				&optimized_clear_valuie,
				IID_PPV_ARGS(&m_back_buffer));
				result != S_OK)
			{
				LogFatal << "Can't create back buffer" << ConvertHrToString(result) << " \n";
				return false;
			}

		#ifndef YSN_RELEASED
			m_back_buffer->SetName(L"Back Buffer");
		#endif

			{
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = Application::Get().GetRenderer()->GetBackBufferFormat();
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

				device->CreateUnorderedAccessView(m_back_buffer.get(), nullptr, &uavDesc, m_backbuffer_uav_descriptor_handle.cpu);
			}
		}

		return true;
	}

	void Yasno::OnResize(ResizeEventArgs& e)
	{
		if (e.Width != GetClientWidth() || e.Height != GetClientHeight())
		{
			Game::OnResize(e);

			m_viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(e.Width), static_cast<float>(e.Height));

			ResizeDepthBuffer(e.Width, e.Height);
			ResizeHdrBuffer(e.Width, e.Height);
			ResizeBackBuffer(e.Width, e.Height);
		}
	}

	void Yasno::OnUpdate(UpdateEventArgs& e)
	{
		static double totalTime = 0.0;

		Game::OnUpdate(e);

		totalTime += e.ElapsedTime;
		m_frame_number += 1;

		if (totalTime > 1.0)
		{
			engine_stats::fps = uint32_t(m_frame_number / totalTime);
			engine_stats::frame_ms = e.ElapsedTime;

			m_frame_number = 0;
			totalTime = 0.0;
		}

		auto kb = game_input.keyboard->GetState();

		auto mouse = game_input.mouse->GetState();
		game_input.mouse->SetMode(mouse.rightButton ? DirectX::Mouse::MODE_RELATIVE : DirectX::Mouse::MODE_ABSOLUTE);

		if (!ImGui::GetIO().WantTextInput)
		{
			if (mouse.positionMode == DirectX::Mouse::MODE_RELATIVE)
			{
				m_render_scene.camera_controler.MoveMouse(mouse.x, mouse.y);

				m_render_scene.camera_controler.m_is_boost_active = kb.IsKeyDown(DirectX::Keyboard::LeftShift);

				if (kb.IsKeyDown(DirectX::Keyboard::W))
				{
					m_render_scene.camera_controler.MoveForward(static_cast<float>(e.ElapsedTime));
				}
				if (kb.IsKeyDown(DirectX::Keyboard::A))
				{
					m_render_scene.camera_controler.MoveLeft(static_cast<float>(e.ElapsedTime));
				}
				if (kb.IsKeyDown(DirectX::Keyboard::S))
				{
					m_render_scene.camera_controler.MoveBackwards(static_cast<float>(e.ElapsedTime));
				}
				if (kb.IsKeyDown(DirectX::Keyboard::D))
				{
					m_render_scene.camera_controler.MoveRight(static_cast<float>(e.ElapsedTime));
				}
				if (kb.IsKeyDown(DirectX::Keyboard::Space))
				{
					m_render_scene.camera_controler.MoveUp(static_cast<float>(e.ElapsedTime));
				}
				if (kb.IsKeyDown(DirectX::Keyboard::LeftControl))
				{
					m_render_scene.camera_controler.MoveDown(static_cast<float>(e.ElapsedTime));
				}
			}

			if (kb.IsKeyDown(DirectX::Keyboard::Escape))
			{
				Application::Get().Quit(0);
			}

			{
				if (kb.IsKeyDown(DirectX::Keyboard::R) && !m_is_raster_pressed)
				{
					m_is_raster = !m_is_raster;
					m_is_raster_pressed = true;
				}

				if (kb.IsKeyUp(DirectX::Keyboard::R))
				{
					m_is_raster_pressed = false;
				}
			}
		}

		if (m_render_scene.camera_controler.IsMoved())
		{
			m_reset_rtx_accumulation = true;
		}

		m_render_scene.camera->SetAspectRatio(GetClientWidth() / static_cast<float>(GetClientHeight()));
		m_render_scene.camera->Update();

		// Clear stage GPU resources
		Application::Get().GetRenderer()->stage_heap.clear();
		Application::Get().GetRenderer()->Tick();
	}

	void Yasno::RenderUi()
	{
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("Exit", "Esc"))
				{
					Application::Get().Quit(0);
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}

		{
			ImGui::Begin(CONTROLS_NAME.c_str());

			static std::string text;
			static char buffer[256] = "";
			if (ImGui::InputText("cvar", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue))
			{
				text = buffer;
				buffer[0] = '\0';

				LogInfo << "New cvar " << text << "\n";
			}
			ImGui::Text("You entered: %s", text.c_str());

			ImGui::Checkbox("Indirect", &m_is_indirect);

			if (ImGui::CollapsingHeader("Lighting"), ImGuiTreeNodeFlags_DefaultOpen)
			{
				static float color[3] = {
					m_render_scene.directional_light.color.x,
					m_render_scene.directional_light.color.y,
					m_render_scene.directional_light.color.z };
				ImGui::ColorEdit3("Color", color, ImGuiColorEditFlags_Float);
				m_render_scene.directional_light.color.x = color[0];
				m_render_scene.directional_light.color.y = color[1];
				m_render_scene.directional_light.color.z = color[2];

				static float dir[3] = {
					m_render_scene.directional_light.direction.x,
					m_render_scene.directional_light.direction.y,
					m_render_scene.directional_light.direction.z };
				ImGui::InputFloat3("Direction", dir);
				m_render_scene.directional_light.direction.x = dir[0];
				m_render_scene.directional_light.direction.y = dir[1];
				m_render_scene.directional_light.direction.z = dir[2];

				m_shadow_pass.UpdateLight(m_render_scene.directional_light);

				ImGui::InputFloat("Intensity", &m_render_scene.directional_light.intensity, 0.0f, 1000.0f);
				ImGui::InputFloat("Env Light Intensity", &m_render_scene.environment_light.intensity, 0.0f, 1000.0f);

				{
					const char* items[] = {
						"Cubemap",
						"Irradiance",
						"Radiance",
					};
					static int item_current = (int)m_active_skybox_cubemap;
					ImGui::Combo("Skybox", &item_current, items, IM_ARRAYSIZE(items));
					m_active_skybox_cubemap = static_cast<SkyboxCubemap>(item_current);
				}
			}

			if (ImGui::CollapsingHeader("Volumetric Fog"), ImGuiTreeNodeFlags_DefaultOpen)
			{
				ImGui::Checkbox("Fog Enabled", &m_volumetric_fog.is_pass_active);
				ImGui::InputInt("Fog Samples", &m_volumetric_fog.num_samples, 4, 256);
			}

			if (ImGui::CollapsingHeader("Shadows"), ImGuiTreeNodeFlags_DefaultOpen)
			{
				ImGui::Checkbox("Shadows Enabled", &m_render_scene.directional_light.cast_shadow);
			}

			if (ImGui::CollapsingHeader("Tonemapping"), ImGuiTreeNodeFlags_DefaultOpen)
			{
				const char* items[] = {
					"None",
					"Reinhard",
					"ACES",
				};
				static int item_current = (int)m_tonemap_pass.tonemap_method;
				ImGui::Combo("Tonemapper", &item_current, items, IM_ARRAYSIZE(items));
				m_tonemap_pass.tonemap_method = static_cast<TonemapMethod>(item_current);

				ImGui::InputFloat("Exposure", &m_tonemap_pass.exposure, 0.0, 1000.0);
			}



			if (ImGui::CollapsingHeader("Camera"), ImGuiTreeNodeFlags_DefaultOpen)
			{
				ImGui::InputFloat("Speed", &m_render_scene.camera_controler.mouse_speed, 0.0f, 1000.0f);
				ImGui::InputFloat("FOV", &m_render_scene.camera->fov, 0.0f, 1000.0f);
			}

			if (ImGui::CollapsingHeader("System"), ImGuiTreeNodeFlags_DefaultOpen)
			{
				static bool vsync = m_window->IsVSync();
				if (ImGui::Checkbox("Vsync", &vsync))
				{
					m_window->SetVSync(vsync);
				}
			}

			ImGui::End();
		}

		{
			ImGui::Begin(STATS_NAME.c_str());

			ImGui::Text(std::format("frame ms: {:.3f}", engine_stats::frame_ms * 1000.f).c_str());
			ImGui::Text(std::format("fps: {}", engine_stats::fps).c_str());

			if (ImGui::CollapsingHeader("Mode"), ImGuiTreeNodeFlags_DefaultOpen)
			{
				if (m_is_raster)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
					ImGui::Text("Rasterization");
					ImGui::PopStyleColor();
				}
				else
				{
					ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 215, 0, 255));
					ImGui::Text("Pathtracing");
					ImGui::Checkbox("Temporal Accumulation", &m_is_rtx_accumulation_enabled);

					ImGui::Text(std::format("Frames accumulated {}", m_rtx_frames_accumulated).c_str());

					ImGui::PopStyleColor();
				}
			}

			ImGui::End();
		}
	}

	void Yasno::OnRender(RenderEventArgs& e)
	{
		ScopedZone s("Frame Render");

		Game::OnRender(e);

		if (GetClientWidth() < 8 || GetClientHeight() < 8)
		{
			// Skip render to very small output
			return;
		}

		// Render GUI
		{
			ImguiPrepareNewFrame();
			ImGuizmo::BeginFrame();

			RenderUi();

			ImGuizmo::SetRect(0, 0, static_cast<float>(GetClientWidth()), static_cast<float>(GetClientHeight()));

			DirectX::XMFLOAT4X4 view;
			DirectX::XMFLOAT4X4 projection;
			DirectX::XMFLOAT4X4 identity;

			XMStoreFloat4x4(&view, m_render_scene.camera->GetViewMatrix());
			XMStoreFloat4x4(&projection, m_render_scene.camera->GetProjectionMatrix());
			XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());

			ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &identity.m[0][0]);
		}

		std::shared_ptr<ysn::CommandQueue> command_queue = Application::Get().GetDirectQueue();
		auto renderer = Application::Get().GetRenderer();

		UINT current_backbuffer_index = m_window->GetCurrentBackBufferIndex();
		wil::com_ptr<ID3D12Resource> current_back_buffer = m_window->GetCurrentBackBuffer();
		D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_handle = m_window->GetCurrentRenderTargetView();

		if (m_is_raster)
		{
			ShadowRenderParameters parameters;
			parameters.command_queue = command_queue;
			parameters.scene_parameters_gpu_buffer = m_scene_parameters_gpu_buffer;

			m_shadow_pass.Render(m_render_scene, parameters);
		}

		{
			const auto command_list_result = command_queue->GetCommandList("Update GPU Data");

			if (!command_list_result.has_value())
				return;

			auto command_list = command_list_result.value();

			UpdateGpuCameraBuffer();
			UpdateGpuSceneParametersBuffer();

			m_volumetric_fog.PrepareData(command_list, GetClientWidth(), GetClientHeight());

			command_queue->CloseCommandList(command_list);
		}
		
		// Do it once
		if(m_is_first_frame)
		{
			{
				ConvertToCubemapParameters parameters;
				parameters.camera_buffer = m_camera_gpu_buffer;
				parameters.source_texture = m_environment_texture;
				parameters.target_cubemap = m_cubemap_texture;

				m_convert_to_cubemap_pass.Render(parameters);
			}

			{
				ConvolveCubemapParameters parameters;
				parameters.camera_buffer = m_camera_gpu_buffer;
				parameters.source_cubemap = m_cubemap_texture;
				parameters.target_irradiance = m_irradiance_cubemap_texture;
				parameters.target_radiance = m_radiance_cubemap_texture;

				m_cubemap_filter_pass.ConvolveRadiance(parameters);
				m_cubemap_filter_pass.ConvolveIrradiance(parameters);
				m_cubemap_filter_pass.ComputeBRDF();
			}
		}

		if (m_is_raster)
		{
			{
				const auto cmd_list_res = command_queue->GetCommandList("Clear before forward");
				if (!cmd_list_res)
					return;

				auto command_list = cmd_list_res.value();

				FLOAT clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };

				CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					current_back_buffer.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
				command_list->ResourceBarrier(1, &barrier);

				command_list->ClearRenderTargetView(backbuffer_handle, clear_color, 0, nullptr);
				command_list->ClearRenderTargetView(m_hdr_rtv_descriptor_handle.cpu, clear_color, 0, nullptr);
				command_list->ClearDepthStencilView(m_depth_dsv_descriptor_handle.cpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

				command_queue->CloseCommandList(command_list);
			}

			{
				ForwardPassRenderParameters render_parameters;
				render_parameters.command_queue = command_queue;
				render_parameters.cbv_srv_uav_heap = renderer->GetCbvSrvUavDescriptorHeap();
				render_parameters.sampler_heap = renderer->GetSamplerDescriptorHeap();
				render_parameters.scene_color_buffer = m_scene_color_buffer;
				render_parameters.hdr_rtv_descriptor_handle = m_hdr_rtv_descriptor_handle;
				render_parameters.dsv_descriptor_handle = m_depth_dsv_descriptor_handle;
				render_parameters.viewport = m_viewport;
				render_parameters.scissors_rect = m_scissors_rect;
				render_parameters.camera_gpu_buffer = m_camera_gpu_buffer;
				render_parameters.backbuffer_handle = backbuffer_handle;
				render_parameters.current_back_buffer = current_back_buffer;
				render_parameters.shadow_map_buffer = m_shadow_pass.shadow_map_buffer;
				render_parameters.scene_parameters_gpu_buffer = m_scene_parameters_gpu_buffer;
				render_parameters.cubemap_texture = m_cubemap_texture;
				render_parameters.irradiance_texture = m_irradiance_cubemap_texture;
				render_parameters.radiance_texture = m_radiance_cubemap_texture;
				render_parameters.brdf_texture_handle = m_cubemap_filter_pass.GetBrdfTextureHandle();

				render_parameters.debug_vertices_buffer_uav = m_debug_renderer.vertices_buffer_uav;
				render_parameters.debug_counter_buffer_uav = m_debug_renderer.counter_uav;

				if (m_is_indirect)
				{
					m_forward_pass.RenderIndirect(m_render_scene, render_parameters);
				}
				else
				{
					m_forward_pass.Render(m_render_scene, render_parameters);
				}
			}
		}
		else
		{
			if (m_is_rtx_accumulation_enabled && !m_reset_rtx_accumulation)
			{
				m_rtx_frames_accumulated += 1;
			}
			else
			{
				m_rtx_frames_accumulated = 0;
				m_reset_rtx_accumulation = true;
			}

			const auto cmd_list_res = command_queue->GetCommandList("RTX Pass");
			if (!cmd_list_res)
				return;
			auto command_list = cmd_list_res.value();

			// Clear the render targets.
			{
				FLOAT clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };

				CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					current_back_buffer.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
				command_list->ResourceBarrier(1, &barrier);

				command_list->ClearRenderTargetView(backbuffer_handle, clear_color, 0, nullptr);
				command_list->ClearRenderTargetView(m_hdr_rtv_descriptor_handle.cpu, clear_color, 0, nullptr);
				command_list->ClearDepthStencilView(m_depth_dsv_descriptor_handle.cpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			}

			m_ray_tracing_pass.Execute(
				Application::Get().GetRenderer(),
				m_rtx_context,
				m_hdr_uav_descriptor_handle,
				m_scene_color_buffer_1_handle,
				command_list,
				GetClientWidth(),
				GetClientHeight(),
				m_scene_color_buffer,
				m_camera_gpu_buffer);

			command_queue->CloseCommandList(command_list);
		}

		if (m_is_raster)
		{
			SkyboxPassParameters skybox_parameters;
			skybox_parameters.scene_color_buffer = m_scene_color_buffer;
			skybox_parameters.hdr_rtv_descriptor_handle = m_hdr_rtv_descriptor_handle;
			skybox_parameters.dsv_descriptor_handle = m_depth_dsv_descriptor_handle;
			skybox_parameters.viewport = m_viewport;
			skybox_parameters.scissors_rect = m_scissors_rect;
			switch (m_active_skybox_cubemap)
			{
				case SkyboxCubemap::Cubemap:
					skybox_parameters.cubemap_texture = m_cubemap_texture;
					break;
				case SkyboxCubemap::Irradiance:
					skybox_parameters.cubemap_texture = m_irradiance_cubemap_texture;
					break;
				case SkyboxCubemap::Radiance:
					skybox_parameters.cubemap_texture = m_radiance_cubemap_texture;
					break;
			}
			skybox_parameters.camera_gpu_buffer = m_camera_gpu_buffer;
			skybox_parameters.debug_vertices_buffer_uav = m_debug_renderer.vertices_buffer_uav;
			skybox_parameters.debug_counter_buffer_uav = m_debug_renderer.counter_uav;

			m_skybox_pass.RenderSkybox(&skybox_parameters);
		}

		if (m_is_raster)
		{
			VolumetricFogPassInput pass_input;
			pass_input.camera_gpu_buffer = m_camera_gpu_buffer;
			pass_input.scene_color_buffer = m_scene_color_buffer;
			pass_input.scene_parameters_gpu_buffer = m_scene_parameters_gpu_buffer; 
			pass_input.hdr_uav_descriptor_handle = m_hdr_uav_descriptor_handle;
			pass_input.depth_srv = m_depth_srv_descriptor_handle;
			pass_input.shadow_map_buffer = m_shadow_pass.shadow_map_buffer;
			pass_input.backbuffer_width = GetClientWidth();
			pass_input.backbuffer_height = GetClientHeight();

			m_volumetric_fog.Render(pass_input);
		}

		{
			TonemapPostprocessParameters tonemap_parameters;
			tonemap_parameters.command_queue = command_queue;
			tonemap_parameters.cbv_srv_uav_heap = renderer->GetCbvSrvUavDescriptorHeap();
			tonemap_parameters.scene_color_buffer = m_scene_color_buffer;
			tonemap_parameters.hdr_uav_descriptor_handle = m_hdr_uav_descriptor_handle;
			tonemap_parameters.backbuffer_uav_descriptor_handle = m_backbuffer_uav_descriptor_handle;
			tonemap_parameters.backbuffer_width = GetClientWidth();
			tonemap_parameters.backbuffer_height = GetClientHeight();

			m_tonemap_pass.Render(&tonemap_parameters);
		}

		{
			const auto cmd_list_res = command_queue->GetCommandList("Copy Backbuffer");
			if (!cmd_list_res)
				return;

			const auto cmd_list = cmd_list_res.value();

			D3D12_BOX sourceRegion;
			sourceRegion.left = 0;
			sourceRegion.top = 0;
			sourceRegion.right = GetClientWidth();
			sourceRegion.bottom = GetClientHeight();
			sourceRegion.front = 0;
			sourceRegion.back = 1;

			CD3DX12_TEXTURE_COPY_LOCATION Dst(current_back_buffer.get());
			CD3DX12_TEXTURE_COPY_LOCATION Src(m_back_buffer.get());

			CD3DX12_RESOURCE_BARRIER barrier0 = CD3DX12_RESOURCE_BARRIER::Transition(
				m_back_buffer.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			cmd_list->ResourceBarrier(1, &barrier0);

			CD3DX12_RESOURCE_BARRIER barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(
				current_back_buffer.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
			cmd_list->ResourceBarrier(1, &barrier1);

			cmd_list->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &sourceRegion);

			CD3DX12_RESOURCE_BARRIER barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(
				current_back_buffer.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
			CD3DX12_RESOURCE_BARRIER barrier3 = CD3DX12_RESOURCE_BARRIER::Transition(
				m_back_buffer.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			cmd_list->ResourceBarrier(1, &barrier2);
			cmd_list->ResourceBarrier(1, &barrier3);

			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_scene_color_buffer.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
			cmd_list->ResourceBarrier(1, &barrier);

			command_queue->CloseCommandList(cmd_list);
		}

		// Debug geometry
		{
			const auto cmd_list_res = command_queue->GetCommandList("Debug Geometry");

			if (!cmd_list_res)
				return;

			const auto cmd_list = cmd_list_res.value();

			cmd_list->OMSetRenderTargets(1, &backbuffer_handle, FALSE, &m_depth_dsv_descriptor_handle.cpu);

			DebugRendererParameters parameters;
			parameters.viewport = m_viewport;
			parameters.scissors_rect = m_scissors_rect;
			parameters.cmd_list = cmd_list;
			parameters.camera_gpu_buffer = m_camera_gpu_buffer;

			m_debug_renderer.RenderDebugGeometry(parameters);

			command_queue->CloseCommandList(cmd_list);
		}

		{
			const auto cmd_list_res = command_queue->GetCommandList("Imgui");
			if (!cmd_list_res)
				return;

			const auto cmd_list = cmd_list_res.value();

			ID3D12DescriptorHeap* ppHeaps[] = { Application::Get().GetRenderer()->GetCbvSrvUavDescriptorHeap()->GetHeapPtr() };
			cmd_list->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
			cmd_list->OMSetRenderTargets(1, &backbuffer_handle, FALSE, &m_depth_dsv_descriptor_handle.cpu);

			ImguiRenderFrame(cmd_list);

			command_queue->CloseCommandList(cmd_list);
		}

		// Present
		{
			const auto cmd_list_res = command_queue->GetCommandList("Present");
			if (!cmd_list_res)
				return;

			auto command_list = cmd_list_res.value();

			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(current_back_buffer.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

			command_list->ResourceBarrier(1, &barrier);

			command_queue->CloseCommandList(command_list);

			const auto execute_res = command_queue->ExecuteCommandLists();

			if (!execute_res)
				return;

			m_fence_values[current_backbuffer_index] = execute_res.value();

			const auto present_res = m_window->Present();

			if (!present_res)
				return;

			current_backbuffer_index = present_res.value();

			command_queue->WaitForFenceValue(m_fence_values[current_backbuffer_index]);
		}

		m_is_first_frame = false;
		m_reset_rtx_accumulation = false;
		m_pathtracing_frame_number += 1;
	}
}
