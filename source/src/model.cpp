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
	Model loadGltfModel()
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

		Model result;
		result.gltf = model;

		return result;
	}


 //   static void DrawMesh(tinygltf::Model &model, const tinygltf::Mesh &mesh) {
 //       //// Skip curves primitive.
 //       // if (gCurvesMesh.find(mesh.name) != gCurvesMesh.end()) {
 //       //  return;
 //       //}

 //       // if (gGLProgramState.uniforms["diffuseTex"] >= 0) {
 //       //  glUniform1i(gGLProgramState.uniforms["diffuseTex"], 0);  // TEXTURE0
 //       //}

 //       if (gGLProgramState.uniforms["isCurvesLoc"] >= 0) {
 //           glUniform1i(gGLProgramState.uniforms["isCurvesLoc"], 0);
 //       }

 //       for (size_t i = 0; i < mesh.primitives.size(); i++) {
 //           const tinygltf::Primitive &primitive = mesh.primitives[i];

 //           if (primitive.indices < 0) return;

 //           // Assume TEXTURE_2D target for the texture object.
 //           // glBindTexture(GL_TEXTURE_2D, gMeshState[mesh.name].diffuseTex[i]);

 //           std::map<std::string, int>::const_iterator it(primitive.attributes.begin());
 //           std::map<std::string, int>::const_iterator itEnd(
 //               primitive.attributes.end());

 //           for (; it != itEnd; it++) {
 //               assert(it->second >= 0);
 //               const tinygltf::Accessor &accessor = model.accessors[it->second];
 //               glBindBuffer(GL_ARRAY_BUFFER, gBufferState[accessor.bufferView].vb);
 //               CheckErrors("bind buffer");
 //               int size = 1;
 //               if (accessor.type == TINYGLTF_TYPE_SCALAR) {
 //                   size = 1;
 //               } else if (accessor.type == TINYGLTF_TYPE_VEC2) {
 //                   size = 2;
 //               } else if (accessor.type == TINYGLTF_TYPE_VEC3) {
 //                   size = 3;
 //               } else if (accessor.type == TINYGLTF_TYPE_VEC4) {
 //                   size = 4;
 //               } else {
 //                   assert(0);
 //               }
 //               // it->first would be "POSITION", "NORMAL", "TEXCOORD_0", ...
 //               if ((it->first.compare("POSITION") == 0) ||
 //                    (it->first.compare("NORMAL") == 0) ||
 //                    (it->first.compare("TEXCOORD_0") == 0)) {
 //                   if (gGLProgramState.attribs[it->first] >= 0) {
 //                       // Compute byteStride from Accessor + BufferView combination.
 //                       int byteStride =
 //                           accessor.ByteStride(model.bufferViews[accessor.bufferView]);
 //                       assert(byteStride != -1);
 //                       glVertexAttribPointer(gGLProgramState.attribs[it->first], size,
 //                                              accessor.componentType,
 //                                              accessor.normalized ? GL_TRUE : GL_FALSE,
 //                                              byteStride, BUFFER_OFFSET(accessor.byteOffset));
 //                       CheckErrors("vertex attrib pointer");
 //                       glEnableVertexAttribArray(gGLProgramState.attribs[it->first]);
 //                       CheckErrors("enable vertex attrib array");
 //                   }
 //               }
 //           }

 //           const tinygltf::Accessor &indexAccessor =
 //               model.accessors[primitive.indices];
 //           glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
 //                         gBufferState[indexAccessor.bufferView].vb);
 //           CheckErrors("bind buffer");
 //           int mode = -1;
 //           if (primitive.mode == TINYGLTF_MODE_TRIANGLES) {
 //               mode = GL_TRIANGLES;
 //           } else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
 //               mode = GL_TRIANGLE_STRIP;
 //           } else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN) {
 //               mode = GL_TRIANGLE_FAN;
 //           } else if (primitive.mode == TINYGLTF_MODE_POINTS) {
 //               mode = GL_POINTS;
 //           } else if (primitive.mode == TINYGLTF_MODE_LINE) {
 //               mode = GL_LINES;
 //           } else if (primitive.mode == TINYGLTF_MODE_LINE_LOOP) {
 //               mode = GL_LINE_LOOP;
 //           } else {
 //               assert(0);
 //           }
 //           glDrawElements(mode, indexAccessor.count, indexAccessor.componentType,
 //                           BUFFER_OFFSET(indexAccessor.byteOffset));
 //           CheckErrors("draw elements");

 //           {
 //               std::map<std::string, int>::const_iterator it(
 //                   primitive.attributes.begin());
 //               std::map<std::string, int>::const_iterator itEnd(
 //                   primitive.attributes.end());

 //               for (; it != itEnd; it++) {
 //                   if ((it->first.compare("POSITION") == 0) ||
 //                        (it->first.compare("NORMAL") == 0) ||
 //                        (it->first.compare("TEXCOORD_0") == 0)) {
 //                       if (gGLProgramState.attribs[it->first] >= 0) {
 //                           glDisableVertexAttribArray(gGLProgramState.attribs[it->first]);
 //                       }
 //                   }
 //               }
 //           }
 //       }
 //   }

 //   static void QuatToAngleAxis(const std::vector<double> quaternion,
 //                                double &outAngleDegrees,
 //                                double *axis) {
 //       double qx = quaternion[0];
 //       double qy = quaternion[1];
 //       double qz = quaternion[2];
 //       double qw = quaternion[3];

 //       double angleRadians = 2 * acos(qw);
 //       if (angleRadians == 0.0) {
 //           outAngleDegrees = 0.0;
 //           axis[0] = 0.0;
 //           axis[1] = 0.0;
 //           axis[2] = 1.0;
 //           return;
 //       }

 //       double denom = sqrt(1-qw*qw);
 //       outAngleDegrees = angleRadians * 180.0 / 3.14;
 //       axis[0] = qx / denom;
 //       axis[1] = qy / denom;
 //       axis[2] = qz / denom;
 //   }

	//static void DrawNode(tinygltf::Model &model, const tinygltf::Node &node) {
	//	// Apply xform

	//	glPushMatrix();
	//	if (node.matrix.size() == 16) {
	//		// Use `matrix' attribute
	//		glMultMatrixd(node.matrix.data());
	//	} else {
	//		// Assume Trans x Rotate x Scale order
	//		if (node.translation.size() == 3) {
	//			glTranslated(node.translation[0], node.translation[1],
	//						  node.translation[2]);
	//		}    

	//		if (node.rotation.size() == 4) {
	//			double angleDegrees;
	//			double axis[3];

	//			QuatToAngleAxis(node.rotation, angleDegrees, axis);

	//			glRotated(angleDegrees, axis[0], axis[1], axis[2]);
	//		}

	//		if (node.scale.size() == 3) {
	//			glScaled(node.scale[0], node.scale[1], node.scale[2]);
	//		}
	//	}

	//	// std::cout << "node " << node.name << ", Meshes " << node.meshes.size() <<
	//	// std::endl;

	//	// std::cout << it->first << std::endl;
	//	// FIXME(syoyo): Refactor.
	//	// DrawCurves(scene, it->second);
	//	if (node.mesh > -1) {
	//		assert(node.mesh < model.meshes.size());
	//		DrawMesh(model, model.meshes[node.mesh]);
	//	}

	//	// Draw child nodes.
	//	for (size_t i = 0; i < node.children.size(); i++) {
	//		assert(node.children[i] < model.nodes.size());
	//		DrawNode(model, model.nodes[node.children[i]]);
	//	}

	//	glPopMatrix();
	//}

	void drawModel( const Model& model )
	{
		assert( model.gltf.scenes.size() > 0 );

		int scene_to_display = model.gltf.defaultScene > -1 ? model.gltf.defaultScene : 0;
		const tinygltf::Scene &scene =  model.gltf.scenes[scene_to_display];



	}

}
