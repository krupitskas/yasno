module;

#include <DirectXMath.h>
#include <cstdint>

export module graphics.lights;

// TODO: Use LUXes here instead of "intensities"

export namespace ysn
{
	enum class AmbientLightSource : std::uint32_t
	{
		Color = 0,
		Cubemap = 1,
		Radiance = 2,
	};

	struct EnvironmentLight
	{
		float intensity = 0.25f;
		DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };
		AmbientLightSource source = AmbientLightSource::Cubemap;
	};

	struct DirectionalLight
	{
		DirectX::XMFLOAT4 color = { 1.0f, 1.0, 1.0f, 0.0f };
		DirectX::XMFLOAT4 direction = { 0.1f, -1.0f, 0.45f, 0.0f };
		float intensity = 1.0f;

		bool cast_shadow = true;
	};

	struct PointLight
	{
		float intensity = 0.0f;
		DirectX::XMFLOAT3 color = { 0.0f, 0.0, 0.0f };
	};

	struct SpotLight
	{
		float intensity = 0.0f;
		DirectX::XMFLOAT3 color = { 0.0f, 0.0, 0.0f };
	};
}
