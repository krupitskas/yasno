#include "shader_structs.h"
#include "shared.hlsl"
#include "debug_renderer.hlsl"

struct RS2PS
{
	float4 position_shadow_space : POSITION_1;

	float4 position : SV_POSITION;
	float4 world_position : POSITION_0;
	float3 normal : NORMAL;
	float2 texcoord_0: TEXCOORD_0;
};

// Buffers
ConstantBuffer<CameraParameters> camera						: register(b0);
ConstantBuffer<GpuSceneParameters> scene_parameters			: register(b1);
ConstantBuffer<InstanceID> instance_id						: register(b2);

StructuredBuffer<PerInstanceData> per_instance_data			: register(t0);
StructuredBuffer<SurfaceShaderParameters> materials_buffer	: register(t1);

// Textures
Texture2D   g_shadow_map			: register(t2);
TextureCube g_input_cubemap			: register(t3);
TextureCube g_input_irradiance		: register(t4);
TextureCube g_input_radiance		: register(t5);
Texture2D	g_input_brdf			: register(t6);

// Samplers
SamplerState g_shadow_sampler		: register(s0);
SamplerState g_linear_sampler		: register(s1);

float PCFShadowCalculation(float4 position)
{
	float shadow_value = 0.0;

	// TODO: dynamic shadowmap size, now it's 4096
	float2 texel_size = 1.0f / 4096.f;

	float3 projected_coordinate;
	projected_coordinate.x = position.x / position.w * 0.5 + 0.5;
	projected_coordinate.y = -position.y / position.w * 0.5 + 0.5;
	projected_coordinate.z = position.z / position.w;

	float current_depth = projected_coordinate.z + 0.0015;

	int pcf_size = 1;

	for(int x = -pcf_size; x <= pcf_size; ++x)
	{
		for(int y = -pcf_size; y <= pcf_size; ++y)
		{
			float closest_depth = g_shadow_map.Sample(g_shadow_sampler, projected_coordinate.xy + float2(x, y) * texel_size).r;

			shadow_value += current_depth + 0.001 > closest_depth ? 1.0 : 0.0;        
		}    
	}

	shadow_value /= (2 * pcf_size + 1) * (2 * pcf_size + 1);

	return shadow_value;
}

float ShadowCalculation(float4 position)
{
	float3 projected_coordinate;
	projected_coordinate.x = position.x / position.w * 0.5 + 0.5;
	projected_coordinate.y = -position.y / position.w * 0.5 + 0.5;
	projected_coordinate.z = position.z / position.w;

	float closest_depth = g_shadow_map.Sample(g_shadow_sampler, projected_coordinate.xy);

	float in_shadow = projected_coordinate.z - 0.0025f > closest_depth ? 1.0 : 0.0;  

	return in_shadow;
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}

float3 fresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(1.0 - roughness, F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}   

static const float3 Fdielectric = 0.04;
static const float Epsilon = 0.00001;

