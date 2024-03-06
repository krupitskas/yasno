#include "core.h"

namespace Engine
{
	namespace Buffers
	{
		GLuint createVAO()
		{
			unsigned int vaoID;
			glCreateVertexArrays( 1, &vaoID );
			glBindVertexArray( vaoID );
			return vaoID;
		}

		void createVBO( GLuint vaoID, GLsizeiptr verticesByteSize, const void* vertices, GLuint bindingIndex, int vertexLen, GLenum usage )
		{
			GLuint vboID;
			glCreateBuffers( 1, &vboID );
			glNamedBufferData( vboID, verticesByteSize, vertices, usage );
			glVertexArrayVertexBuffer( vaoID, bindingIndex, vboID, 0, vertexLen * sizeof( float ) );
		}

		void addVertexAttrib( GLuint vaoID, GLuint location, GLuint attribLen, GLuint offset, GLuint bindingIndex )
		{
			glVertexArrayAttribFormat( vaoID, location, attribLen, GL_FLOAT, GL_FALSE, offset );
			glVertexArrayAttribBinding( vaoID, location, bindingIndex );
			glEnableVertexArrayAttrib( vaoID, location );
		}

		void createEBO( GLuint vaoID, GLsizeiptr indicesByteSize, GLuint* indices, GLenum usage )
		{
			GLuint eboID;
			glCreateBuffers( 1, &eboID );
			glNamedBufferData( eboID, indicesByteSize, indices, usage );
			glVertexArrayElementBuffer( vaoID, eboID );
		}

		void useVAO( GLuint vaoID )
		{
			glBindVertexArray( vaoID );
		}

		void unbindVAO()
		{
			glBindVertexArray( 0 );
		}
	}
}