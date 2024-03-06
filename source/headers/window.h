#pragma once
#include "core.h"

namespace Engine {
	namespace Window {
		extern GLFWwindow* nativeWindow;
		extern int windowWidth;
		extern int windowHeight;

		bool createWindow(int width, int height, const char* title, bool fullScreenMode);
		void addWindowCallbacks();
		void windowResizeCallback(GLFWwindow* window, int width, int height);
		void close();
	}
}