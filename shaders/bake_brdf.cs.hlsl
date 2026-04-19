#include "shader_structs.h"
#include "shared.hlsl"

RWTexture2D<float2> brdf_texture : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID)
{
	const int lut_size = 512;

	if (threadId.x >= lut_size || threadId.y >= lut_size)
	{
		return;
	}

	// lut_size - 
    float2 texcoords = (float2(threadId.x, threadId.y) + float2(0.5, 0.5)) / float2(lut_size, lut_size);
    float2 integratedBRDF = IntegrateBRDF(texcoords.x, texcoords.y);

	brdf_texture[threadId.xy] = integratedBRDF; 
}
