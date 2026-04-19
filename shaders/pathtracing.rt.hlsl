#include "shader_structs.h"
#include "shared.hlsl"
#include "brdf.hlsl"

RWTexture2D<float4> result_texture							: register(u0);
RWTexture2D<float4> accumulation_texture					: register(u1);

ConstantBuffer<CameraParameters> camera						: register(b0);
ConstantBuffer<GpuSceneParameters> scene_parameters			: register(b1);

RaytracingAccelerationStructure scene_bvh					: register(t0);
StructuredBuffer<VertexLayout> vertex_buffer				: register(t1);
StructuredBuffer<uint> index_buffer							: register(t2);
StructuredBuffer<SurfaceShaderParameters> materials_buffer	: register(t3);
StructuredBuffer<PerInstanceData> per_instance_buffer		: register(t4);

SamplerState linear_sampler									: register(s0);

PTMaterialProperties LoadMaterialProperties(int material_id, float2 uv)
{
	PTMaterialProperties result;

	SurfaceShaderParameters mat = materials_buffer[material_id];

	float3 base_color = mat.base_color_factor.rgb;
	float metalness = mat.metallic_factor;
	float roughness = mat.roughness_factor;
	float3 emissive_result = 0.0f;

	if (mat.texture_enable_bitmask & (1 << ALBEDO_ENABLED_BIT))
	{
		Texture2D albedo_texture = ResourceDescriptorHeap[mat.albedo_texture_index];
		base_color *= albedo_texture.SampleLevel(linear_sampler, uv, 0.0f).rgb;
	}

	if (mat.texture_enable_bitmask & (1 << METALLIC_ROUGHNESS_ENABLED_BIT))
	{
		Texture2D m_r_texture = ResourceDescriptorHeap[mat.metallic_roughness_texture_index];
		float3 m_r_texture_result = m_r_texture.SampleLevel(linear_sampler, uv, 0.0f).rgb;

		metalness *= m_r_texture_result.b;
		roughness *= m_r_texture_result.g;
	}

	//if (mat.texture_enable_bitmask & (1 << NORMAL_ENABLED_BIT))
	//{
	//	Texture2D normals_texture = ResourceDescriptorHeap[mat.normal_texture_index];

	//}

	//if (mat.texture_enable_bitmask & (1 << OCCLUSION_ENABLED_BIT))
	//{
	//	Texture2D occlusion_texture = ResourceDescriptorHeap[mat.occlusion_texture_index];

	//}

	if (mat.texture_enable_bitmask & (1 << EMISSIVE_ENABLED_BIT))
	{
		Texture2D emissive_texture = ResourceDescriptorHeap[mat.emissive_texture_index];
		float3 emissive_texture_result = emissive_texture.SampleLevel(linear_sampler, uv, 0.0f).rgb;

		emissive_result += emissive_texture_result.rgb;
	}

	result.base_color = base_color;
	result.metalness = metalness;
	result.emissive = emissive_result;
	result.roughness = roughness;
	result.transmissivness = 0;
	result.opacity = 1;

	return result;
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0f);
	float NdotH2 = NdotH * NdotH;

	float nom = a2;
	float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
	denom = PI * denom * denom;

	return nom / max(denom, 0.00001f);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
	return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float3 EvaluateDirectionalLight(float3 N, float3 V, float3 L, PTMaterialProperties material, float3 radiance)
{
	float roughness = clamp(material.roughness, 0.045f, 1.0f);
	float metalness = clamp(material.metalness, 0.0f, 1.0f);
	float3 albedo = material.base_color;

	float3 H = normalize(V + L);
	float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metalness);

	float NDF = DistributionGGX(N, H, roughness);
	float G = GeometrySmith(N, V, L, roughness);
	float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);

	float3 numerator = NDF * G * F;
	float denominator = 4.0f * max(dot(N, V), 0.0f) * max(dot(N, L), 0.0f) + 0.0001f;
	float3 specular = numerator / denominator;

	float3 kS = F;
	float3 kD = (1.0f - kS) * (1.0f - metalness);
	float NdotL = max(dot(N, L), 0.0f);

	return (kD * albedo / PI + specular) * radiance * NdotL;
}

