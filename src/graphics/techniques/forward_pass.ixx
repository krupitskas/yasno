module;

#include <d3d12.h>
#include <d3dx12.h>
#include <wil/com.h>

#include <shader_structs.h>

export module graphics.techniques.forward_pass;

import graphics.primitive;
import graphics.render_scene;
import graphics.material;
import graphics.techniques.shadow_map_pass;
import renderer.pso;
import renderer.command_queue;
import renderer.dx_renderer;
import renderer.descriptor_heap;
import renderer.gpu_texture;
import renderer.gpu_buffer;
import renderer.gpu_pixel_buffer;
import system.filesystem;
import system.application;
import system.logger;
import system.asserts;

namespace ysn
{
	struct IndirectCommand
	{
		D3D12_GPU_VIRTUAL_ADDRESS camera_parameters_cbv;
		D3D12_GPU_VIRTUAL_ADDRESS scene_parameters_cbv;
		D3D12_GPU_VIRTUAL_ADDRESS per_instance_data_cbv;
		D3D12_DRAW_INDEXED_ARGUMENTS draw_arguments;
	};
}

export namespace ysn
{
	struct ForwardPassRenderParameters
	{
		std::shared_ptr<CommandQueue> command_queue = nullptr;
		std::shared_ptr<CbvSrvUavDescriptorHeap> cbv_srv_uav_heap = nullptr;
		std::shared_ptr<SamplerDescriptorHeap> sampler_heap = nullptr;
		wil::com_ptr<ID3D12Resource> scene_color_buffer = nullptr;
		DescriptorHandle hdr_rtv_descriptor_handle;
		DescriptorHandle dsv_descriptor_handle;
		D3D12_VIEWPORT viewport;
		D3D12_RECT scissors_rect;
		wil::com_ptr<ID3D12Resource> camera_gpu_buffer;
		wil::com_ptr<ID3D12Resource> scene_parameters_gpu_buffer;
		D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_handle;
		wil::com_ptr<ID3D12Resource> current_back_buffer;
		ShadowMapBuffer shadow_map_buffer;

		GpuPixelBuffer3D cubemap_texture;
		GpuPixelBuffer3D irradiance_texture;
		GpuPixelBuffer3D radiance_texture;

		DescriptorHandle brdf_texture_handle;

		// TODO: debug renderer test below
		DescriptorHandle debug_counter_buffer_uav;
		DescriptorHandle debug_vertices_buffer_uav;
	};

	enum class IndirectRootParameters : uint8_t
	{
		CameraParametersSrv,
		SceneParametersSrv,
		PerInstanceDataSrv,

		Count
	};

	struct ForwardPass
	{
		bool Initialize(
			const RenderScene& render_scene,
			wil::com_ptr<ID3D12Resource> camera_gpu_buffer,
			wil::com_ptr<ID3D12Resource> scene_parameters_gpu_buffer,
			const GpuBuffer& instance_buffer,
			wil::com_ptr<ID3D12GraphicsCommandList4> cmd_list);
		bool InitializeIndirectPipeline(
			const RenderScene& render_scene,
			wil::com_ptr<ID3D12Resource> camera_gpu_buffer,
			wil::com_ptr<ID3D12Resource> scene_parameters_gpu_buffer,
			const GpuBuffer& instance_buffer,
			wil::com_ptr<ID3D12GraphicsCommandList4> cmd_list);
		bool CompilePrimitivePso(ysn::Primitive& primitive, std::vector<Material> materials);
		bool Render(const RenderScene& render_scene, const ForwardPassRenderParameters& render_parameters);
		bool RenderIndirect(const RenderScene& render_scene, const ForwardPassRenderParameters& render_parameters);

		// Indirect data
		wil::com_ptr<ID3D12RootSignature> m_indirect_root_signature;
		wil::com_ptr<ID3D12CommandSignature> m_command_signature;
		GpuBuffer m_command_buffer;
		PsoId indirect_pso_id = 0;
		uint32_t m_command_buffer_size = 0; // PrimitiveCount * sizeof(IndirectCommand)
	};
}

module :private;

