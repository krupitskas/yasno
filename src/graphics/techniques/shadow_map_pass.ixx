module;

#include <SimpleMath.h>
#include <DirectXMath.h>
#include <d3dx12.h>
#include <wil/com.h>

#include <shader_structs.h>

export module graphics.techniques.shadow_map_pass;

import std;
import renderer.dx_renderer;
import renderer.descriptor_heap;
import renderer.command_queue;
import graphics.primitive;
import graphics.material;
import graphics.lights;
import graphics.render_scene;
import system.math;
import system.filesystem;
import system.application;
import system.logger;
import system.asserts;

export namespace ysn
{
	using namespace DirectX;

	struct ShadowMapBuffer
	{
		wil::com_ptr<ID3D12Resource> buffer;
		DescriptorHandle dsv_handle;
		DescriptorHandle srv_handle;
	};

	struct ShadowRenderParameters
	{
		std::shared_ptr<ysn::CommandQueue> command_queue;
		wil::com_ptr<ID3D12Resource> scene_parameters_gpu_buffer;
	};

	class ShadowMapPass
	{
	public:
		void Initialize(std::shared_ptr<DxRenderer> p_renderer);
		bool CompilePrimitivePso(Primitive& primitive, std::vector<Material> materials);
		void UpdateLight(const DirectionalLight& Light);
		bool Render(const RenderScene& render_scene, const ShadowRenderParameters& parameters);

		// Shadow Map resources
		std::uint32_t ShadowMapDimension = 4096;
		float shadow_follow_distance = 55.0f;
		float shadow_eye_distance = 220.0f;
		float shadow_extent = 110.0f;
		float shadow_near_plane = 0.1f;
		float shadow_far_plane = 700.0f;

		XMMATRIX shadow_matrix;

		D3D12_VIEWPORT Viewport;
		D3D12_RECT ScissorRect;

		ShadowMapBuffer shadow_map_buffer;

	private:
		bool InitializeShadowMapBuffer(std::shared_ptr<ysn::DxRenderer> p_renderer);
		void InitializeOrthProjection(SimpleMath::Vector3 direction);
		void UpdateShadowMatrixForCamera(const RenderScene& render_scene);
		bool InitializeCamera(std::shared_ptr<ysn::DxRenderer> p_renderer);

		wil::com_ptr<ID3D12Resource> m_camera_buffer;
		SimpleMath::Vector3 m_light_direction = { 0.1f, -1.0f, 0.45f };
	};
}

module :private;

namespace ysn
{
	namespace
	{
		DirectX::SimpleMath::Vector3 NormalizeOrDefault(
			const DirectX::SimpleMath::Vector3& value,
			const DirectX::SimpleMath::Vector3& fallback)
		{
			if (value.LengthSquared() <= 0.00001f)
			{
				return fallback;
			}

			auto normalized = value;
			normalized.Normalize();

			return normalized;
		}
	}

	bool ShadowMapPass::InitializeCamera(std::shared_ptr<DxRenderer> p_renderer)
	{
		D3D12_HEAP_PROPERTIES heapProperties = {};
		heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC resourceDesc = {};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Width = AlignPow2(sizeof(ShadowCamera), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
		resourceDesc.Height = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		resourceDesc.SampleDesc = { 1, 0 };
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		auto result = p_renderer->GetDevice()->CreateCommittedResource(
			&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_camera_buffer));

		if (result != S_OK)
		{
			LogError << "Can't create camera GPU buffer\n";
			return false;
		}

		return true;
	}

	void ShadowMapPass::Initialize(std::shared_ptr<DxRenderer> p_renderer)
	{
		InitializeShadowMapBuffer(p_renderer);
		InitializeCamera(p_renderer);
		InitializeOrthProjection(m_light_direction);
	}

