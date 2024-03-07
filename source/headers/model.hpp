#pragma once

#include "tinygltf/tiny_gltf.h"

namespace yasn
{
	struct Model
	{
		tinygltf::Model gltf;
	};

	Model loadGltfModel();

	void drawModel( const Model& model );
}