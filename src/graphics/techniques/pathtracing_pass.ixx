module;
// https://developer.nvidia.com/rtx/raytracing/dxr/DX12-Raytracing-tutorial-Part-2

#include <wil/com.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <dxcapi.h>
#include <DirectXMath.h>

#include <shader_structs.h>

export module graphics.techniques.raytracing_pass;

import std;
import yasno.camera;
import renderer.dx_renderer;
import renderer.rtx_context;
import renderer.descriptor_heap;
import renderer.shader_storage;
import renderer.gpu_buffer;
import renderer.vertex_storage;
import system.filesystem;
import system.string_helpers;
import system.logger;
import system.asserts;

class ShaderRecord
{
public:
	ShaderRecord(void* pShaderIdentifier, UINT shaderIdentifierSize) : shaderIdentifier(pShaderIdentifier, shaderIdentifierSize)
	{
	}

	ShaderRecord(void* pShaderIdentifier, UINT shaderIdentifierSize, void* pLocalRootArguments, UINT localRootArgumentsSize) :
		shaderIdentifier(pShaderIdentifier, shaderIdentifierSize), localRootArguments(pLocalRootArguments, localRootArgumentsSize)
	{
	}

	void CopyTo(void* dest) const
	{
		uint8_t* byteDest = static_cast<uint8_t*>(dest);
		memcpy(byteDest, shaderIdentifier.ptr, shaderIdentifier.size);
		if (localRootArguments.ptr)
		{
			memcpy(byteDest + shaderIdentifier.size, localRootArguments.ptr, localRootArguments.size);
		}
	}

	struct PointerWithSize
	{
		void* ptr;
		UINT size;

		PointerWithSize() : ptr(nullptr), size(0)
		{
		}
		PointerWithSize(void* _ptr, UINT _size) : ptr(_ptr), size(_size)
		{
		};
	};
	PointerWithSize shaderIdentifier;
	PointerWithSize localRootArguments;
};

// Shader table = {{ ShaderRecord 1}, {ShaderRecord 2}, ...}
//  : public GpuUploadBuffer
class ShaderTable
{
	uint8_t* m_mapped_shader_record;
	UINT m_shader_record_size = 0;

	// Debug support
	std::string m_name;
	std::vector<ShaderRecord> m_shaderRecords;

	ShaderTable()
	{
	}

public:
	ysn::GpuBuffer buffer;

