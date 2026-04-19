#if __cplusplus
// C++ defines
#pragma once
#include <DirectXMath.h>
#include <d3d12.h>

import std;
import system.math;

#define SHADER_ALIGN 16
#define CHECK_STRUCT_ALIGNMENT(struct_name) \
    static_assert(sizeof(struct_name) % SHADER_ALIGN == 0, #struct_name " is not 16 bytes aligned, don't forget add the padding")

#define matrix DirectX::XMMATRIX
#define float4 DirectX::XMFLOAT4
#define float3 DirectX::XMFLOAT3
#define float2 DirectX::XMFLOAT2
#define float16_t std::uint16_t
#define float16_t2 std::uint32_t
#define uint std::uint32_t
#define int std::int32_t
#define float float
#define OUT_PARAMETER(X) X&

template <typename ShaderStruct>
constexpr uint GetGpuSize()
{
    return static_cast<uint>(ysn::AlignPow2(sizeof(ShaderStruct), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
}
// C++ defines end here
#else
// Shader defines
#define CHECK_STRUCT_ALIGNMENT(struct_name)
#endif

#define ALBEDO_ENABLED_BIT 0
#define METALLIC_ROUGHNESS_ENABLED_BIT 1
#define NORMAL_ENABLED_BIT 2
#define OCCLUSION_ENABLED_BIT 3
#define EMISSIVE_ENABLED_BIT 4

#define AMBIENT_LIGHT_SOURCE_COLOR 0
#define AMBIENT_LIGHT_SOURCE_CUBEMAP 1
#define AMBIENT_LIGHT_SOURCE_RADIANCE 2

// Default PBR material
struct SurfaceShaderParameters
{
    float4 base_color_factor;
    float metallic_factor;
    float roughness_factor;

    // Encods which textures are active
    int texture_enable_bitmask;

    int albedo_texture_index;
    int metallic_roughness_texture_index;
    int normal_texture_index;
    int occlusion_texture_index;
    int emissive_texture_index;
};
CHECK_STRUCT_ALIGNMENT(SurfaceShaderParameters);

// Per instance data for rendering, this can be split into smaller parts
struct PerInstanceData
{
    matrix model_matrix;
    int material_id;
    int vertices_before;
    int indices_before; // offset
    int pad;
};
CHECK_STRUCT_ALIGNMENT(PerInstanceData);

struct InstanceID
{
    uint id;
};
//CHECK_STRUCT_ALIGNMENT(InstanceID);

struct GpuSceneParameters
{
    matrix shadow_matrix;
    float4 directional_light_color;
    float4 directional_light_direction;
    float directional_light_intensity;
    float ambient_light_intensity;
    uint shadows_enabled;
    uint ambient_light_mode;
    float4 ambient_light_color;
    int ambient_cubemap_texture_index;
    int ambient_radiance_texture_index;

    uint pad[2];
};
CHECK_STRUCT_ALIGNMENT(GpuSceneParameters);

struct ShadowCamera
{
    matrix view_projection;
};
CHECK_STRUCT_ALIGNMENT(ShadowCamera);

struct TonemapParameters
{
    uint display_width;
    uint display_height;
    uint tonemap_method;
    float exposure;
};
CHECK_STRUCT_ALIGNMENT(TonemapParameters);

#define VolumetricFogDispatchX 8
#define VolumetricFogDispatchY 4
#define VolumetricFogDispatchZ 1

struct VolumetricFogParameters
{
    uint display_width;
    uint display_height;
    float num_steps;
    float pad;
};
CHECK_STRUCT_ALIGNMENT(TonemapParameters);

struct GenerateMipsConstantBuffer
{
    uint src_mip_level;     // Texture level of source mip
    uint num_mip_levels;    // Number of OutMips to write: [1-4]
    uint src_dimension;     // Width and height of the source texture are even or odd.
    uint is_srgb;           // Must apply gamma correction to sRGB textures.
    float2 texel_size;      // 1.0 / OutMip1.Dimensions

    uint pad[2];
};
CHECK_STRUCT_ALIGNMENT(GenerateMipsConstantBuffer);

struct CameraParameters
{
    matrix view_projection;
    matrix view;
    matrix previous_view;
    matrix projection;
    matrix view_inverse;
    matrix projection_inverse;
    float3 position;
    uint frame_number;
    uint rtx_frames_accumulated;
    uint reset_accumulation;
    uint accumulation_enabled;
    uint pad;
};
CHECK_STRUCT_ALIGNMENT(CameraParameters);

struct RtxHitInfo
{
    float4 encoded_normals; // Octahedron encoded
    float3 hit_position;
    int material_id;
    float2 uvs;

    bool has_hit()
    {
        return material_id != -1;
    }
};
//CHECK_STRUCT_ALIGNMENT(RtxHitInfo); // Not required, this is payload for DXR
 
struct RtxAttributes
{
    float2 bary;
};
// CHECK_STRUCT_ALIGNMENT(RtxAttributes); // Not required, this is attributes for DXR

#if __cplusplus
#undef matrix 
#undef float4 
#undef float3 
#undef float2 
#undef float16_t 
#undef float16_t2 
#undef uint 
#undef int 
#undef float 
#undef OUT_PARAMETER
#endif