float4 main(RS2PS input) : SV_Target
{
	PerInstanceData instance_data = per_instance_data[instance_id.id];
	SurfaceShaderParameters pbr_material = materials_buffer[instance_data.material_id];

	float4 albedo = pbr_material.base_color_factor;
	float metalness = pbr_material.metallic_factor;
	float roughness = pbr_material.roughness_factor;
	float3 normals = normalize(input.normal);
	float occlusion = 1;
	float3 emissive = 0;
	float2 uv = input.texcoord_0;

	if (pbr_material.texture_enable_bitmask & (1 << ALBEDO_ENABLED_BIT))
	{
		Texture2D albedo_texture = ResourceDescriptorHeap[pbr_material.albedo_texture_index];
		albedo *= albedo_texture.Sample(g_linear_sampler, uv);

		//if(base_color.a < 0.1)
		//{
		//	discard;
		//}
	}

	if (pbr_material.texture_enable_bitmask & (1 << METALLIC_ROUGHNESS_ENABLED_BIT))
	{
		Texture2D metallic_roughness_texture = ResourceDescriptorHeap[pbr_material.metallic_roughness_texture_index];

		float4 metallicRoughnessResult = metallic_roughness_texture.Sample(g_linear_sampler, uv);

		metalness *= metallicRoughnessResult.b;
		roughness *= metallicRoughnessResult.g;
	}

	roughness = clamp(roughness, 0.045f, 1.0f);
	metalness = clamp(metalness, 0.0f, 1.0f);

	if (pbr_material.texture_enable_bitmask & (1 << NORMAL_ENABLED_BIT))
	{
		Texture2D normal_texture = ResourceDescriptorHeap[pbr_material.normal_texture_index];
		
		float3 tangent_normal = normal_texture.Sample(g_linear_sampler, uv).xyz * 2.0 - 1.0;
		//float3 tangent_normal = normal_texture.Sample(g_linear_sampler, uv).xyz * 2.0 - 1.0;
		normals = normalize(normals + tangent_normal);
	}

	if (pbr_material.texture_enable_bitmask & (1 << OCCLUSION_ENABLED_BIT))
	{
		Texture2D occlusion_texture = ResourceDescriptorHeap[pbr_material.occlusion_texture_index];
		occlusion = occlusion_texture.Sample(g_linear_sampler, uv).r;
	}

	if (pbr_material.texture_enable_bitmask & (1 << EMISSIVE_ENABLED_BIT))
	{
		Texture2D emissive_texture = ResourceDescriptorHeap[pbr_material.emissive_texture_index];
		emissive = emissive_texture.Sample(g_linear_sampler, uv).rgb;
	}

	const float3 N = normals;
	const float3 V = normalize(camera.position.xyz - input.world_position.xyz);
	const float3 R = reflect(-V, N); 

	const float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metalness); 

	float3 directLighting = 0.0;
	{
		const float3 L = -scene_parameters.directional_light_direction.xyz;
		const float3 H = normalize(V + L);
		const float3 radiance = scene_parameters.directional_light_color.rgb * scene_parameters.directional_light_intensity;

		float NDF = DistributionGGX(N, H, roughness);   
		float G   = GeometrySmith(N, V, L, roughness);    
		float3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);

		float3 numerator    = NDF * G * F;
		float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001; // + 0.0001 to prevent divide by zero
		float3 spec = numerator / denominator;

		float3 kS = F;

		float3 kD = 1.0 - kS;
		kD *= 1.0 - metalness;	                
      
		float NdotL = max(dot(N, L), 0.0);        

		directLighting += (kD * albedo.rgb / PI + spec) * radiance * NdotL;
	}

	float3 ambient_diffuse = 0.0;
	float3 ambient_spec = 0.0;
	const float ibl_intensity = scene_parameters.ambient_light_intensity;

	{
	    float3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    
		float3 kS = F;
		float3 kD = 1.0 - kS;
		kD *= 1.0 - metalness;	  
    
		float3 irradiance	= g_input_irradiance.SampleLevel(g_linear_sampler, N, 0).rgb;
		float3 diffuse      = irradiance * albedo.rgb;

		const float MAX_REFLECTION_LOD = 7.0;

		float3 prefilteredColor = g_input_radiance.SampleLevel(g_linear_sampler, R, roughness * MAX_REFLECTION_LOD).rgb;    
		float2 brdf  = g_input_brdf.SampleLevel(g_linear_sampler, float2(max(dot(N, V), 0.0), roughness), 0).rg;
		float3 spec = prefilteredColor * (F * brdf.x + brdf.y);

		ambient_diffuse = kD * diffuse * ibl_intensity;
		ambient_spec = spec * ibl_intensity;
	}

	const float shadow = scene_parameters.shadows_enabled ? ShadowCalculation(input.position_shadow_space) : 0.0f;
	const float direct_visibility = 1.0f - shadow;

	float3 result = (ambient_diffuse + ambient_spec) * occlusion + directLighting * direct_visibility + emissive;

	return float4(result, 1.0);
}