	ShaderTable(UINT num_shader_records, UINT shader_record_size, std::string name) : m_name(name)
	{
		m_shader_record_size = ysn::AlignUp(shader_record_size, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
		m_shaderRecords.reserve(num_shader_records);
		UINT buffer_size = num_shader_records * m_shader_record_size;

		ysn::GpuBufferCreateInfo sbt_storage_create_info{
			.size = buffer_size,
			.heap_type = D3D12_HEAP_TYPE_UPLOAD,
			.flags = D3D12_RESOURCE_FLAG_NONE,
			.state = D3D12_RESOURCE_STATE_GENERIC_READ,
		};

		buffer = CreateGpuBuffer(sbt_storage_create_info, name).value();

		// We don't unmap this until the app closes. Keeping buffer mapped for the lifetime of the resource is okay.
		CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
		HRESULT res = buffer.Resource()->Map(0, &readRange, reinterpret_cast<void**>(&m_mapped_shader_record));

		ysn::AssertMsg(res == S_OK, "Can't map ShaderTable resource!");
	}

	void PushBack(const ShaderRecord& shaderRecord)
	{
		ysn::AssertMsg(m_shaderRecords.size() < m_shaderRecords.capacity(), "Shader records size less then capacity");
		m_shaderRecords.push_back(shaderRecord);
		shaderRecord.CopyTo(m_mapped_shader_record);
		m_mapped_shader_record += m_shader_record_size;
	}

	UINT GetShaderRecordSize()
	{
		return m_shader_record_size;
	}

	// Pretty-print the shader records.
	void DebugPrint(std::unordered_map<void*, std::wstring> shaderIdToStringMap)
	{
		std::wstringstream wstr;
		wstr << L"|--------------------------------------------------------------------\n";
		wstr << L"|Shader table - " << m_name.c_str() << L": " << m_shader_record_size << L" | "
			<< m_shaderRecords.size() * m_shader_record_size << L" bytes\n";

		for (UINT i = 0; i < m_shaderRecords.size(); i++)
		{
			wstr << L"| [" << i << L"]: ";
			wstr << shaderIdToStringMap[m_shaderRecords[i].shaderIdentifier.ptr] << L", ";
			wstr << m_shaderRecords[i].shaderIdentifier.size << L" + " << m_shaderRecords[i].localRootArguments.size << L" bytes \n";
		}
		wstr << L"|--------------------------------------------------------------------\n";
		wstr << L"\n";

		ysn::LogInfo << ysn::WStringToString(wstr.str());
	}
};

void PrintDxrStateObjectDesc(const D3D12_STATE_OBJECT_DESC* desc)
{
	std::wstringstream wstr;

	wstr << L"\n";
	wstr << L"--------------------------------------------------------------------\n";
	wstr << L"| D3D12 State Object 0x" << static_cast<const void*>(desc) << L": ";
	if (desc->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION)
		wstr << L"Collection\n";
	if (desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)
		wstr << L"Raytracing Pipeline\n";

	auto ExportTree = [](UINT depth, UINT numExports, const D3D12_EXPORT_DESC* exports)
	{
		std::wostringstream woss;
		for (UINT i = 0; i < numExports; i++)
		{
			woss << L"|";
			if (depth > 0)
			{
				for (UINT j = 0; j < 2 * depth - 1; j++)
					woss << L" ";
			}
			woss << L" [" << i << L"]: ";
			if (exports[i].ExportToRename)
				woss << exports[i].ExportToRename << L" --> ";
			woss << exports[i].Name << L"\n";
		}
		return woss.str();
	};

	for (UINT i = 0; i < desc->NumSubobjects; i++)
	{
		wstr << L"| [" << i << L"]: ";
		switch (desc->pSubobjects[i].Type)
		{
			case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
				wstr << L"Global Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
				break;
			case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
				wstr << L"Local Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
				break;
			case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
				wstr << L"Node Mask: 0x" << std::hex << std::setfill(L'0') << std::setw(8)
					<< *static_cast<const UINT*>(desc->pSubobjects[i].pDesc) << std::setw(0) << std::dec << L"\n";
				break;
			case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
			{
				wstr << L"DXIL Library 0x";
				auto lib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(desc->pSubobjects[i].pDesc);
				wstr << lib->DXILLibrary.pShaderBytecode << L", " << lib->DXILLibrary.BytecodeLength << L" bytes\n";
				wstr << ExportTree(1, lib->NumExports, lib->pExports);
				break;
			}
			case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
			{
				wstr << L"Existing Library 0x";
				auto collection = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(desc->pSubobjects[i].pDesc);
				wstr << collection->pExistingCollection << L"\n";
				wstr << ExportTree(1, collection->NumExports, collection->pExports);
				break;
			}
			case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
			{
				wstr << L"Subobject to Exports Association (Subobject [";
				auto association = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
				UINT index = static_cast<UINT>(association->pSubobjectToAssociate - desc->pSubobjects);
				wstr << index << L"])\n";
				for (UINT j = 0; j < association->NumExports; j++)
				{
					wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
				}
				break;
			}
			case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
			{
				wstr << L"DXIL Subobjects to Exports Association (";
				auto association = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
				wstr << association->SubobjectToAssociate << L")\n";
				for (UINT j = 0; j < association->NumExports; j++)
				{
					wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
				}
				break;
			}
			case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
			{
				wstr << L"Raytracing Shader Config\n";
				auto config = static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(desc->pSubobjects[i].pDesc);
				wstr << L"|  [0]: Max Payload Size: " << config->MaxPayloadSizeInBytes << L" bytes\n";
				wstr << L"|  [1]: Max Attribute Size: " << config->MaxAttributeSizeInBytes << L" bytes\n";
				break;
			}
			case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
			{
				wstr << L"Raytracing Pipeline Config\n";
				auto config = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(desc->pSubobjects[i].pDesc);
				wstr << L"|  [0]: Max Recursion Depth: " << config->MaxTraceRecursionDepth << L"\n";
				break;
			}
			case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
			{
				wstr << L"Hit Group (";
				auto hitGroup = static_cast<const D3D12_HIT_GROUP_DESC*>(desc->pSubobjects[i].pDesc);
				wstr << (hitGroup->HitGroupExport ? hitGroup->HitGroupExport : L"[none]") << L")\n";
				wstr << L"|  [0]: Any Hit Import: " << (hitGroup->AnyHitShaderImport ? hitGroup->AnyHitShaderImport : L"[none]") << L"\n";
				wstr << L"|  [1]: Closest Hit Import: "
					<< (hitGroup->ClosestHitShaderImport ? hitGroup->ClosestHitShaderImport : L"[none]") << L"\n";
				wstr << L"|  [2]: Intersection Import: "
					<< (hitGroup->IntersectionShaderImport ? hitGroup->IntersectionShaderImport : L"[none]") << L"\n";
				break;
			}
		}
		wstr << L"|--------------------------------------------------------------------\n";
	}
	wstr << L"\n";

	ysn::LogInfo << ysn::WStringToString(wstr.str());
}

export namespace ysn
{
	class PathtracingPass
	{
	public:
		bool Initialize(
			std::shared_ptr<ysn::DxRenderer> renderer,
			wil::com_ptr<ID3D12Resource> scene_color,
			wil::com_ptr<ID3D12Resource> accumulation_buffer_color,
			RtxContext& rtx_context,
			wil::com_ptr<ID3D12Resource> camera_buffer,
			wil::com_ptr<ID3D12Resource> vertex_buffer,
			wil::com_ptr<ID3D12Resource> index_buffer,
			wil::com_ptr<ID3D12Resource> material_buffer,
			wil::com_ptr<ID3D12Resource> per_instance_buffer,
			uint32_t vertices_count,
			uint32_t indices_count,
			uint32_t materials_count,
			uint32_t primitives_count);

