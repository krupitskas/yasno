module;

#include <DirectXMath.h>
#include <d3dx12.h>
#include <wil/com.h>
#include <shader_structs.h>

export module graphics.techniques.volumetric_fog;

import graphics.primitive;
import renderer.dx_renderer;
import renderer.descriptor_heap;
import renderer.vertex_storage;
import renderer.gpu_buffer;
import renderer.dx_types;
import renderer.command_queue;
import renderer.pso;
import system.string_helpers;
import system.filesystem;
import system.application;
import system.logger;
import graphics.techniques.shadow_map_pass;

export namespace ysn
{
	struct VolumetricFogPassInput
	{
		ShadowMapBuffer shadow_map_buffer;
		DescriptorHandle hdr_uav_descriptor_handle;
		DescriptorHandle depth_srv;

		wil::com_ptr<ID3D12Resource> camera_gpu_buffer;
		wil::com_ptr<ID3D12Resource> scene_parameters_gpu_buffer;
		wil::com_ptr<ID3D12Resource> scene_color_buffer;

		uint32_t backbuffer_width = 0;
		uint32_t backbuffer_height = 0;
	};

	class VolumetricFog
	{
	public:
		bool Initialize();
		bool Render(const VolumetricFogPassInput& pass_input);
		void PrepareData(wil::com_ptr<DxGraphicsCommandList> cmd_list, uint32_t width, uint32_t height);

		GpuBuffer gpu_parameters;

		int num_samples = 128;
		bool is_half_res = false;

		bool is_pass_active = true;
	private:
		PsoId m_pso_id;
	};
}

module :private;

namespace ysn
{
	void VolumetricFog::PrepareData(wil::com_ptr<DxGraphicsCommandList> cmd_list, uint32_t width, uint32_t height)
	{
		// TODO: utilize cmd_list
		void* data;
		gpu_parameters->Map(0, nullptr, &data);

		auto* parameters = static_cast<VolumetricFogParameters*>(data);
		parameters->display_width = width;
		parameters->display_height = height;
		parameters->num_steps = num_samples;

		gpu_parameters->Unmap(0, nullptr);

		//UploadToGpuBuffer(cmd_list, gpu_parameters, &parameters, {}, D3D12_RESOURCE_STATE_GENERIC_READ);
	}

	bool VolumetricFog::Initialize()
	{
		auto renderer = Application::Get().GetRenderer();

		CD3DX12_DESCRIPTOR_RANGE shadow_map_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0);
		CD3DX12_DESCRIPTOR_RANGE depth_map_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0);
		CD3DX12_DESCRIPTOR_RANGE hdr_output_uav(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0);

		CD3DX12_ROOT_PARAMETER root_parameters[6] = {};
		root_parameters[0].InitAsDescriptorTable(1, &shadow_map_srv);
		root_parameters[1].InitAsDescriptorTable(1, &depth_map_srv);
		root_parameters[2].InitAsDescriptorTable(1, &hdr_output_uav);
		root_parameters[3].InitAsConstantBufferView(0);
		root_parameters[4].InitAsConstantBufferView(1);
		root_parameters[5].InitAsConstantBufferView(2);

		ID3D12RootSignature* root_signature = nullptr;

		// Setting Root Signature
		D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
		root_signature_desc.NumParameters = 6;
		root_signature_desc.pParameters = &root_parameters[0];

		renderer->CreateRootSignature(&root_signature_desc, &root_signature);

		ComputePsoDesc pso_desc("VolumetricFogTrace");
		pso_desc.AddShader({ ShaderType::Compute, VfsPath(L"shaders/volumetric_fog_trace.cs.hlsl") });
		pso_desc.SetRootSignature(root_signature);

		auto pso_result = renderer->BuildPso(pso_desc);

		if (!pso_result.has_value())
			return false;

		m_pso_id = *pso_result;

		GpuBufferCreateInfo create_info
		{
			.size = GetGpuSize<VolumetricFogParameters>(),
			.heap_type = D3D12_HEAP_TYPE_UPLOAD,
			.flags = D3D12_RESOURCE_FLAG_NONE,
			.state = D3D12_RESOURCE_STATE_GENERIC_READ,
		};

		const auto result = CreateGpuBuffer(create_info, "VolumetricFogParameters");

		if (!result.has_value())
		{
			LogError << "Can't create volumetric fog parameters\n";
			return false;
		}

		gpu_parameters = result.value();

		return true;
	}

	bool VolumetricFog::Render(const VolumetricFogPassInput& pass_input)
	{
		auto renderer = Application::Get().GetRenderer();

		const auto cmd_list_res = renderer->GetDirectQueue()->GetCommandList("Volumetric Fog");
		if (!cmd_list_res)
			return false;
		auto command_list = cmd_list_res.value();

		ID3D12DescriptorHeap* ppHeaps[] = { renderer->GetCbvSrvUavDescriptorHeap()->GetHeapPtr() };

		if (is_pass_active)
		{
			{
				CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					pass_input.scene_color_buffer.get(),
					D3D12_RESOURCE_STATE_RENDER_TARGET,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				command_list->ResourceBarrier(1, &barrier);
			}

			{
				CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					pass_input.shadow_map_buffer.buffer.get(),
					D3D12_RESOURCE_STATE_DEPTH_WRITE,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				command_list->ResourceBarrier(1, &barrier);
			}

			const std::optional<Pso> pso = renderer->GetPso(m_pso_id);
			if (!pso.has_value())
				return false;
			auto& pso_object = pso.value();

			command_list->SetPipelineState(pso_object.pso.get());
			command_list->SetComputeRootSignature(pso_object.root_signature.get());
			command_list->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

			command_list->SetComputeRootDescriptorTable(0, pass_input.shadow_map_buffer.srv_handle.gpu);
			command_list->SetComputeRootDescriptorTable(1, pass_input.depth_srv.gpu);
			command_list->SetComputeRootDescriptorTable(2, pass_input.hdr_uav_descriptor_handle.gpu);
			command_list->SetComputeRootConstantBufferView(3, gpu_parameters->GetGPUVirtualAddress());
			command_list->SetComputeRootConstantBufferView(4, pass_input.camera_gpu_buffer->GetGPUVirtualAddress());
			command_list->SetComputeRootConstantBufferView(5, pass_input.scene_parameters_gpu_buffer->GetGPUVirtualAddress());

			command_list->Dispatch(
				UINT(std::ceil(pass_input.backbuffer_width / (float)VolumetricFogDispatchX)),
				UINT(std::ceil(pass_input.backbuffer_height / (float)VolumetricFogDispatchY)),
				VolumetricFogDispatchZ);

			{
				CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					pass_input.shadow_map_buffer.buffer.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_DEPTH_WRITE);
				command_list->ResourceBarrier(1, &barrier);
			}

			{
				CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					pass_input.scene_color_buffer.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_RENDER_TARGET);
				command_list->ResourceBarrier(1, &barrier);
			}
		}

		renderer->GetDirectQueue()->CloseCommandList(command_list);

		return true;
	}
}