[shader("raygeneration")]
void RayGen()
{
	result_texture[DispatchRaysIndex().xy] = float4(255, 0, 0, 0);

	// Initialize the ray payload
	RtxHitInfo payload;

	uint2 LaunchIndex = DispatchRaysIndex().xy;
	uint2 LaunchDimensions = DispatchRaysDimensions().xy;

	// Get the location within the dispatched 2D grid of work items
	// (often maps to pixels, so this could represent a pixel coordinate).
	uint2 pixel_xy = DispatchRaysIndex().xy;
	float2 resolution = float2(DispatchRaysDimensions().xy);

	float2 d = (((pixel_xy.xy + 0.5f) / resolution.xy) * 2.f - 1.f);

	float aspect_ratio = resolution.x / resolution.y;

	// Define a ray, consisting of origin, direction, and the min-max distance values
	RayDesc ray;
	ray.Origin = mul(camera.view_inverse, float4(0, 0, 0, 1));
	float4 target = mul(camera.projection_inverse, float4(d.x, -d.y, 1, 1));
	ray.Direction = normalize(mul(camera.view_inverse, float4(target.xyz, 0)).xyz);
	ray.TMin = 0;
	ray.TMax = 100000;

	float3 radiance = float3(0.0f, 0.0f, 0.0f);
	float3 throughput = float3(1.0f, 1.0f, 1.0f);
	float3 sky_value = float3(10.0f, 10.0f, 10.0f) * max(scene_parameters.ambient_light_intensity, 0.0f);

	uint rng_state = InitRNG(LaunchIndex, LaunchDimensions, camera.frame_number);

	int max_bounces = 4;

	for (int bounce = 0; bounce < max_bounces; bounce++)
	{
		TraceRay(
			scene_bvh,
			RAY_FLAG_NONE,
			0xFF,
			0,
			0,
			0,
			ray,
			payload
		);

		float2 uvs = float2(0.0f, 0.0f);

		if(!payload.has_hit())
		{
			radiance += throughput * sky_value; // TODO: Sample cubemap here?
			break;
		}

		float3 geometry_normal;
		float3 shading_normal;
		DecodeNormals(payload.encoded_normals, geometry_normal, shading_normal);

		float3 view_vec = -ray.Direction;

		if (dot(geometry_normal, view_vec) < 0.0f)
		{
			geometry_normal = -geometry_normal;
		}

		if (dot(geometry_normal, shading_normal) < 0.0f)
		{
			shading_normal = -shading_normal;
		}

		PTMaterialProperties material = LoadMaterialProperties(payload.material_id, payload.uvs);

		radiance += throughput * material.emissive;

		const float3 directional_light_dir = normalize(-scene_parameters.directional_light_direction.xyz);
		const float3 directional_light_radiance =
			scene_parameters.directional_light_color.xyz * scene_parameters.directional_light_intensity;

		if (scene_parameters.directional_light_intensity > 0.0f)
		{
			const float NdotL = dot(shading_normal, directional_light_dir);
			const float NgdotL = dot(geometry_normal, directional_light_dir);

			if (NdotL > 0.0f && NgdotL > 0.0f)
			{
				RayDesc shadow_ray;
				shadow_ray.Origin = OffsetRay(payload.hit_position, geometry_normal);
				shadow_ray.Direction = directional_light_dir;
				shadow_ray.TMin = 0.001f;
				shadow_ray.TMax = 100000.0f;

				RtxHitInfo shadow_payload;
				shadow_payload.material_id = -1;
				shadow_payload.encoded_normals = 0.0f;
				shadow_payload.hit_position = 0.0f;
				shadow_payload.uvs = 0.0f;

				TraceRay(
					scene_bvh,
					RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
					0xFF,
					0,
					0,
					0,
					shadow_ray,
					shadow_payload
				);

				if (!shadow_payload.has_hit())
				{
					radiance += throughput * EvaluateDirectionalLight(
						shading_normal,
						view_vec,
						directional_light_dir,
						material,
						directional_light_radiance);
				}
			}
		}

		// Sample BRDF to generate the next ray
		// First, figure out whether to sample diffuse or specular BRDF
		int brdf_type;
		if (material.metalness == 1.0f && material.roughness == 0.0f) 
		{
			// Fast path for mirrors
			brdf_type = SPECULAR_TYPE;
		} 
		else
		{
			// Decide whether to sample diffuse or specular BRDF (based on Fresnel term)
			float brdf_probability = GetBrdfProbability(material, view_vec, shading_normal);

			if (Rand(rng_state) < brdf_probability)
			{
				brdf_type = SPECULAR_TYPE;
				throughput /= brdf_probability;
			} 
			else
			{
				brdf_type = DIFFUSE_TYPE;
				throughput /= (1.0f - brdf_probability);
			}
		}

		// Run importance sampling of selected BRDF to generate reflecting ray direction
		float3 brdf_weight = 0;
		float2 u = float2(Rand(rng_state), Rand(rng_state));

		if (!EvalIndirectCombinedBRDF(u, shading_normal, geometry_normal, view_vec, material, brdf_type, ray.Direction, brdf_weight)) 
		{
			break;
		}

		// Account for surface properties using the BRDF "weight"
		throughput *= brdf_weight;

		// Offset a new ray origin from the hitpoint to prevent self-intersections
		ray.Origin = OffsetRay(payload.hit_position, geometry_normal);
	}

	if(camera.reset_accumulation > 0)
	{
		accumulation_texture[pixel_xy] = 0;
	}

	float3 result_color = 0;

	if(camera.accumulation_enabled > 0)
	{
		// Temporal accumulation
		float3 previous_color = accumulation_texture[pixel_xy].rgb;
		float3 accumulated_color = radiance;
		accumulated_color = previous_color + radiance;
		accumulation_texture[pixel_xy] = float4(accumulated_color, 1.0f);

		result_color = accumulated_color / ( camera.rtx_frames_accumulated + 1);
	}
	else
	{
		result_color = radiance;
	}

	result_texture[pixel_xy] = float4(result_color, 1.f);
}