namespace ysn
{
	bool ForwardPass::CompilePrimitivePso(ysn::Primitive& primitive, std::vector<Material> materials)
	{
		std::shared_ptr<DxRenderer> renderer = Application::Get().GetRenderer();

		GraphicsPsoDesc pso_desc("Primitive PSO");

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

			D3D12_DESCRIPTOR_RANGE materials_input_range = {};
			materials_input_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			materials_input_range.NumDescriptors = 1;
			materials_input_range.BaseShaderRegister = 1;

			D3D12_ROOT_PARAMETER materials_buffer_parameter;
			materials_buffer_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			materials_buffer_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			materials_buffer_parameter.DescriptorTable.NumDescriptorRanges = 1;
			materials_buffer_parameter.DescriptorTable.pDescriptorRanges = &materials_input_range;

			D3D12_DESCRIPTOR_RANGE shadow_input_range = {};
			shadow_input_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			shadow_input_range.NumDescriptors = 1;
			shadow_input_range.BaseShaderRegister = 2;

			D3D12_ROOT_PARAMETER shadow_parameter;
			shadow_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			shadow_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			shadow_parameter.DescriptorTable.NumDescriptorRanges = 1;
			shadow_parameter.DescriptorTable.pDescriptorRanges = &shadow_input_range;

			D3D12_DESCRIPTOR_RANGE cubemap_descriptor_range = {};
			cubemap_descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			cubemap_descriptor_range.NumDescriptors = 1;
			cubemap_descriptor_range.BaseShaderRegister = 3;

			D3D12_ROOT_PARAMETER cubemap_parameter;
			cubemap_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			cubemap_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			cubemap_parameter.DescriptorTable.NumDescriptorRanges = 1;
			cubemap_parameter.DescriptorTable.pDescriptorRanges = &cubemap_descriptor_range;

			D3D12_DESCRIPTOR_RANGE irradiance_descriptor_range = {};
			irradiance_descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			irradiance_descriptor_range.NumDescriptors = 1;
			irradiance_descriptor_range.BaseShaderRegister = 4;

			D3D12_ROOT_PARAMETER irradiance_parameter;
			irradiance_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			irradiance_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			irradiance_parameter.DescriptorTable.NumDescriptorRanges = 1;
			irradiance_parameter.DescriptorTable.pDescriptorRanges = &irradiance_descriptor_range;

			D3D12_DESCRIPTOR_RANGE radiance_descriptor_range = {};
			radiance_descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			radiance_descriptor_range.NumDescriptors = 1;
			radiance_descriptor_range.BaseShaderRegister = 5;

			D3D12_ROOT_PARAMETER radiance_parameter;
			radiance_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			radiance_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			radiance_parameter.DescriptorTable.NumDescriptorRanges = 1;
			radiance_parameter.DescriptorTable.pDescriptorRanges = &radiance_descriptor_range;

			D3D12_DESCRIPTOR_RANGE brdf_descriptor_range = {};
			brdf_descriptor_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			brdf_descriptor_range.NumDescriptors = 1;
			brdf_descriptor_range.BaseShaderRegister = 6;

			D3D12_ROOT_PARAMETER brdf_parameter;
			brdf_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			brdf_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			brdf_parameter.DescriptorTable.NumDescriptorRanges = 1;
			brdf_parameter.DescriptorTable.pDescriptorRanges = &brdf_descriptor_range;

			CD3DX12_DESCRIPTOR_RANGE debug_vertices_uav(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 126, 0, 0);
			D3D12_ROOT_PARAMETER debug_vertices_parameter;
			debug_vertices_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			debug_vertices_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			debug_vertices_parameter.DescriptorTable.NumDescriptorRanges = 1;
			debug_vertices_parameter.DescriptorTable.pDescriptorRanges = &debug_vertices_uav;

			CD3DX12_DESCRIPTOR_RANGE debug_counter_uav(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 127, 0, 0);
			D3D12_ROOT_PARAMETER debug_counter_parameter;
			debug_counter_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			debug_counter_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			debug_counter_parameter.DescriptorTable.NumDescriptorRanges = 1;
			debug_counter_parameter.DescriptorTable.pDescriptorRanges = &debug_counter_uav;

			D3D12_ROOT_PARAMETER rootParams[12] = {
				{D3D12_ROOT_PARAMETER_TYPE_CBV, {0, 0}, D3D12_SHADER_VISIBILITY_ALL}, // CameraParameters
				{D3D12_ROOT_PARAMETER_TYPE_CBV, {1, 0}, D3D12_SHADER_VISIBILITY_ALL}, // SceneParameters

				// InstanceID
				{.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
				 .Constants = {.ShaderRegister = 2, .RegisterSpace = 0, .Num32BitValues = 1},
				 .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL},

				instance_buffer_parameter,  // PerInstanceData
				materials_buffer_parameter, // Materials buffer
				shadow_parameter,           // Shadow Map input
				cubemap_parameter,          // Cubemap input
				irradiance_parameter,       // Irradiance input
				radiance_parameter,         // Radiance input
				brdf_parameter,             // BRDF input
				debug_vertices_parameter, 
				debug_counter_parameter
			};

			// TEMP
			rootParams[0].Descriptor.RegisterSpace = 0;
			rootParams[1].Descriptor.RegisterSpace = 0;

			// 0 ShadowSampler
			// 1 LinearSampler
			CD3DX12_STATIC_SAMPLER_DESC static_sampler[2];
			static_sampler[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			static_sampler[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_ANISOTROPIC);

			D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc = {};
			RootSignatureDesc.NumParameters = 12;
			RootSignatureDesc.pParameters = &rootParams[0];
			RootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED // For bindless rendering
				| D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
			RootSignatureDesc.pStaticSamplers = &static_sampler[0];
			RootSignatureDesc.NumStaticSamplers = 2;

			ID3D12RootSignature* root_signature = nullptr;

			result = renderer->CreateRootSignature(&RootSignatureDesc, &root_signature);

			if (!result)
			{
				LogError << "Can't create root signature for primitive\n";
				return false;
			}

			pso_desc.SetRootSignature(root_signature);
		}

		pso_desc.AddShader({ ShaderType::Vertex, VfsPath(L"shaders/forward_pass.vs.hlsl") });
		pso_desc.AddShader({ ShaderType::Pixel, VfsPath(L"shaders/forward_pass.ps.hlsl") });
		pso_desc.SetDepthStencilState(
			{ .DepthEnable = true, .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL, .DepthFunc = D3D12_COMPARISON_FUNC_LESS });
		pso_desc.SetInputLayout(Vertex::GetVertexLayoutDesc());
		pso_desc.SetSampleMask(UINT_MAX);

		const auto material = materials[primitive.material_id];

		pso_desc.SetRasterizerState(material.rasterizer_desc);
		pso_desc.SetBlendState(material.blend_desc);

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

		pso_desc.SetRenderTargetFormat(renderer->GetHdrFormat(), renderer->GetDepthBufferFormat());

		auto result_pso = renderer->BuildPso(pso_desc);

		if (!result_pso.has_value())
		{
			return false;
		}

		primitive.pso_id = *result_pso;

		return true;
	}

