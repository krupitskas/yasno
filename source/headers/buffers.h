#pragma once
#include "core.h"

namespace Engine {
	namespace Buffers {
		GLuint createVAO();
		void createVBO(GLuint vaoID, GLsizeiptr verticesByteSize, const void* vertices, GLuint bindingIndex, int vertexLen, GLenum usage);
		void addVertexAttrib(GLuint vaoID, GLuint location, GLuint attribLen, GLuint offset, GLuint bindingIndex);
		void createEBO(GLuint vaoID, GLsizeiptr indicesByteSize, GLuint* indices, GLenum usage);
		void useVAO(GLuint vaoID);
		void unbindVAO();
	}
}