	bool ShadowMapPass::CompilePrimitivePso(ysn::Primitive& primitive, std::vector<Material> materials)
	{
		std::shared_ptr<DxRenderer> renderer = Application::Get().GetRenderer();

		GraphicsPsoDesc pso_desc("Shadow PSO");

		{
			bool result = false;

			D3D12_DESCRIPTOR_RANGE instance_data_input_range = {};
			instance_data_input_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			instance_data_input_range.NumDescriptors = 1;
			instance_data_input_range.BaseShaderRegister = 0;

			D3D12_ROOT_PARAMETER instance_buffer_parameter;
			instance_buffer_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			instance_buffer_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			instance_buffer_parameter.DescriptorTable.NumDescriptorRanges = 1;
			instance_buffer_parameter.DescriptorTable.pDescriptorRanges = &instance_data_input_range;

			D3D12_ROOT_PARAMETER root_params[4] = {
				{D3D12_ROOT_PARAMETER_TYPE_CBV, {0, 0}, D3D12_SHADER_VISIBILITY_VERTEX},
				{D3D12_ROOT_PARAMETER_TYPE_CBV, {1, 0}, D3D12_SHADER_VISIBILITY_VERTEX},

				// InstanceID
				{.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				 .Constants = {.ShaderRegister = 2, .RegisterSpace = 0, .Num32BitValues = 1},
				 .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL},

				instance_buffer_parameter };

			root_params[0].Descriptor.RegisterSpace = 0;
			root_params[1].Descriptor.RegisterSpace = 0;

			D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
			root_signature_desc.NumParameters = _countof(root_params);
			root_signature_desc.pParameters = &root_params[0];
			root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

			ID3D12RootSignature* root_signature = nullptr;

			result = renderer->CreateRootSignature(&root_signature_desc, &root_signature);

			if (!result)
			{
				LogError << "Can't create root signature for primitive\n";
				return false;
			}

			pso_desc.SetRootSignature(root_signature);
		}

		pso_desc.AddShader({ ShaderType::Vertex, VfsPath(L"shaders/forward_pass.vs.hlsl"), {L"SHADOW_PASS"} });

		D3D12_DEPTH_STENCIL_DESC depth_stencil_desc = {};
		depth_stencil_desc.DepthEnable = true;
		depth_stencil_desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		depth_stencil_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

		pso_desc.SetDepthStencilState(depth_stencil_desc);
		pso_desc.SetInputLayout(Vertex::GetVertexLayoutDesc());
		pso_desc.SetSampleMask(UINT_MAX);

		const auto material = materials[primitive.material_id];

		// TODO: Do I need it?
		auto blend_desc = material.blend_desc;
		blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		D3D12_RASTERIZER_DESC raster_desc = {};
		raster_desc.FillMode = D3D12_FILL_MODE_SOLID;
		raster_desc.CullMode = D3D12_CULL_MODE_NONE;

		pso_desc.SetRasterizerState(raster_desc);
		pso_desc.SetBlendState(blend_desc);

		switch (primitive.topology)
		{
			case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
				pso_desc.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);
				break;
			case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
			case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
				pso_desc.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
				break;
			case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
			case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
				pso_desc.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
				break;
			default:
				AssertMsg(false, "Unsupported primitive topology");
		}

		pso_desc.SetDepthTargetFormat(DXGI_FORMAT_D32_FLOAT);

		auto result_pso = renderer->BuildPso(pso_desc);

		if (!result_pso.has_value())
		{
			return false;
		}

		primitive.shadow_pso_id = *result_pso;