		bool CreateRaytracingPipeline(std::shared_ptr<ysn::DxRenderer> renderer);
		bool CreateShaderBindingTable();
		bool CreateRootSignatures(std::shared_ptr<ysn::DxRenderer> renderer);

		void Execute(
			std::shared_ptr<ysn::DxRenderer> renderer,
			RtxContext& rtx_context,
			const DescriptorHandle& output_texture_srv,
			const DescriptorHandle& accumulation_texture_srv,
			wil::com_ptr<ID3D12GraphicsCommandList4> command_list,
			uint32_t width,
			uint32_t height,
			wil::com_ptr<ID3D12Resource> scene_color,
			wil::com_ptr<ID3D12Resource> camera_buffer,
			wil::com_ptr<ID3D12Resource> scene_parameters_buffer);

	private:
		std::wstring m_raygen_shader_name = L"RayGen";
		std::wstring m_miss_shader_name = L"Miss";
		std::wstring m_closest_hit_shader_name = L"ClosestHit";
		std::wstring m_hit_group_name = L"HitGroup";

		wil::com_ptr<IDxcBlob> m_pathtracing_shader_blob;

		wil::com_ptr<ID3D12RootSignature> m_global_rs;
		wil::com_ptr<ID3D12RootSignature> m_local_rs;

		GpuBuffer m_miss_shader_table;
		GpuBuffer m_hit_group_shader_table;
		GpuBuffer m_ray_gen_shader_table;

		wil::com_ptr<ID3D12StateObject> m_rt_state_object;
		wil::com_ptr<ID3D12StateObjectProperties> m_rt_state_object_props;

