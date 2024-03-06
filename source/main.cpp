#include "core.h"

#include "texture.hpp"
#include "model.hpp"

using namespace Engine;

void terminateGLFW();

int main()
{
	const int windowWidth = 1920;
	const int windowHeight = 1080;
	const bool fullScreenMode = false;

	// Create Window
	const bool success = Window::createWindow( windowWidth, windowHeight, "Yasno", fullScreenMode );
	if( !success ) return -1;

	// Initialize shader
	// Remember to delete shaders created this way at the end
	Shader* shader = NULL;
	try
	{
		shader = new Shader( "shaders/color.vert", "shaders/color.frag" );
	}
	catch( std::exception& e )
	{
		std::cout << e.what() << std::endl;
		terminateGLFW();
		return -1;
	}

	const auto texture = yasn::loadTexture();

	yasn::loadGltfModel();

	Vertex vertices[] = {
		{ glm::vec3( 0.5f, 0.5f, 0.0f ),	glm::vec4( 0.9f, 0.8f, 0.2f, 1.0f ), glm::vec2( 1.0f , 1.0f ) }, // Top right
		{ glm::vec3( 0.5f, -0.5f, 0.0f ),	glm::vec4( 0.2f, 0.9f, 0.8f, 1.0f ), glm::vec2( 1.0f , 0.0f ) }, // Bottom right
		{ glm::vec3( -0.5f, -0.5f, 0.0f ),	glm::vec4( 0.8f, 0.2f, 0.9f, 1.0f ), glm::vec2( 0.0f , 0.0f ) }, // Bottom left
		{ glm::vec3( -0.5f, 0.5f, 0.0f ),	glm::vec4( 0.8f, 0.9f, 0.2f, 1.0f ), glm::vec2( 0.0f , 1.0f ) }, // Top left
	};
	// Automaticall calculate required data
	GLuint vertexLen = sizeof( Vertex ) / sizeof( float );
	GLsizeiptr verticesByteSize = sizeof( vertices );
	GLuint vertexCount = (GLuint)( verticesByteSize / vertexLen / sizeof( float ) );
	// Set usage type GL_STATIC_DRAW, GL_DYNAMIC_DRAW, etc.
	GLenum usage = GL_STATIC_DRAW;

	// Create the indices
	GLuint indices[] = {
		0, 1, 2,
		2, 3, 0
	};
	GLuint indicesByteSize = sizeof( indices );
	GLuint indicesLen = (GLuint)( indicesByteSize / sizeof( GLuint ) );

	// Create VAO, VBO, EBO & set attributes
	GLuint vaoID = Buffers::createVAO();
	GLuint bindingIndex = 0;
	Buffers::createVBO( vaoID, verticesByteSize, vertices, bindingIndex, vertexLen, usage );
	Buffers::createEBO( vaoID, indicesByteSize, indices, usage );
	Buffers::addVertexAttrib( vaoID, 0, 3, offsetof( Vertex, position ), bindingIndex );	// Position
	Buffers::addVertexAttrib( vaoID, 1, 4, offsetof( Vertex, color ), bindingIndex );		// Color
	Buffers::addVertexAttrib( vaoID, 2, 2, offsetof( Vertex, texCoord ), bindingIndex );		// Color

	// Set clear color
	glClearColor( 0.2f, 0.3f, 0.3f, 1.0f );

	// Main loop
	while( !glfwWindowShouldClose( Window::nativeWindow ) )
	{
		glClear( GL_COLOR_BUFFER_BIT );

		Input::handleKeyInput();

		glBindTexture(GL_TEXTURE_2D, texture);
		Buffers::useVAO( vaoID );
		shader->use();
		glDrawElements( GL_TRIANGLES, indicesLen, GL_UNSIGNED_INT, 0 );

		glfwSwapBuffers( Window::nativeWindow );
		glfwPollEvents();
	}

	// Terminate
	delete shader;
	terminateGLFW();
	return 0;
}

void Input::handleKeyInput()
{
	if( Input::isKeyDown( GLFW_KEY_ESCAPE ) )
	{
		Window::close();
	}
}

void terminateGLFW()
{
	glfwDestroyWindow( Window::nativeWindow );
	glfwTerminate();
}