	bool ForwardPass::Render(const RenderScene& render_scene, const ForwardPassRenderParameters& render_parameters)
	{
		auto renderer = Application::Get().GetRenderer();

		const auto cmd_list_res = render_parameters.command_queue->GetCommandList("Forward Pass");

		if (!cmd_list_res.has_value())
			return false;

		auto command_list = cmd_list_res.value();

		ID3D12DescriptorHeap* pDescriptorHeaps[] = {
			render_parameters.cbv_srv_uav_heap->GetHeapPtr(),
			render_parameters.sampler_heap->GetHeapPtr(),
		};
		command_list->SetDescriptorHeaps(_countof(pDescriptorHeaps), pDescriptorHeaps);
		command_list->RSSetViewports(1, &render_parameters.viewport);
		command_list->RSSetScissorRects(1, &render_parameters.scissors_rect);
		command_list->OMSetRenderTargets(1, &render_parameters.hdr_rtv_descriptor_handle.cpu, FALSE, &render_parameters.dsv_descriptor_handle.cpu);

		{
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				render_parameters.shadow_map_buffer.buffer.get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			command_list->ResourceBarrier(1, &barrier);
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
					const std::optional<Pso> pso = renderer->GetPso(primitive.pso_id);

					if (pso.has_value())
					{
						command_list->SetGraphicsRootSignature(pso.value().root_signature.get());
						command_list->SetPipelineState(pso.value().pso.get());

						command_list->IASetPrimitiveTopology(primitive.topology);

						command_list->SetGraphicsRootConstantBufferView(0, render_parameters.camera_gpu_buffer->GetGPUVirtualAddress());
						command_list->SetGraphicsRootConstantBufferView(1, render_parameters.scene_parameters_gpu_buffer->GetGPUVirtualAddress());
						command_list->SetGraphicsRoot32BitConstant(2, instance_id, 0); // InstanceID

						command_list->SetGraphicsRootDescriptorTable(3, render_scene.instance_buffer_srv.gpu);
						command_list->SetGraphicsRootDescriptorTable(4, render_scene.materials_buffer_srv.gpu);
						command_list->SetGraphicsRootDescriptorTable(5, render_parameters.shadow_map_buffer.srv_handle.gpu);

						command_list->SetGraphicsRootDescriptorTable(6, render_parameters.cubemap_texture.srv.gpu);
						command_list->SetGraphicsRootDescriptorTable(7, render_parameters.irradiance_texture.srv.gpu);
						command_list->SetGraphicsRootDescriptorTable(8, render_parameters.radiance_texture.srv.gpu);
						command_list->SetGraphicsRootDescriptorTable(9, render_parameters.brdf_texture_handle.gpu);

						command_list->SetGraphicsRootDescriptorTable(10, render_parameters.debug_vertices_buffer_uav.gpu);
						command_list->SetGraphicsRootDescriptorTable(11, render_parameters.debug_counter_buffer_uav.gpu);

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
						LogInfo << "Can't render primitive because it haven't any PSO\n";
					}

					instance_id++;
				}
			}
		}

		{
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				render_parameters.shadow_map_buffer.buffer.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			command_list->ResourceBarrier(1, &barrier);
		}

		render_parameters.command_queue->CloseCommandList(command_list);

		return true;
	}

	bool ForwardPass::Initialize(
		const RenderScene& render_scene,
		wil::com_ptr<ID3D12Resource> camera_gpu_buffer,
		wil::com_ptr<ID3D12Resource> scene_parameters_gpu_buffer,
		const GpuBuffer& instance_buffer,
		wil::com_ptr<ID3D12GraphicsCommandList4> cmd_list)
	{
		bool result = InitializeIndirectPipeline(render_scene, camera_gpu_buffer, scene_parameters_gpu_buffer, instance_buffer, cmd_list);

		if (!result)
		{
			LogError << "Forward pass can't initialize indirect pipeline\n";
			return false;
		}

		return true;
	}

	bool ForwardPass::InitializeIndirectPipeline(
		const RenderScene& render_scene,
		wil::com_ptr<ID3D12Resource> camera_gpu_buffer,
		wil::com_ptr<ID3D12Resource> scene_parameters_gpu_buffer,
		const GpuBuffer& instance_buffer,
		wil::com_ptr<ID3D12GraphicsCommandList4> cmd_list)
	{
		auto renderer = Application::Get().GetRenderer();

		// Create root signature
		{
			D3D12_ROOT_PARAMETER root_params[(uint8_t)IndirectRootParameters::Count] = {
				{D3D12_ROOT_PARAMETER_TYPE_CBV, {0, 0}, D3D12_SHADER_VISIBILITY_ALL}, // CameraParameters
				{D3D12_ROOT_PARAMETER_TYPE_CBV, {1, 0}, D3D12_SHADER_VISIBILITY_ALL}, // SceneParameters
				{D3D12_ROOT_PARAMETER_TYPE_CBV, {2, 0}, D3D12_SHADER_VISIBILITY_ALL}, // PerInstanceData
			};

			// TEMP
			root_params[0].Descriptor.RegisterSpace = 0;
			root_params[1].Descriptor.RegisterSpace = 0;
			root_params[2].Descriptor.RegisterSpace = 0;

			// 0 ShadowSampler
			// 1 Aniso
			CD3DX12_STATIC_SAMPLER_DESC static_sampler[2];
			static_sampler[0] = CD3DX12_STATIC_SAMPLER_DESC(0, 
															D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
															D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
															D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
															D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			static_sampler[1] = CD3DX12_STATIC_SAMPLER_DESC(1);

			D3D12_ROOT_SIGNATURE_DESC RootSignatureDesc = {};
			RootSignatureDesc.NumParameters = (UINT)IndirectRootParameters::Count;
			RootSignatureDesc.pParameters = &root_params[0];
			RootSignatureDesc.Flags = 
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED  |
				D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

			RootSignatureDesc.pStaticSamplers = &static_sampler[0];
			RootSignatureDesc.NumStaticSamplers = 2;

			bool result = renderer->CreateRootSignature(&RootSignatureDesc, &m_indirect_root_signature);

			if (!result)
			{
				LogError << "Can't create root signature for primitive\n";
				return false;
			}
		}

		// Create command signature
		D3D12_INDIRECT_ARGUMENT_DESC argument_desc[4] = {};

		argument_desc[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
		argument_desc[0].ConstantBufferView.RootParameterIndex = 0;

		argument_desc[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
		argument_desc[1].ConstantBufferView.RootParameterIndex = 1;

		argument_desc[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
		argument_desc[2].ConstantBufferView.RootParameterIndex = 2;

		argument_desc[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

		D3D12_COMMAND_SIGNATURE_DESC command_signature_desc = {};
		command_signature_desc.pArgumentDescs = argument_desc;
		command_signature_desc.NumArgumentDescs = _countof(argument_desc);
		command_signature_desc.ByteStride = sizeof(IndirectCommand);

		if (auto result = renderer->GetDevice()->CreateCommandSignature(
			&command_signature_desc, m_indirect_root_signature.get(), IID_PPV_ARGS(&m_command_signature));
			result != S_OK)
		{
			LogError << "Forward Pass can't create command signature\n";
			return false;
		}

		// Create PSO
		GraphicsPsoDesc pso_desc("Indirect Primitive PSO");
		pso_desc.SetRootSignature(m_indirect_root_signature.get());
		pso_desc.AddShader({ ShaderType::Vertex, VfsPath(L"shaders/indirect_forward_pass.vs.hlsl") });
		pso_desc.AddShader({ ShaderType::Pixel, VfsPath(L"shaders/indirect_forward_pass.ps.hlsl") });
		pso_desc.SetDepthStencilState(
			{ .DepthEnable = true, .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL, .DepthFunc = D3D12_COMPARISON_FUNC_LESS });
		pso_desc.SetInputLayout(Vertex::GetVertexLayoutDesc());
		pso_desc.SetSampleMask(UINT_MAX);

		// TODO: Should use GLTFs parameters
		D3D12_BLEND_DESC blend_desc = {};
		blend_desc.RenderTarget[0].BlendEnable = false;
		blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		D3D12_RASTERIZER_DESC rasterizer_desc = {};
		rasterizer_desc.CullMode = D3D12_CULL_MODE_FRONT;
		rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
		rasterizer_desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		rasterizer_desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		rasterizer_desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		rasterizer_desc.ForcedSampleCount = 0;
		rasterizer_desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		pso_desc.SetRasterizerState(rasterizer_desc);

		pso_desc.SetBlendState(blend_desc);
		pso_desc.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		pso_desc.SetRenderTargetFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);

		auto result_pso = renderer->BuildPso(pso_desc);

		if (!result_pso.has_value())
		{
			LogError << "Can't create indirect pso\n";
			return false;
		}

		indirect_pso_id = *result_pso;

		// Create commands
		//m_indirect_commands.resize(render_scene.primitives_count);

		D3D12_GPU_VIRTUAL_ADDRESS instance_data_buffer_gpu_address = instance_buffer.GPUVirtualAddress();
		UINT command_index = 0;

		std::vector<IndirectCommand> m_indirect_commands;

		// Fill commands
		for (auto& model : render_scene.models)
		{
			for (int mesh_id = 0; mesh_id < model.meshes.size(); mesh_id++)
			{
				const Mesh& mesh = model.meshes[mesh_id];

				for (int primitive_id = 0; primitive_id < mesh.primitives.size(); primitive_id++)
				{
					const Primitive& primitive = mesh.primitives[primitive_id];

					IndirectCommand command;

					command.camera_parameters_cbv = camera_gpu_buffer->GetGPUVirtualAddress();
					command.scene_parameters_cbv = scene_parameters_gpu_buffer->GetGPUVirtualAddress();
					command.per_instance_data_cbv = instance_data_buffer_gpu_address;

					command.draw_arguments.IndexCountPerInstance = primitive.index_count;
					command.draw_arguments.InstanceCount = 1;
					command.draw_arguments.StartIndexLocation = primitive.global_index_offset;
					command.draw_arguments.BaseVertexLocation = primitive.global_vertex_offset;
					command.draw_arguments.StartInstanceLocation = 0;

					m_indirect_commands.push_back(command);

					command_index++;
					instance_data_buffer_gpu_address += sizeof(PerInstanceData);
				}
			}
		}

		m_command_buffer_size = command_index * sizeof(IndirectCommand);

		GpuBufferCreateInfo create_info{ .size = m_command_buffer_size,
			.heap_type = D3D12_HEAP_TYPE_DEFAULT, .state = D3D12_RESOURCE_STATE_COPY_DEST };

		const auto command_buffer_result = CreateGpuBuffer(create_info, "RenderInstance Buffer");

		if (!command_buffer_result.has_value())
		{
			LogError << "Can't create indirect command buffer\n";
			return false;
		}

		m_command_buffer = command_buffer_result.value();

		UploadToGpuBuffer(cmd_list, m_command_buffer, m_indirect_commands.data(), {}, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

		return true;
	}

	bool ForwardPass::RenderIndirect(const RenderScene& render_scene, const ForwardPassRenderParameters& render_parameters)
	{
		auto renderer = Application::Get().GetRenderer();

		const auto cmd_list_res = render_parameters.command_queue->GetCommandList("Indirect Forward Pass");

		if (!cmd_list_res.has_value())
			return false;

		auto command_list = cmd_list_res.value();

		{
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(render_parameters.shadow_map_buffer.buffer.get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			command_list->ResourceBarrier(1, &barrier);
		}

		const std::optional<Pso> pso = renderer->GetPso(indirect_pso_id);

		if (pso.has_value())
		{
			ID3D12DescriptorHeap* pDescriptorHeaps[] = {
				render_parameters.cbv_srv_uav_heap->GetHeapPtr(),
				render_parameters.sampler_heap->GetHeapPtr(),
			};
			command_list->SetDescriptorHeaps(_countof(pDescriptorHeaps), pDescriptorHeaps);
			command_list->RSSetViewports(1, &render_parameters.viewport);
			command_list->RSSetScissorRects(1, &render_parameters.scissors_rect);
			command_list->OMSetRenderTargets(
				1, &render_parameters.hdr_rtv_descriptor_handle.cpu, FALSE, &render_parameters.dsv_descriptor_handle.cpu);

			command_list->SetPipelineState(pso.value().pso.get());
			command_list->SetGraphicsRootSignature(m_indirect_root_signature.get());
			command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			command_list->IASetVertexBuffers(0, 1, &render_scene.vertex_buffer_view);
			command_list->IASetIndexBuffer(&render_scene.index_buffer_view);


			// render_scene.primitives_count
			command_list->ExecuteIndirect(m_command_signature.get(), render_scene.primitives_count, m_command_buffer.Resource(), 0, nullptr, 0);
		}
		else
		{
			LogError << "Can't find indirect buffer PSO\n";
		}

		{
			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(render_parameters.shadow_map_buffer.buffer.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			command_list->ResourceBarrier(1, &barrier);
		}

		render_parameters.command_queue->CloseCommandList(command_list);

		return true;
	}
}