		// Input data
		DescriptorHandle m_vertex_buffer_srv;
		DescriptorHandle m_indices_buffer_srv;
		DescriptorHandle m_materials_buffer_srv;
		DescriptorHandle m_per_instance_data_buffer_srv;
	};
}

module :private;

namespace ysn
{
	bool PathtracingPass::Initialize(
		std::shared_ptr<ysn::DxRenderer> renderer,
		wil::com_ptr<ID3D12Resource> scene_color,
		wil::com_ptr<ID3D12Resource> accumulation_buffer_color,
		RtxContext& rtx_context,
		wil::com_ptr<ID3D12Resource> camera_buffer,
		wil::com_ptr<ID3D12Resource> vertex_buffer,
		wil::com_ptr<ID3D12Resource> index_buffer,
		wil::com_ptr<ID3D12Resource> material_buffer,
		wil::com_ptr<ID3D12Resource> per_instance_buffer,
		uint32_t vertices_count,
		uint32_t indices_count,
		uint32_t materials_count,
		uint32_t primitives_count)
	{
		if (!CreateRootSignatures(renderer))
		{
			LogError << "Can't create raytracing root signatures\n";
			return false;
		}

		if (!CreateRaytracingPipeline(renderer))
		{
			LogError << "Can't create raytracing pass pipeline\n";
			return false;
		}

		if (!CreateShaderBindingTable())
		{
			LogError << "Can't create raytracing pass shader binding table\n";
			return false;
		}

		rtx_context.CreateTlasSrv(renderer);

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv_desc.Buffer.FirstElement = 0;
			srv_desc.Buffer.NumElements = static_cast<UINT>(vertices_count);
			srv_desc.Buffer.StructureByteStride = sizeof(Vertex);
			srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

			m_vertex_buffer_srv = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
			renderer->GetDevice()->CreateShaderResourceView(vertex_buffer.get(), &srv_desc, m_vertex_buffer_srv.cpu);
		}

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv_desc.Buffer.FirstElement = 0;
			srv_desc.Buffer.NumElements = static_cast<UINT>(indices_count);
			srv_desc.Buffer.StructureByteStride = sizeof(uint32_t);
			srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

			m_indices_buffer_srv = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
			renderer->GetDevice()->CreateShaderResourceView(index_buffer.get(), &srv_desc, m_indices_buffer_srv.cpu);
		}

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv_desc.Buffer.FirstElement = 0;
			srv_desc.Buffer.NumElements = static_cast<UINT>(materials_count);
			srv_desc.Buffer.StructureByteStride = sizeof(SurfaceShaderParameters);
			srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

