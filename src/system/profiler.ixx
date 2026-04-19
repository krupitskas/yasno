module;

#ifdef SUPERLUMINAL_API_EXIST
#include <Superluminal/PerformanceAPI_capi.h>
#endif

export module system.profiler;

import std;

export namespace ysn
{
	void ProfilerSetThreadName(std::string_view name)
	{
	#ifdef SUPERLUMINAL_API_EXIST
		PerformanceAPI_SetCurrentThreadName(name.data());
	#else
		(void)name;
	#endif
	}

	struct ScopedZone
	{
		ScopedZone() = delete;
		ScopedZone(std::string_view name)
		{
		#ifdef SUPERLUMINAL_API_EXIST
			PerformanceAPI_BeginEvent(name.data(), nullptr, PERFORMANCEAPI_DEFAULT_COLOR);
		#else
			(void)name;
		#endif
		}

		~ScopedZone()
		{
		#ifdef SUPERLUMINAL_API_EXIST
			PerformanceAPI_EndEvent();
		#endif
		}
	};

}

module :private;
