#include "shader_structs.h"

RWTexture2D<float4> HDRTexture : register(u0);
RWTexture2D<float4> LDRTexture : register(u1);

ConstantBuffer<TonemapParameters> parameters : register(b0);

//https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return clamp((x*(a*x + b)) / (x*(c*x + d) + e), 0.0f, 1.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID)
{
	if(threadId.x >= parameters.display_width || threadId.y >= parameters.display_height)
		return;

	const float gamma = 2.2;
	const float gamma_rcp = 1.0 / gamma;
	const float3 hdr_source = HDRTexture[threadId.xy];
	const float exposure = max(parameters.exposure, 0.0f);
	const float3 hdr_exposed = hdr_source * exposure;

	float3 ldr_result = 0;

	// This 'if' is not optimized but this is only for research purpose
	// In normal world should be ifdef or different shader code (ubershader)
	if (parameters.tonemap_method == 0) // None
	{
		ldr_result = hdr_exposed;
	}
	else if (parameters.tonemap_method == 1) // Reinhard
	{
		ldr_result = 1.0f - exp(-hdr_exposed);
	}
	else if(parameters.tonemap_method == 2) // Aces
	{
		ldr_result = ACESFilm(hdr_exposed);
	}
	else
	{
		// Error magenta
		ldr_result = float3(1, 0, 1);
	}

	// Gamma correction
	ldr_result = pow(ldr_result, gamma_rcp);

	LDRTexture[threadId.xy] = float4(ldr_result, 0.0);
}
