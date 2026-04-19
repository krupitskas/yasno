module;

#include <d3d12.h>
#include <d3dx12.h>
#include <wil/com.h>
#include <DirectXMath.h>

export module graphics.techniques.convolve_cubemap;

import system.string_helpers;
import system.filesystem;
import system.application;
import system.logger;
import renderer.dx_renderer;
import renderer.pso;
import renderer.gpu_texture;
import renderer.gpu_pixel_buffer;
import renderer.vertex_storage;
import graphics.primitive;

export namespace ysn
{
	struct ConvolveCubemapParameters
	{
		wil::com_ptr<ID3D12Resource> camera_buffer;
		GpuPixelBuffer3D source_cubemap;
		GpuPixelBuffer3D target_irradiance;
		GpuPixelBuffer3D target_radiance;
	};

	class ConvolveCubemap
	{
	public:
		ConvolveCubemap() = default;
		bool Initialize();
		bool ComputeBRDF();
		bool ConvolveIrradiance(ConvolveCubemapParameters& parameters);
		bool ConvolveRadiance(ConvolveCubemapParameters& parameters);
		DescriptorHandle GetBrdfTextureHandle() const;
	private:
		bool InitializeIrradiance();
		bool InitializeRadiance();
		bool InitializeBrdf();

		PsoId m_irradiance_pso_id;
		PsoId m_radiance_pso_id;
		PsoId m_brdf_bake_pso_id;

		MeshPrimitive cube;

		DirectX::XMMATRIX m_projection;
		DirectX::XMMATRIX m_views[6];

		D3D12_RECT m_rect;

		// Split Sum BRDF texture
		wil::com_ptr<ID3D12Resource> m_brdf_texture;
		DescriptorHandle m_brdf_uav_handle;
		DescriptorHandle m_brdf_srv_handle;
		D3D12_RESOURCE_STATES m_brdf_texture_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		const uint32_t m_brdf_texture_size = 512;
	};
}

module :private;

namespace ysn
{
	using namespace DirectX;

	enum class IrradianceShaderParameters : uint8_t
	{
		View,
		Projection,
		InputTexture,
		ParametersCount
	};

	enum class RadianceShaderParameters : uint8_t
	{
		View,
		Projection,
		Parameters,
		InputTexture,
		ParametersCount
	};

	enum class BrdfBakeParameters : uint8_t
	{
		BrdfTexture,
		ParametersCount
	};

