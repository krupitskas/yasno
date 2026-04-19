<div align="center">

# `🌩️ Yasno / Ясно 🌥️`

**DirectX 12 personal sandbox renderer**

</div>

<img src="docs/images/bistro.png" style="width: 100%; height: 100%">

<p align="center">
    <img src="docs/images/raster.png" style="width: 49%; height: 49%">
    <img src="docs/images/pathtracing.png" style="width: 49%; height: 49%">
</p>


## General

Yasno has two modes - raster and RTX pathtracing.

### Features

#### Rendering

* Dual rendering modes: forward rasterization and DXR path tracing
* Forward raster pipeline with direct and indirect draw paths
* Physically based shading (metallic-roughness workflow)
* HDR pipeline with tonemapping (None, Reinhard, ACES)
* Bindless texture/resource access

#### Lighting and Atmosphere

* Directional light with shadow mapping and PCF filtering
* Image-based lighting from prefiltered cubemaps (cubemap, irradiance, radiance)
* Configurable ambient light source (solid color, cubemap, or radiance)
* Volumetric fog pass

#### Ray Tracing

* RTX path tracing with temporal accumulation
* Environment lighting sampled from cubemap/radiance in miss shading
* Shared lighting controls across raster and path tracing modes

#### Content and Tooling

* glTF scene loading
* Packed GPU buffers for vertices, indices, materials, and instances
* Shader hot reloading
* ImGui and ImGuizmo integration for runtime debugging

### Get Started

```vcpkg install``` to install vcpkg dependencies and nuget should automatically download others