			m_materials_buffer_srv = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
			renderer->GetDevice()->CreateShaderResourceView(material_buffer.get(), &srv_desc, m_materials_buffer_srv.cpu);
		}

		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv_desc.Buffer.FirstElement = 0;
			srv_desc.Buffer.NumElements = static_cast<UINT>(primitives_count);
			srv_desc.Buffer.StructureByteStride = sizeof(PerInstanceData);
			srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

			m_per_instance_data_buffer_srv = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
			renderer->GetDevice()->CreateShaderResourceView(per_instance_buffer.get(), &srv_desc, m_per_instance_data_buffer_srv.cpu);
		}

		return true;
	}

	bool PathtracingPass::CreateRaytracingPipeline(std::shared_ptr<ysn::DxRenderer> renderer)
	{
		// Load shaders
		{
			ShaderCompileParameters pathtracing_parameters(ShaderType::Library, VfsPath(L"shaders/pathtracing.rt.hlsl"));
			const auto compiled_shader_hash = renderer->GetShaderStorage()->CompileShader(pathtracing_parameters);

			if (!compiled_shader_hash.has_value())
			{
				LogError << "Can't compile pathtracing shader\n";
				return false;
			}

			m_pathtracing_shader_blob = renderer->GetShaderStorage()->GetShader(*compiled_shader_hash).value();
		}

		CD3DX12_STATE_OBJECT_DESC rtx_pipeline{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

		// Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
		CD3DX12_DXIL_LIBRARY_SUBOBJECT* dxil_lib_subobjects = rtx_pipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();

		D3D12_SHADER_BYTECODE libdxil =
			CD3DX12_SHADER_BYTECODE(m_pathtracing_shader_blob->GetBufferPointer(), m_pathtracing_shader_blob->GetBufferSize());
		dxil_lib_subobjects->SetDXILLibrary(&libdxil);

		{
			dxil_lib_subobjects->DefineExport(L"RayGen");
			dxil_lib_subobjects->DefineExport(L"ClosestHit");
			dxil_lib_subobjects->DefineExport(L"Miss");
		}

		CD3DX12_HIT_GROUP_SUBOBJECT* hit_group = rtx_pipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
		hit_group->SetClosestHitShaderImport(L"ClosestHit");
		hit_group->SetHitGroupExport(L"HitGroup");
		hit_group->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

		CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT* shader_cfg =
			rtx_pipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
		const UINT payload_size = sizeof(RtxHitInfo);
		const UINT attribute_size = sizeof(RtxAttributes);
		shader_cfg->Config(payload_size, attribute_size);

		// Ray gen and miss shaders in this sample are not using a local root signature and thus one is not associated with them.
		// Local root signature to be used in a hit group.
		auto local_root_signature = rtx_pipeline.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();

		local_root_signature->SetRootSignature(m_local_rs.get());

		// Define explicit shader association for the local root signature.
		{
			auto rs_association = rtx_pipeline.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
			rs_association->SetSubobjectToAssociate(*local_root_signature);
			rs_association->AddExport(L"HitGroup");
		}

		// This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
		auto global_root_signature = rtx_pipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
		global_root_signature->SetRootSignature(m_global_rs.get());

		// Pipeline config
		auto pipeline_config = rtx_pipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
		UINT max_recursion_depth = 1; // ~ primary rays only.
		pipeline_config->Config(max_recursion_depth);

		//PrintDxrStateObjectDesc(rtx_pipeline);

		HRESULT hr = renderer->GetDevice()->CreateStateObject(rtx_pipeline, IID_PPV_ARGS(&m_rt_state_object));

		if (FAILED(hr))
		{
			LogError << "Could not create the raytracing state object\n";
			return false;
		}

		if (auto result = m_rt_state_object->QueryInterface(IID_PPV_ARGS(&m_rt_state_object_props)); FAILED(result))
		{
			LogError << "Can't QueryInterface rt_state_object_props from CreateRaytracingPipeline\n";
			return false;
		}

		return true;
	}

	bool PathtracingPass::CreateRootSignatures(std::shared_ptr<ysn::DxRenderer> renderer)
	{
		// Global root signature
		{
			CD3DX12_DESCRIPTOR_RANGE output_texture_uav(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
			CD3DX12_DESCRIPTOR_RANGE accumulation_texture_uav(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);

			CD3DX12_DESCRIPTOR_RANGE scene_bvh_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);         // t0
			CD3DX12_DESCRIPTOR_RANGE vertex_buffer_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);     // t1
			CD3DX12_DESCRIPTOR_RANGE index_buffer_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);      // t2
			CD3DX12_DESCRIPTOR_RANGE material_buffer_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);   // t3
			CD3DX12_DESCRIPTOR_RANGE per_instance_data_srv(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4); // t4

			// note(perf): order from most frequent to least frequent
			std::array<CD3DX12_ROOT_PARAMETER, 9> root_parameters;
			root_parameters[0].InitAsDescriptorTable(1, &output_texture_uav);       // u0
			root_parameters[1].InitAsDescriptorTable(1, &accumulation_texture_uav); // u1

			root_parameters[2].InitAsConstantBufferView(0); // b0 camera
			root_parameters[3].InitAsConstantBufferView(1); // b1 scene parameters

			root_parameters[4].InitAsDescriptorTable(1, &scene_bvh_srv);         // t0
			root_parameters[5].InitAsDescriptorTable(1, &vertex_buffer_srv);     // t1
			root_parameters[6].InitAsDescriptorTable(1, &index_buffer_srv);      // t2
			root_parameters[7].InitAsDescriptorTable(1, &material_buffer_srv);   // t3
			root_parameters[8].InitAsDescriptorTable(1, &per_instance_data_srv); // t4

			CD3DX12_ROOT_SIGNATURE_DESC global_root_sig_desc((UINT)root_parameters.size(), root_parameters.data());

			D3D12_STATIC_SAMPLER_DESC static_sampler = {};
			static_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			static_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			static_sampler.MipLODBias = 0.f;
			static_sampler.MaxAnisotropy = 1;
			static_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			static_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
			static_sampler.MinLOD = 0.f;
			static_sampler.MaxLOD = D3D12_FLOAT32_MAX;
			static_sampler.ShaderRegister = 0;
			static_sampler.RegisterSpace = 0;
			static_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			global_root_sig_desc.NumStaticSamplers = 1;
			global_root_sig_desc.pStaticSamplers = &static_sampler;
			global_root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

			ID3DBlob* root_signature_blob = nullptr;
			ID3DBlob* error_blob = nullptr;

			HRESULT hr = D3D12SerializeRootSignature(&global_root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_signature_blob, &error_blob);

			if (FAILED(hr))
			{
				const auto error = static_cast<char*>(error_blob->GetBufferPointer());
				ysn::LogError << "Can't serialize root signature: " << error << "\n";
				return false;
			}

			hr = renderer->GetDevice()->CreateRootSignature(
				0, root_signature_blob->GetBufferPointer(), root_signature_blob->GetBufferSize(), IID_PPV_ARGS(&m_global_rs));

			if (FAILED(hr))
			{
				ysn::LogError << "Can't create root signature: " << "\n";
				return false;
			}
		}

		// Local root signature
		{
			std::array<CD3DX12_ROOT_PARAMETER, 0> root_parameters;
			// rootParameters[GlobalRootSignatureParams::OutputViewSlot].InitAsDescriptorTable(1, &ranges[0]);

			CD3DX12_ROOT_SIGNATURE_DESC local_rs_desc((UINT)root_parameters.size(), root_parameters.data());
			local_rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

			ID3DBlob* root_signature_blob = nullptr;
			ID3DBlob* error_blob = nullptr;

			HRESULT hr = D3D12SerializeRootSignature(&local_rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_signature_blob, &error_blob);

			if (FAILED(hr))
			{
				const auto error = static_cast<char*>(error_blob->GetBufferPointer());
				ysn::LogError << "Can't serialize root signature: " << error << "\n";
				return false;
			}

			hr = renderer->GetDevice()->CreateRootSignature(
				0, root_signature_blob->GetBufferPointer(), root_signature_blob->GetBufferSize(), IID_PPV_ARGS(&m_local_rs));

			if (FAILED(hr))
			{
				ysn::LogError << "Can't create root signature: " << "\n";
				return false;
			}
		}

		return true;
	}

	bool PathtracingPass::CreateShaderBindingTable()
	{
		void* raygen_shader_id;
		void* miss_shader_id;
		void* hit_group_shader_id;

		auto GetShaderIdentifiers = [&](auto* state_objects_properties)
		{
			raygen_shader_id = state_objects_properties->GetShaderIdentifier(m_raygen_shader_name.c_str());
			miss_shader_id = state_objects_properties->GetShaderIdentifier(m_miss_shader_name.c_str());
			hit_group_shader_id = state_objects_properties->GetShaderIdentifier(m_hit_group_name.c_str());
		};

		// Get shader identifiers.
		UINT shader_id_size;
		{
			GetShaderIdentifiers(m_rt_state_object_props.get());
			shader_id_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		}

		// Ray gen shader table
		{
			UINT num_shader_records = 1;
			UINT shader_record_size = shader_id_size;
			ShaderTable ray_gen_shader_table(num_shader_records, shader_record_size, "RayGenShaderTable");
			ray_gen_shader_table.PushBack(ShaderRecord(raygen_shader_id, shader_id_size));
			m_ray_gen_shader_table = ray_gen_shader_table.buffer;
		}

		// Miss shader table
		{
			UINT num_shader_records = 1;
			UINT shader_record_size = shader_id_size;
			ShaderTable miss_shader_table(num_shader_records, shader_record_size, "MissShaderTable");
			miss_shader_table.PushBack(ShaderRecord(miss_shader_id, shader_id_size));
			m_miss_shader_table = miss_shader_table.buffer;
		}

		// Hit group shader table
		//{
		//    struct RootArguments
		//    {
		//        CubeConstantBuffer cb;
		//    } rootArguments;
		//    rootArguments.cb = m_cubeCB;

		//    UINT numShaderRecords = 1;
		//    UINT shaderRecordSize = shaderIdentifierSize + sizeof(rootArguments);
		//    ShaderTable hitGroupShaderTable(device, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");
		//    hitGroupShaderTable.push_back(ShaderRecord(hitGroupShaderIdentifier, shaderIdentifierSize, &rootArguments,
		//    sizeof(rootArguments))); m_hitGroupShaderTable = hitGroupShaderTable.GetResource();
		//}

		{
			UINT num_shader_records = 1;
			UINT shader_record_size = shader_id_size;
			ShaderTable hit_group_shader_table(num_shader_records, shader_record_size, "HitGroupShaderTable");
			hit_group_shader_table.PushBack(ShaderRecord(hit_group_shader_id, shader_id_size));
			m_hit_group_shader_table = hit_group_shader_table.buffer;
		}

		return true;
	}

	void PathtracingPass::Execute(
		std::shared_ptr<ysn::DxRenderer> renderer,
		RtxContext& rtx_context,

		const DescriptorHandle& output_texture_srv,
		const DescriptorHandle& accumulation_texture_srv,

		wil::com_ptr<ID3D12GraphicsCommandList4> command_list,
		uint32_t width,
		uint32_t height,
		wil::com_ptr<ID3D12Resource> scene_color,
		wil::com_ptr<ID3D12Resource> camera_buffer,
		wil::com_ptr<ID3D12Resource> scene_parameters_buffer)
	{
		ID3D12DescriptorHeap* ppHeaps[] = {
			renderer->GetCbvSrvUavDescriptorHeap()->GetHeapPtr(),
			renderer->GetSamplerDescriptorHeap()->GetHeapPtr(),
		};
		command_list->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

		CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(
			scene_color.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		command_list->ResourceBarrier(1, &transition);

		// Input resources
		command_list->SetComputeRootSignature(m_global_rs.get());
		command_list->SetComputeRootDescriptorTable(0, output_texture_srv.gpu);
		command_list->SetComputeRootDescriptorTable(1, accumulation_texture_srv.gpu);

		command_list->SetComputeRootConstantBufferView(2, camera_buffer->GetGPUVirtualAddress());
		command_list->SetComputeRootConstantBufferView(3, scene_parameters_buffer->GetGPUVirtualAddress());

		command_list->SetComputeRootDescriptorTable(4, rtx_context.tlas_buffers.tlas_srv.gpu);

		command_list->SetComputeRootDescriptorTable(5, m_vertex_buffer_srv.gpu);
		command_list->SetComputeRootDescriptorTable(6, m_indices_buffer_srv.gpu);
		command_list->SetComputeRootDescriptorTable(7, m_materials_buffer_srv.gpu);
		command_list->SetComputeRootDescriptorTable(8, m_per_instance_data_buffer_srv.gpu);

		D3D12_DISPATCH_RAYS_DESC desc = {};
		desc.HitGroupTable.StartAddress = m_hit_group_shader_table.GPUVirtualAddress();
		desc.HitGroupTable.SizeInBytes = m_hit_group_shader_table.Desc().Width;
		desc.HitGroupTable.StrideInBytes = desc.HitGroupTable.SizeInBytes;

		desc.MissShaderTable.StartAddress = m_miss_shader_table.GPUVirtualAddress();
		desc.MissShaderTable.SizeInBytes = m_miss_shader_table.Desc().Width;
		desc.MissShaderTable.StrideInBytes = desc.MissShaderTable.SizeInBytes;

		desc.RayGenerationShaderRecord.StartAddress = m_ray_gen_shader_table.GPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes = m_ray_gen_shader_table.Desc().Width;

		desc.Width = width;
		desc.Height = height;
		desc.Depth = 1;

		command_list->SetPipelineState1(m_rt_state_object.get());

		command_list->DispatchRays(&desc);

		transition = CD3DX12_RESOURCE_BARRIER::Transition(
			scene_color.get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_RENDER_TARGET); // TODO(rtx): This transition is done in Tonemap, so it's duplicate

		command_list->ResourceBarrier(1, &transition);
	}
}
