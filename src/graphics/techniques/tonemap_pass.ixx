module;

#include <wil/com.h>
#include <d3dx12.h>

#include <shader_structs.h>

export module graphics.techniques.tonemap_pass;

import std;
import renderer.shader_storage;
import renderer.dx_renderer;
import renderer.descriptor_heap;
import renderer.command_queue;
import renderer.pso;
import system.math;
import system.filesystem;
import system.application;
import system.logger;

export namespace ysn
{
	enum class TonemapMethod : uint8_t
	{
		None,
		Reinhard,
		Aces
	};

	struct TonemapPostprocessParameters
	{
		std::shared_ptr<CommandQueue> command_queue = nullptr;
		std::shared_ptr<CbvSrvUavDescriptorHeap> cbv_srv_uav_heap = nullptr;
		wil::com_ptr<ID3D12Resource> scene_color_buffer = nullptr;
		DescriptorHandle hdr_uav_descriptor_handle;
		DescriptorHandle backbuffer_uav_descriptor_handle;
		uint32_t backbuffer_width = 0;
		uint32_t backbuffer_height = 0;
	};

	class TonemapPostprocess
	{
	public:
		bool Initialize();
		bool Render(TonemapPostprocessParameters* parameters);

		wil::com_ptr<ID3D12Resource> parameters_buffer;

		float exposure = 1.0f;
		TonemapMethod tonemap_method = TonemapMethod::Aces;
	private:
		PsoId m_pso_id;
	};
}

module :private;

namespace ysn
{
	static bool CreateParameters(wil::com_ptr<ID3D12Resource>& parameters_buffer)
	{
		wil::com_ptr<ID3D12Device5> device = Application::Get().GetDevice();

		D3D12_HEAP_PROPERTIES heapProperties = {};
		heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC resourceDesc = {};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Width = GetGpuSize<TonemapParameters>();
		resourceDesc.Height = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		resourceDesc.SampleDesc = { 1, 0 };
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		auto hr = device->CreateCommittedResource(
			&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&parameters_buffer));

		if (hr != S_OK)
		{
			LogError << "Can't create tonemap parameters buffer\n";
			return false;
		}

		return true;
	}

	void UpdateParameters(wil::com_ptr<ID3D12Resource> parameters_buffer, uint32_t width, uint32_t height, float exposure, TonemapMethod method)
	{
		void* data;
		parameters_buffer->Map(0, nullptr, &data);

		auto* tonemap_parameters = static_cast<TonemapParameters*>(data);

		tonemap_parameters->display_width = width;
		tonemap_parameters->display_height = height;
		tonemap_parameters->exposure = std::max(0.0f, exposure);
		tonemap_parameters->tonemap_method = static_cast<uint32_t>(method);

		parameters_buffer->Unmap(0, nullptr);
	}

	bool TonemapPostprocess::Initialize()
	{
		auto renderer = Application::Get().GetRenderer();

		CD3DX12_DESCRIPTOR_RANGE hdr_texture_uav(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0);
		CD3DX12_DESCRIPTOR_RANGE ldr_texture_uav(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0);

		CD3DX12_ROOT_PARAMETER root_parameters[3] = {};
		root_parameters[0].InitAsDescriptorTable(1, &hdr_texture_uav);
		root_parameters[1].InitAsDescriptorTable(1, &ldr_texture_uav);
		root_parameters[2].InitAsConstantBufferView(0);

		ID3D12RootSignature* root_signature = nullptr;

		// Setting Root Signature
		D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
		root_signature_desc.NumParameters = 3;
		root_signature_desc.pParameters = &root_parameters[0];

		renderer->CreateRootSignature(&root_signature_desc, &root_signature);

		ComputePsoDesc pso_desc("Tonemap");
		pso_desc.AddShader({ ShaderType::Compute, VfsPath(L"shaders/tonemap.cs.hlsl") });
		pso_desc.SetRootSignature(root_signature);

		auto pso_result = renderer->BuildPso(pso_desc);

		if (!pso_result.has_value())
			return false;

		m_pso_id = *pso_result;

		if (!CreateParameters(parameters_buffer))
		{
			LogError << "Can't create tonemap parameters\n";
			return false;
		}

		return true;
	}

	bool TonemapPostprocess::Render(TonemapPostprocessParameters* parameters)
	{
		auto renderer = Application::Get().GetRenderer();

		UpdateParameters(parameters_buffer, parameters->backbuffer_width, parameters->backbuffer_height, exposure, tonemap_method);

		const auto command_list_result = parameters->command_queue->GetCommandList("Tonemap");

		if (!command_list_result.has_value())
			return false;

		auto command_list = command_list_result.value();

		const std::optional<Pso> pso = renderer->GetPso(m_pso_id);
		if (!pso.has_value())
			return false;

		auto& pso_object = pso.value();

		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			parameters->scene_color_buffer.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		command_list->ResourceBarrier(1, &barrier);

		command_list->SetPipelineState(pso_object.pso.get());
		command_list->SetComputeRootSignature(pso_object.root_signature.get());

		ID3D12DescriptorHeap* ppHeaps[] = { parameters->cbv_srv_uav_heap->GetHeapPtr() };
		command_list->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

		command_list->SetComputeRootDescriptorTable(0, parameters->hdr_uav_descriptor_handle.gpu);
		command_list->SetComputeRootDescriptorTable(1, parameters->backbuffer_uav_descriptor_handle.gpu);
		command_list->SetComputeRootConstantBufferView(2, parameters_buffer->GetGPUVirtualAddress());

		command_list->Dispatch(
			UINT(std::ceil(parameters->backbuffer_width / (float)8)), UINT(std::ceil(parameters->backbuffer_height / (float)8)), 1);

		parameters->command_queue->CloseCommandList(command_list);

		return true;
	}
}
