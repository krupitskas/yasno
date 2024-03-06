#include "model.hpp"

#pragma warning( push )
#pragma warning( disable : 4018)
#pragma warning( disable : 4267)
#define TINYGLTF_IMPLEMENTATION
//#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TINYGLTF_NOEXCEPTION 
#include "tinygltf/tiny_gltf.h"
#pragma warning( pop ) 

#include <string>

namespace yasn
{
	void loadGltfModel()
	{
		tinygltf::Model model;
		tinygltf::TinyGLTF loader;
		std::string err;
		std::string warn;

		bool ret = loader.LoadASCIIFromFile( &model, &err, &warn, "../assets/models/cube/Cube.gltf" );

		if( !warn.empty() )
		{
			printf( "Warn: %s\n", warn.c_str() );
		}

		if( !err.empty() )
		{
			printf( "Err: %s\n", err.c_str() );
		}

		if( !ret )
		{
			printf( "Failed to parse glTF\n" );
		}
	}
}