	bool ConvolveCubemap::Initialize()
	{
		const auto box_result = ConstructBox();
		if (!box_result.has_value())
			return false;
		cube = box_result.value();

		if (!InitializeIrradiance())
			return false;

		if (!InitializeRadiance())
			return false;

		if (!InitializeBrdf())
			return false;

		const XMVECTOR eye = XMVectorSet(0.0, 0.0, 0.0, 1.0);

		XMVECTOR targets[6] =
		{
			XMVectorAdd(eye, XMVectorSet(1.0f,  0.0f,  0.0f, 0.0f)),  // +X
			XMVectorAdd(eye, XMVectorSet(-1.0f,  0.0f,  0.0f, 0.0f)), // -X
			XMVectorAdd(eye, XMVectorSet(0.0f,  1.0f,  0.0f, 0.0f)),  // +Y
			XMVectorAdd(eye, XMVectorSet(0.0f, -1.0f,  0.0f, 0.0f)),  // -Y
			XMVectorAdd(eye, XMVectorSet(0.0f,  0.0f, -1.0f, 0.0f)),  // -Z
			XMVectorAdd(eye, XMVectorSet(0.0f,  0.0f,  1.0f, 0.0f)),  // +Z
		};

		XMVECTOR ups[6] =
		{
			XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),  // +X
			XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),  // -X
			XMVectorSet(0.0f, 0.0f,  1.0f, 0.0f),  // +Y
			XMVectorSet(0.0f, 0.0f,  -1.0f, 0.0f), // -Y
			XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),  // -Z
			XMVectorSet(0.0f, 1.0f,  0.0f, 0.0f),  // +Z
		};

		for (int i = 0; i < 6; i++)
			m_views[i] = XMMatrixLookAtRH(eye, targets[i], ups[i]);

		m_projection = XMMatrixPerspectiveFovRH(XMConvertToRadians(90.f), 1.0f, 0.1f, 10.f);

		m_rect = CD3DX12_RECT(0, 0, LONG_MAX, LONG_MAX);

		return true;
	}

	bool ConvolveCubemap::ComputeBRDF()
	{
		auto renderer = Application::Get().GetRenderer();

		const auto command_list_result = renderer->GetDirectQueue()->GetCommandList("Bake BRDF");

		if (!command_list_result.has_value())
			return false;

		auto command_list = command_list_result.value();

		const std::optional<Pso> pso = renderer->GetPso(m_brdf_bake_pso_id);
		if (!pso.has_value())
			return false;

		auto& pso_object = pso.value();

		command_list->SetPipelineState(pso_object.pso.get());
		command_list->SetComputeRootSignature(pso_object.root_signature.get());

		ID3D12DescriptorHeap* ppHeaps[] = { renderer->GetCbvSrvUavDescriptorHeap()->GetHeapPtr() };
		command_list->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

		if (m_brdf_texture_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		{
			CD3DX12_RESOURCE_BARRIER to_uav = CD3DX12_RESOURCE_BARRIER::Transition(
				m_brdf_texture.get(), m_brdf_texture_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			command_list->ResourceBarrier(1, &to_uav);
			m_brdf_texture_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		}

		command_list->SetComputeRootDescriptorTable(0, m_brdf_uav_handle.gpu);

		command_list->Dispatch(
			UINT(std::ceil(m_brdf_texture_size / (float)8)), UINT(std::ceil(m_brdf_texture_size / (float)8)), 1);

		{
			CD3DX12_RESOURCE_BARRIER uav_barrier = CD3DX12_RESOURCE_BARRIER::UAV(m_brdf_texture.get());
			command_list->ResourceBarrier(1, &uav_barrier);
		}

		{
			CD3DX12_RESOURCE_BARRIER to_srv = CD3DX12_RESOURCE_BARRIER::Transition(
				m_brdf_texture.get(), m_brdf_texture_state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			command_list->ResourceBarrier(1, &to_srv);
			m_brdf_texture_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		}

		renderer->GetDirectQueue()->CloseCommandList(command_list);

		return true;
	}

	bool ConvolveCubemap::InitializeIrradiance()
	{
		const auto renderer = Application::Get().GetRenderer();

		CD3DX12_DESCRIPTOR_RANGE source_texture_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0);

		CD3DX12_ROOT_PARAMETER root_parameters[(uint8_t)IrradianceShaderParameters::ParametersCount] = {};
		root_parameters[(uint8_t)IrradianceShaderParameters::View].InitAsConstants(sizeof(XMMATRIX) / sizeof(float), 0);
		root_parameters[(uint8_t)IrradianceShaderParameters::Projection].InitAsConstants(sizeof(XMMATRIX) / sizeof(float), 1);
		root_parameters[(uint8_t)IrradianceShaderParameters::InputTexture].InitAsDescriptorTable(1, &source_texture_srv);

		CD3DX12_STATIC_SAMPLER_DESC static_sampler(
			0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

		D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
		root_signature_desc.NumParameters = (uint8_t)IrradianceShaderParameters::ParametersCount;
		root_signature_desc.pParameters = &root_parameters[0];
		root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		root_signature_desc.pStaticSamplers = &static_sampler;
		root_signature_desc.NumStaticSamplers = 1;

		ID3D12RootSignature* root_signature = nullptr;
		bool result = Application::Get().GetRenderer()->CreateRootSignature(&root_signature_desc, &root_signature);
		if (!result)
		{
			LogError << "Can't create ConvertToCubemap root signature\n";
			return false;
		}

		D3D12_RASTERIZER_DESC rasterizer_desc = {};
		rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE; // TODO: Should be back?

		D3D12_BLEND_DESC blend_desc = {};
		blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		GraphicsPsoDesc pso_desc("Filter Irradiance Cubemap PSO");
		pso_desc.AddShader({ ShaderType::Vertex, VfsPath(L"shaders/convolve_cubemap.vs.hlsl") });
		pso_desc.AddShader({ ShaderType::Pixel, VfsPath(L"shaders/convolve_irradiance.ps.hlsl") });
		pso_desc.SetInputLayout(Vertex::GetVertexLayoutDesc());
		pso_desc.SetRasterizerState(rasterizer_desc);
		pso_desc.SetBlendState(blend_desc);
		pso_desc.SetRootSignature(root_signature);
		pso_desc.SetDepthStencilState({ .DepthEnable = false });
		pso_desc.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		pso_desc.SetSampleMask(UINT_MAX);
		pso_desc.SetRenderTargetFormat(renderer->GetHdrFormat(), renderer->GetDepthBufferFormat());

		auto result_pso = renderer->BuildPso(pso_desc);

		if (!result_pso.has_value())
			return false;

		m_irradiance_pso_id = *result_pso;

		return true;
	}

	bool ConvolveCubemap::InitializeBrdf()
	{
		const auto renderer = Application::Get().GetRenderer();

		D3D12_CLEAR_VALUE optimized_clear_valuie = {};
		optimized_clear_valuie.Format = DXGI_FORMAT_R16G16_FLOAT;
		optimized_clear_valuie.Color[0] = 0.0f;
		optimized_clear_valuie.Color[1] = 0.0f;


		const auto heap_properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

		const auto resource_desc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R16G16_FLOAT,
			m_brdf_texture_size,
			m_brdf_texture_size,
			1,
			0,
			1,
			0,
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		if (auto result = renderer->GetDevice()->CreateCommittedResource(
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&resource_desc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			&optimized_clear_valuie,
			IID_PPV_ARGS(&m_brdf_texture));
			result != S_OK)
		{
			LogFatal << "Can't create brdf texture " << ConvertHrToString(result) << " \n";
			return false;
		}

		m_brdf_texture->SetName(L"BRDF Texture");

		m_brdf_uav_handle = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();

		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
			uav_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
			uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

			renderer->GetDevice()->CreateUnorderedAccessView(m_brdf_texture.get(), nullptr, &uav_desc, m_brdf_uav_handle.cpu);
		}

		m_brdf_srv_handle = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
			srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv_desc.Texture2D.MipLevels = 1;
			srv_desc.Texture2D.MostDetailedMip = 0;

			renderer->GetDevice()->CreateShaderResourceView(m_brdf_texture.get(), &srv_desc, m_brdf_srv_handle.cpu);
		}

		CD3DX12_DESCRIPTOR_RANGE brdf_texture_uav(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0);

		CD3DX12_ROOT_PARAMETER root_parameters[(uint8_t)BrdfBakeParameters::ParametersCount] = {};
		root_parameters[(uint8_t)BrdfBakeParameters::BrdfTexture].InitAsDescriptorTable(1, &brdf_texture_uav);

		ID3D12RootSignature* root_signature = nullptr;

		// Setting Root Signature
		D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
		root_signature_desc.NumParameters = (uint8_t)BrdfBakeParameters::ParametersCount;
		root_signature_desc.pParameters = &root_parameters[0];

		renderer->CreateRootSignature(&root_signature_desc, &root_signature);

		ComputePsoDesc pso_desc("BRDF Bake PSO");
		pso_desc.AddShader({ ShaderType::Compute, VfsPath(L"shaders/bake_brdf.cs.hlsl") });
		pso_desc.SetRootSignature(root_signature);

		auto pso_result = renderer->BuildPso(pso_desc);

		if (!pso_result.has_value())
			return false;

		m_brdf_bake_pso_id = *pso_result;

		return true;
	}

	bool ConvolveCubemap::InitializeRadiance()
	{
		const auto renderer = Application::Get().GetRenderer();

		CD3DX12_DESCRIPTOR_RANGE source_texture_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0);

		CD3DX12_ROOT_PARAMETER root_parameters[(uint8_t)RadianceShaderParameters::ParametersCount] = {};
		root_parameters[(uint8_t)RadianceShaderParameters::View].InitAsConstants(sizeof(XMMATRIX) / sizeof(float), 0);
		root_parameters[(uint8_t)RadianceShaderParameters::Projection].InitAsConstants(sizeof(XMMATRIX) / sizeof(float), 1);
		root_parameters[(uint8_t)RadianceShaderParameters::Parameters].InitAsConstants(sizeof(float) / sizeof(float), 2);
		root_parameters[(uint8_t)RadianceShaderParameters::InputTexture].InitAsDescriptorTable(1, &source_texture_srv);

		CD3DX12_STATIC_SAMPLER_DESC static_sampler(
			0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

		D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
		root_signature_desc.NumParameters = (uint8_t)RadianceShaderParameters::ParametersCount;
		root_signature_desc.pParameters = &root_parameters[0];
		root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		root_signature_desc.pStaticSamplers = &static_sampler;
		root_signature_desc.NumStaticSamplers = 1;

		ID3D12RootSignature* root_signature = nullptr;
		bool result = Application::Get().GetRenderer()->CreateRootSignature(&root_signature_desc, &root_signature);
		if (!result)
		{
			LogError << "Can't create radiance cubemap root signature\n";
			return false;
		}

		D3D12_RASTERIZER_DESC rasterizer_desc = {};
		rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE; // TODO: Should be back?

		D3D12_BLEND_DESC blend_desc = {};
		blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		GraphicsPsoDesc pso_desc("Filter Radiance Cubemap PSO");
		pso_desc.AddShader({ ShaderType::Vertex, VfsPath(L"shaders/convolve_cubemap.vs.hlsl") });
		pso_desc.AddShader({ ShaderType::Pixel, VfsPath(L"shaders/convolve_radiance.ps.hlsl") });
		pso_desc.SetInputLayout(Vertex::GetVertexLayoutDesc());
		pso_desc.SetRasterizerState(rasterizer_desc);
		pso_desc.SetBlendState(blend_desc);
		pso_desc.SetRootSignature(root_signature);
		pso_desc.SetDepthStencilState({
			.DepthEnable = false,
									  });
		pso_desc.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		pso_desc.SetSampleMask(UINT_MAX);
		pso_desc.SetRenderTargetFormat(renderer->GetHdrFormat(), renderer->GetDepthBufferFormat());

		auto result_pso = renderer->BuildPso(pso_desc);

		if (!result_pso.has_value())
			return false;

		m_radiance_pso_id = *result_pso;

		return true;
	}

	bool ConvolveCubemap::ConvolveIrradiance(ConvolveCubemapParameters& parameters)
	{
		const auto renderer = Application::Get().GetRenderer();

		const std::optional<Pso> pso = renderer->GetPso(m_irradiance_pso_id);

		if (!pso.has_value())
		{
			LogError << "Can't find convolve irradiance PSO\n";
			return false;
		}

		auto& pso_object = pso.value();

		const auto command_list_result = renderer->GetDirectQueue()->GetCommandList("Convolve Irradiance Cubemap");

		if (!command_list_result.has_value())
			return false;

		auto command_list = command_list_result.value();

		CD3DX12_RESOURCE_BARRIER barrier_before = CD3DX12_RESOURCE_BARRIER::Transition(
			parameters.target_irradiance.Resource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		command_list->ResourceBarrier(1, &barrier_before);

		ID3D12DescriptorHeap* ppHeaps[] = { renderer->GetCbvSrvUavDescriptorHeap()->GetHeapPtr() };

		const auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f,
							   static_cast<float>(parameters.target_irradiance->GetDesc().Width),
							   static_cast<float>(parameters.target_irradiance->GetDesc().Height));

		command_list->RSSetViewports(1, &viewport);
		command_list->RSSetScissorRects(1, &m_rect);
		command_list->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
		command_list->SetPipelineState(pso_object.pso.get());
		command_list->SetGraphicsRootSignature(pso_object.root_signature.get());
		command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		command_list->IASetVertexBuffers(0, 1, &cube.vertex_buffer_view);
		command_list->IASetIndexBuffer(&cube.index_buffer_view);

		command_list->SetGraphicsRootDescriptorTable((int)IrradianceShaderParameters::InputTexture, parameters.source_cubemap.srv.gpu);

		for (int face = 0; face < 6; face++)
		{
			float view[16];
			XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(view), m_views[face]);

			float projection[16];
			XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(projection), m_projection);

			command_list->SetGraphicsRoot32BitConstants((uint8_t)IrradianceShaderParameters::View, 16, view, 0);
			command_list->SetGraphicsRoot32BitConstants((uint8_t)IrradianceShaderParameters::Projection, 16, projection, 0);
			command_list->OMSetRenderTargets(1, &parameters.target_irradiance.rtv[0][face].cpu, FALSE, nullptr);
			command_list->DrawIndexedInstanced(cube.index_count, 1, 0, 0, 0);
		}

		CD3DX12_RESOURCE_BARRIER barrier_after = CD3DX12_RESOURCE_BARRIER::Transition(
			parameters.target_irradiance.Resource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		command_list->ResourceBarrier(1, &barrier_after);

		renderer->GetDirectQueue()->CloseCommandList(command_list);

		return true;
	}

	bool ConvolveCubemap::ConvolveRadiance(ConvolveCubemapParameters& parameters)
	{
		const auto renderer = Application::Get().GetRenderer();

		const std::optional<Pso> pso = renderer->GetPso(m_radiance_pso_id);

		if (!pso.has_value())
		{
			LogError << "Can't find convolve radiance PSO\n";
			return false;
		}

		auto& pso_object = pso.value();

		const auto command_list_result = renderer->GetDirectQueue()->GetCommandList("Filter Radiance");

		if (!command_list_result.has_value())
			return false;

		auto command_list = command_list_result.value();

		CD3DX12_RESOURCE_BARRIER barrier_before = CD3DX12_RESOURCE_BARRIER::Transition(
			parameters.target_radiance.Resource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		command_list->ResourceBarrier(1, &barrier_before);

		ID3D12DescriptorHeap* ppHeaps[] = { renderer->GetCbvSrvUavDescriptorHeap()->GetHeapPtr() };


		command_list->RSSetScissorRects(1, &m_rect);
		command_list->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
		command_list->SetPipelineState(pso_object.pso.get());
		command_list->SetGraphicsRootSignature(pso_object.root_signature.get());
		command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		command_list->IASetVertexBuffers(0, 1, &cube.vertex_buffer_view);
		command_list->IASetIndexBuffer(&cube.index_buffer_view);

		command_list->SetGraphicsRootDescriptorTable((uint8_t)RadianceShaderParameters::InputTexture, parameters.source_cubemap.srv.gpu);

		const uint32_t max_mip_levels = parameters.target_radiance->GetDesc().MipLevels;

		for (uint32_t mip = 0; mip < max_mip_levels; mip++)
		{
			unsigned int mip_width = parameters.target_radiance->GetDesc().Width * std::pow(0.5, mip);
			unsigned int mip_height = parameters.target_radiance->GetDesc().Height * std::pow(0.5, mip);

			float roughness = (float)mip / (float)(max_mip_levels - 1); // TODO: provide

			for (int face = 0; face < 6; face++)
			{
				const auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f,
													   static_cast<float>(mip_width),
													   static_cast<float>(mip_height));

				command_list->RSSetViewports(1, &viewport);

				float view[16];
				XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(view), m_views[face]);

				float projection[16];
				XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(projection), m_projection);

				command_list->SetGraphicsRoot32BitConstants((uint8_t)RadianceShaderParameters::View, 16, view, 0);
				command_list->SetGraphicsRoot32BitConstants((uint8_t)RadianceShaderParameters::Projection, 16, projection, 0);
				command_list->SetGraphicsRoot32BitConstants((uint8_t)RadianceShaderParameters::Parameters, 1, &roughness, 0);
				command_list->OMSetRenderTargets(1, &parameters.target_radiance.rtv[mip][face].cpu, FALSE, nullptr);
				command_list->DrawIndexedInstanced(cube.index_count, 1, 0, 0, 0);
			}
		}

		CD3DX12_RESOURCE_BARRIER barrier_after = CD3DX12_RESOURCE_BARRIER::Transition(
			parameters.target_radiance.Resource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		command_list->ResourceBarrier(1, &barrier_after);

		renderer->GetDirectQueue()->CloseCommandList(command_list);

		return true;
	}

	DescriptorHandle ConvolveCubemap::GetBrdfTextureHandle() const
	{
		return m_brdf_srv_handle;
	}
}
