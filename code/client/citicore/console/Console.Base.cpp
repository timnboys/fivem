#include <StdInc.h>
#include "Console.Base.h"

#include "Console.VariableHelpers.h"

#include <stdarg.h>

#include <tbb/concurrent_queue.h>

namespace console
{
static bool g_allowVt;

static inline bool _isdigit(char c)
{
	return (c >= '0' && c <= '9');
}

static const int g_colors[] = {
	97, // bright white, black
	91, // red
	32, // green
	93, // bright yellow
	94, // blue
	36, // cyan
	35, // magenta
	0,  // reset
	31, // dark red
	34  // dark blue
};

static void CfxPrintf(const std::string& str)
{
	for (size_t i = 0; i < str.size(); i++)
	{
		if (str[i] == '^' && _isdigit(str[i + 1]))
		{
			if (g_allowVt)
			{
				printf("\x1B[%dm", g_colors[str[i + 1] - '0']);
			}

			i += 1;
		}
		else
		{
			printf("%c", str[i]);
		}
	}
}

static void PrintfTraceListener(ConsoleChannel channel, const char* out)
{
#ifdef _WIN32
	static std::once_flag g_vtOnceFlag;

	std::call_once(g_vtOnceFlag, []()
	{
		HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

		DWORD consoleMode;
		GetConsoleMode(hConsole, &consoleMode);
		consoleMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;

		if (SetConsoleMode(hConsole, consoleMode))
		{
			g_allowVt = true;
		}
	});
#else
	g_allowVt = true;
#endif

	static std::once_flag g_initConsoleFlag;
	static std::condition_variable g_consoleCondVar;
	static std::mutex g_consoleMutex;

	static tbb::concurrent_queue<std::string> g_consolePrintQueue;

	std::call_once(g_initConsoleFlag, []()
	{
		std::thread([]()
		{
			SetThreadName(-1, "[Cfx] Console Thread");

			while (true)
			{
				{
					std::unique_lock<std::mutex> lock(g_consoleMutex);
					g_consoleCondVar.wait(lock);
				}

				std::string str;

				while (g_consolePrintQueue.try_pop(str))
				{
					CfxPrintf(str);
				}
			}
		}).detach();
	});

	g_consolePrintQueue.push(std::string{ out });
	g_consoleCondVar.notify_all();
}

static std::vector<void(*)(ConsoleChannel, const char*)> g_printListeners = { PrintfTraceListener };
static int g_useDeveloper;

void Printf(ConsoleChannel channel, const char* format, const fmt::ArgList& argList)
{
	auto buffer = fmt::sprintf(format, argList);

	// print to all interested listeners
	for (auto& listener : g_printListeners)
	{
		listener(channel, buffer.data());
	}
}

void DPrintf(ConsoleChannel channel, const char* format, const fmt::ArgList& argList)
{
	if (g_useDeveloper > 0)
	{
		Printf(channel, format, argList);
	}
}

void PrintWarning(ConsoleChannel channel, const char* format, const fmt::ArgList& argList)
{
	// print the string directly
	Printf(channel, "^3Warning: %s^7", fmt::sprintf(format, argList));
}

void PrintError(ConsoleChannel channel, const char* format, const fmt::ArgList& argList)
{
	// print the string directly
	Printf(channel, "^1Error: %s^7", fmt::sprintf(format, argList));
}

static ConVar<int> developerVariable(GetDefaultContext(), "developer", ConVar_Archive, 0, &g_useDeveloper);
}

extern "C" DLL_EXPORT void CoreAddPrintListener(void(*function)(ConsoleChannel, const char*))
{
	console::g_printListeners.push_back(function);
}
