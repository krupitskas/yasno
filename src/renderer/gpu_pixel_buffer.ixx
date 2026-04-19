module;

#include <wil/com.h>
#include <d3d12.h>
#include <d3dx12.h>

export module renderer.gpu_pixel_buffer;

import renderer.gpu_buffer;
import renderer.descriptor_heap;
import system.application;

export namespace ysn
{
	class GpuPixelBuffer2D : public GpuBuffer
	{

	};

	class GpuPixelBuffer3D : public GpuBuffer
	{
	public:
		std::vector<std::array<DescriptorHandle, 6>> rtv; // mip -> [6 faces]
		DescriptorHandle srv;

		void GenerateRTVs()
		{
			auto renderer = Application::Get().GetRenderer();
			const auto resource_desc = m_resource->GetDesc();

			for (int mip = 0; mip < resource_desc.MipLevels; mip++)
			{
				std::array<DescriptorHandle, 6> faces;

				for (int face = 0; face < 6; face++)
				{
					D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
					rtv_desc.Format = resource_desc.Format;
					rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
					rtv_desc.Texture2DArray.MipSlice = mip;
					rtv_desc.Texture2DArray.FirstArraySlice = face;
					rtv_desc.Texture2DArray.ArraySize = 1;

					faces[face] = renderer->GetRtvDescriptorHeap()->GetNewHandle();
					renderer->GetDevice()->CreateRenderTargetView(m_resource.get(), &rtv_desc, faces[face].cpu);
				}

				rtv.push_back(faces);
			}

			{
				D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
				srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv_desc.Format = resource_desc.Format;
				srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
				srv_desc.TextureCube.MostDetailedMip = 0;
				srv_desc.TextureCube.MipLevels = resource_desc.MipLevels;
				srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;

				srv = renderer->GetCbvSrvUavDescriptorHeap()->GetNewHandle();
				renderer->GetDevice()->CreateShaderResourceView(m_resource.get(), &srv_desc, srv.cpu);
			}
		}
	};
}
