#include <Windows.h>

import std;
import yasno;
import yasno.settings;
import system.application;
import system.profiler;

int WINAPI wWinMain(_In_ HINSTANCE hinstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
	ysn::ProfilerSetThreadName("Ysn Main Thread");

	ysn::GraphicsSettings gs;

	ysn::Application::Create(hinstance);
	auto window = ysn::Application::Get().CreateRenderWindow(L"Yasno", 1920, 1080, false);

	int ret_code = 0;
	{
		auto yasno_core = std::make_shared<ysn::Yasno>(L"Yasno", 1920, 1080);
		yasno_core->SetWindow(window);
		window->RegisterCallbacks(yasno_core);
		window->Show();

		ret_code = ysn::Application::Get().Run(yasno_core);
	}

	ysn::Application::Get().DestroyWindow(window);
	ysn::Application::Destroy();

	return ret_code;
}