		return true;
	}

	bool ShadowMapPass::InitializeShadowMapBuffer(std::shared_ptr<DxRenderer> p_renderer)
	{
		HRESULT result = S_OK;
		const DXGI_FORMAT ShadowMapFormat = DXGI_FORMAT_D32_FLOAT;

		D3D12_CLEAR_VALUE optimizedClearValue = {};
		optimizedClearValue.Format = ShadowMapFormat;
		optimizedClearValue.DepthStencil = { 1.0f, 0 };

		const CD3DX12_HEAP_PROPERTIES heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

		const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			ShadowMapFormat, ShadowMapDimension, ShadowMapDimension, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

		result = p_renderer->GetDevice()->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&optimizedClearValue,
			IID_PPV_ARGS(&shadow_map_buffer.buffer));

		if (result != S_OK)
			return false;

		D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
		dsv.Format = ShadowMapFormat;
		dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsv.Texture2D.MipSlice = 0;
		dsv.Flags = D3D12_DSV_FLAG_NONE;

		D3D12_SHADER_RESOURCE_VIEW_DESC srv_view_desc = {};
		srv_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_view_desc.Format = DXGI_FORMAT_R32_FLOAT;
		srv_view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_view_desc.Texture2D.MipLevels = 1;

		shadow_map_buffer.dsv_handle = p_renderer->GetDsvDescriptorHeap()->GetNewHandle();
		shadow_map_buffer.srv_handle = p_renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();

		p_renderer->GetDevice()->CreateDepthStencilView(shadow_map_buffer.buffer.get(), &dsv, shadow_map_buffer.dsv_handle.cpu);
		p_renderer->GetDevice()->CreateShaderResourceView(shadow_map_buffer.buffer.get(), &srv_view_desc, shadow_map_buffer.srv_handle.cpu);

		Viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(ShadowMapDimension), static_cast<float>(ShadowMapDimension));
		ScissorRect = CD3DX12_RECT(0, 0, ShadowMapDimension, ShadowMapDimension);

		return true;
	}

	void ShadowMapPass::InitializeOrthProjection(DirectX::SimpleMath::Vector3 direction)
	{
		direction = NormalizeOrDefault(direction, DirectX::SimpleMath::Vector3{ 0.1f, -1.0f, 0.45f });

		const float extent = 100.0f;
		const float eye_distance = 175.0f;
		const DirectX::SimpleMath::Vector3 frustum_center = DirectX::SimpleMath::Vector3::Zero;
		const DirectX::SimpleMath::Vector3 eye = frustum_center - direction * eye_distance;
		const auto up = std::abs(direction.Dot(DirectX::SimpleMath::Vector3::Up)) > 0.99f
			? DirectX::SimpleMath::Vector3{ 1.0f, 0.0f, 0.0f }
			: DirectX::SimpleMath::Vector3::Up;

		DirectX::XMMATRIX projection =
			DirectX::XMMatrixOrthographicOffCenterRH(-extent, extent, -extent, extent, 0.1f, 500.0f);
		DirectX::XMMATRIX view = DirectX::XMMatrixLookAtRH(eye, frustum_center, up);
		shadow_matrix = view * projection;
	}

	void ShadowMapPass::UpdateLight(const DirectionalLight& directional_light)
	{
		m_light_direction = NormalizeOrDefault(
			SimpleMath::Vector3{ directional_light.direction.x, directional_light.direction.y, directional_light.direction.z },
			DirectX::SimpleMath::Vector3{ 0.1f, -1.0f, 0.45f });

		InitializeOrthProjection(m_light_direction);
	}

	void ShadowMapPass::UpdateShadowMatrixForCamera(const RenderScene& render_scene)
	{
		if (!render_scene.camera)
		{
			return;
		}

		const float follow_distance = std::max(0.0f, shadow_follow_distance);
		const float eye_distance = std::max(1.0f, shadow_eye_distance);
		const float extent = std::max(1.0f, shadow_extent);
		const float near_plane = std::max(0.01f, shadow_near_plane);
		const float far_plane = std::max(near_plane + 1.0f, shadow_far_plane);

		const auto camera_position = SimpleMath::Vector3(render_scene.camera->GetPosition());
		const auto camera_forward =
			NormalizeOrDefault(SimpleMath::Vector3(render_scene.camera->GetForwardVector()), SimpleMath::Vector3{ 0.0f, 0.0f, 1.0f });
		const auto focus_position = camera_position + camera_forward * follow_distance;

		const auto light_direction = NormalizeOrDefault(m_light_direction, SimpleMath::Vector3{ 0.1f, -1.0f, 0.45f });
		const auto light_up = std::abs(light_direction.Dot(SimpleMath::Vector3::Up)) > 0.99f
			? SimpleMath::Vector3{ 1.0f, 0.0f, 0.0f }
			: SimpleMath::Vector3::Up;
		auto light_eye = focus_position - light_direction * eye_distance;

		auto view = DirectX::XMMatrixLookAtRH(light_eye, focus_position, light_up);

		// Stabilize directional shadows: snap projection center to shadow texel grid to reduce shimmer.
		const float texel_world_size = (extent * 2.0f) / static_cast<float>(ShadowMapDimension);
		if (texel_world_size > 0.0f)
		{
			const auto focus_light_space_v = DirectX::XMVector3TransformCoord(focus_position, view);
			DirectX::SimpleMath::Vector3 focus_light_space;
			DirectX::XMStoreFloat3(&focus_light_space, focus_light_space_v);

			const float snapped_x = std::floor(focus_light_space.x / texel_world_size + 0.5f) * texel_world_size;
			const float snapped_y = std::floor(focus_light_space.y / texel_world_size + 0.5f) * texel_world_size;

			const DirectX::SimpleMath::Vector3 delta_light = {
				snapped_x - focus_light_space.x,
				snapped_y - focus_light_space.y,
				0.0f
			};

			const auto inv_view = DirectX::XMMatrixInverse(nullptr, view);
			const auto delta_world_v = DirectX::XMVector3TransformNormal(delta_light, inv_view);
			DirectX::SimpleMath::Vector3 delta_world;
			DirectX::XMStoreFloat3(&delta_world, delta_world_v);

			light_eye += delta_world;
			const auto snapped_focus = focus_position + delta_world;
			view = DirectX::XMMatrixLookAtRH(light_eye, snapped_focus, light_up);
		}

		const auto projection = DirectX::XMMatrixOrthographicOffCenterRH(
			-extent,
			extent,
			-extent,
			extent,
			near_plane,
			far_plane);

		shadow_matrix = view * projection;
	}

	bool ShadowMapPass::Render(const RenderScene& render_scene, const ShadowRenderParameters& parameters)
	{
		auto renderer = Application::Get().GetRenderer();

		const auto command_list_result = parameters.command_queue->GetCommandList("ShadowMap");

		if (!command_list_result.has_value())
			return false;

		auto command_list = command_list_result.value();

		ID3D12DescriptorHeap* pDescriptorHeaps[] = {
			renderer->GetCbvSrvUavDescriptorHeap()->GetHeapPtr(),
		};
		command_list->SetDescriptorHeaps(_countof(pDescriptorHeaps), pDescriptorHeaps);

		UpdateShadowMatrixForCamera(render_scene);

		command_list->ClearDepthStencilView(shadow_map_buffer.dsv_handle.cpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		command_list->RSSetViewports(1, &Viewport);
		command_list->RSSetScissorRects(1, &ScissorRect);
		command_list->OMSetRenderTargets(0, nullptr, FALSE, &shadow_map_buffer.dsv_handle.cpu);

		{
			void* data = nullptr;
			m_camera_buffer->Map(0, nullptr, &data);
			auto* pCameraData = static_cast<ShadowCamera*>(data);
			pCameraData->view_projection = shadow_matrix;
			m_camera_buffer->Unmap(0, nullptr);
		}

		uint32_t instance_id = 0;

		for (auto& model : render_scene.models)
		{
			for (int mesh_id = 0; mesh_id < model.meshes.size(); mesh_id++)
			{
				const Mesh& mesh = model.meshes[mesh_id];

				for (int primitive_id = 0; primitive_id < mesh.primitives.size(); primitive_id++)
				{
					const Primitive& primitive = mesh.primitives[primitive_id];

					command_list->IASetVertexBuffers(0, 1, &primitive.vertex_buffer_view);

					// TODO: check for -1 as pso_id
					const std::optional<Pso> pso = renderer->GetPso(primitive.shadow_pso_id);

					if (pso.has_value())
					{
						command_list->SetGraphicsRootSignature(pso.value().root_signature.get());
						command_list->SetPipelineState(pso.value().pso.get());

						command_list->IASetPrimitiveTopology(primitive.topology);

						command_list->SetGraphicsRootConstantBufferView(0, m_camera_buffer->GetGPUVirtualAddress());
						command_list->SetGraphicsRootConstantBufferView(
							1, parameters.scene_parameters_gpu_buffer->GetGPUVirtualAddress());
						command_list->SetGraphicsRoot32BitConstant(2, instance_id, 0); // InstanceID

						command_list->SetGraphicsRootDescriptorTable(3, render_scene.instance_buffer_srv.gpu); // PerInstanceData

						if (primitive.index_count)
						{
							command_list->IASetIndexBuffer(&primitive.index_buffer_view);
							command_list->DrawIndexedInstanced(primitive.index_count, 1, 0, 0, 0);
						}
						else
						{
							command_list->DrawInstanced(primitive.vertex_count, 1, 0, 0);
						}
					}
					else
					{
						LogInfo << "Can't render primitive because it has't any pso attached\n";
					}

					instance_id++;
				}
			}
		}

		parameters.command_queue->CloseCommandList(command_list);

		return true;
	}
}