uint3 GetIndices(uint geometry_id, uint triangle_index)
{
	PerInstanceData data = per_instance_buffer[geometry_id];

	int address = data.indices_before + triangle_index * 3;

	uint i0 = index_buffer[address + 0];
	uint i1 = index_buffer[address + 1];
	uint i2 = index_buffer[address + 2];

	return uint3(i0, i1, i2);
}

// Vertex data for usage in Hit shader
struct VertexData
{
	float3 position;
	float3 shading_normal;
	float3 geometry_normal;
	float2 uv;
};
VertexData GetVertexData(uint geometry_id, uint triangle_index, float3 barycentrics)
{
	// Get the triangle indices
	uint3 indices = GetIndices(geometry_id, triangle_index);

	VertexData v = (VertexData)0;

	VertexLayout input_vertex[3];

	float3 triangle_vertices[3];

	int address = per_instance_buffer[geometry_id].vertices_before;

	// Interpolate the vertex attributes
	for (uint i = 0; i < 3; i++)
	{
		input_vertex[i] = vertex_buffer[address + indices[i]];

		// Load and interpolate position and transform it to world space
		triangle_vertices[i] = mul(ObjectToWorld3x4(), float4(input_vertex[i].position.xyz, 1.0f)).xyz;

		v.position += triangle_vertices[i] * barycentrics[i];

		// Load and interpolate normal
		v.shading_normal += input_vertex[i].normal * barycentrics[i];

		// Load and interpolate texture coordinates
		v.uv += input_vertex[i].texcoord_0 * barycentrics[i];
	}

	// Transform normal from local to world space
	v.shading_normal = normalize(mul(ObjectToWorld3x4(), float4(v.shading_normal, 0.0f)).xyz);

	// Calculate geometry normal from triangle vertices positions
	float3 edge20 = triangle_vertices[2] - triangle_vertices[0];
	float3 edge21 = triangle_vertices[2] - triangle_vertices[1];
	float3 edge10 = triangle_vertices[1] - triangle_vertices[0];

	v.geometry_normal = normalize(cross(edge20, edge10));

	return v;
}

[shader("closesthit")]
void ClosestHit(inout RtxHitInfo payload, RtxAttributes attrib)
{
	float3 barycentrics = float3(1.f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);

	VertexData vertex = GetVertexData(InstanceIndex(), PrimitiveIndex(), barycentrics);

	payload.encoded_normals = EncodeNormals(vertex.geometry_normal, vertex.shading_normal);
	payload.hit_position = vertex.position;
	payload.material_id = per_instance_buffer[InstanceIndex()].material_id;
	payload.uvs = vertex.uv;
}

[shader("miss")]
void Miss(inout RtxHitInfo payload : SV_RayPayload)
{
    payload.material_id = -1;
    payload.encoded_normals = float4(255,255,255,255);
}
