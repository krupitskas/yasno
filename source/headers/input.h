#pragma once
#include "core.h"

namespace Engine {
	namespace Input {
		extern bool keyPressedData[GLFW_KEY_LAST];
		extern bool mouseButtonPressedData[GLFW_MOUSE_BUTTON_LAST];
		extern float mouseX;
		extern float mouseY;
		extern float mouseScrollX;
		extern float mouseScrollY;

		// Handle user input
		void handleKeyInput();

		// Utility
		bool isKeyDown(int key);
		bool isMouseButtonDown(int mouseButton);

		// Callback
		void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
		void mousePosCallback(GLFWwindow* window, double xpos, double ypos);
		void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
		void mouseScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
	}
}