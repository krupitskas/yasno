#include "input.h"
#include "window.h"

namespace Engine
{
	namespace Window
	{
		GLFWwindow* nativeWindow = nullptr;
		int windowWidth = 0;
		int windowHeight = 0;

		bool createWindow( int width, int height, const char* title, bool fullScreenMode )
		{
// Init GLFW
			if( !glfwInit() )
			{
				printf( "Failed to initialize GLFW." );
				return false;
			}

			// Load OpenGL 4.6
			glfwWindowHint( GLFW_CONTEXT_VERSION_MAJOR, 4 );
			glfwWindowHint( GLFW_CONTEXT_VERSION_MINOR, 6 );
			// Load OpenGL Core Profile (No deprecated functions)
			glfwWindowHint( GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE );

			// Get primary monitor & set fullscreen mode if required
			GLFWmonitor* primaryMonitor = nullptr;
			if( fullScreenMode )
			{
				primaryMonitor = glfwGetPrimaryMonitor();
				const GLFWvidmode* mode = glfwGetVideoMode( primaryMonitor );
				width = mode->width;
				height = mode->height;
			}

			// Create window
			nativeWindow = glfwCreateWindow( width, height, title, primaryMonitor, nullptr );
			windowWidth = width;
			windowHeight = height;
			if( nativeWindow == nullptr )
			{
				printf( "Failed to create GLFW window." );
				glfwTerminate();
				return false;
			}
			glfwMakeContextCurrent( nativeWindow );
			addWindowCallbacks();

			// Init GLAD (Load OpenGL functions)
			if( !gladLoadGLLoader( (GLADloadproc)glfwGetProcAddress ) )
			{
				printf( "Failed to initialize GLAD." );
				glfwTerminate();
				return false;
			}

			glViewport( 0, 0, width, height );
			return true;
		}

		void addWindowCallbacks()
		{
			if( nativeWindow != nullptr )
			{
				glfwSetFramebufferSizeCallback( nativeWindow, windowResizeCallback );			// Window resize callback
				glfwSetKeyCallback( nativeWindow, Input::keyCallback );					// Window key callback
				glfwSetCursorPosCallback( nativeWindow, Input::mousePosCallback );			// Window mouse position callback
				glfwSetMouseButtonCallback( nativeWindow, Input::mouseButtonCallback );	// Window mouse button callback
			}
		}

		void windowResizeCallback( GLFWwindow* window, int width, int height )
		{
			windowWidth = width;
			windowHeight = height;
			glViewport( 0, 0, width, height );
			printf( "Window size is: %d x %d\n", width, height );
		}

		void close()
		{
			if( nativeWindow != nullptr )
				glfwSetWindowShouldClose( nativeWindow, GLFW_TRUE );
		}
	}
}