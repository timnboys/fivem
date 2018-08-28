/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include <time.h>
#include <dbghelp.h>
#include <client/windows/handler/exception_handler.h>
#include <client/windows/crash_generation/client_info.h>
#include <client/windows/crash_generation/crash_generation_server.h>
#include <common/windows/http_upload.h>

#include <commctrl.h>
#include <shellapi.h>

#include <json.hpp>

#include <regex>
#include <sstream>

#include <optional>

#include <CfxSubProcess.h>

#include <citversion.h>

using json = nlohmann::json;

static json load_json_file(const std::wstring& path)
{
	FILE* f = _wfopen(MakeRelativeCitPath(path).c_str(), L"rb");

	if (f)
	{
		fseek(f, 0, SEEK_END);
		int len = ftell(f);
		fseek(f, 0, SEEK_SET);

		std::vector<uint8_t> text(len);
		fread(&text[0], 1, len, f);

		fclose(f);

		return json::parse(text);
	}

	return json(nullptr);
}

static json load_error_pickup()
{
	return load_json_file(L"cache\\error-pickup");
}

static std::map<std::string, std::string> load_crashometry()
{
	std::map<std::string, std::string> rv;

	FILE* f = _wfopen(MakeRelativeCitPath(L"cache\\crashometry").c_str(), L"rb");

	if (f)
	{
		while (!feof(f))
		{
			uint32_t keyLen = 0;
			uint32_t valLen = 0;

			fread(&keyLen, 1, sizeof(keyLen), f);
			fread(&valLen, 1, sizeof(valLen), f);

			if (keyLen > 0 && valLen > 0)
			{
				std::vector<char> data(keyLen + valLen + 2);
				fread(&data[0], 1, keyLen, f);
				fread(&data[keyLen + 1], 1, valLen, f);

				rv[&data[0]] = &data[keyLen + 1];
			}
		}

		fclose(f);
	}

	return rv;
}

static std::wstring crashHash;

static std::wstring HashCrash(const std::wstring& key);

static void add_crashometry(json& data)
{
	auto map = load_crashometry();
	_wunlink(MakeRelativeCitPath(L"cache\\crashometry").c_str());

	for (const auto& pair : map)
	{
		data["crashometry_" + pair.first] = pair.second;
	}

	if (!crashHash.empty())
	{
		auto ch = ToNarrow(crashHash);

		data["crash_hash"] = ch;
		data["crash_hash_id"] = HashString(ch.c_str());
		data["crash_hash_key"] = ToNarrow(HashCrash(crashHash));
	}
}

using namespace google_breakpad;

static ExceptionHandler* g_exceptionHandler;

struct ErrorData
{
	std::string errorName;
	std::string errorDescription;

	ErrorData()
	{
	}

	ErrorData(const std::string& errorName, const std::string& errorDescription)
		: errorName(errorName), errorDescription(errorDescription)
	{

	}
};

static ErrorData LookupError(uint32_t hash)
{
	FILE* f = _wfopen(MakeRelativeGamePath(L"update/x64/data/errorcodes/american.txt").c_str(), L"r");

	if (f)
	{
		char line[8192] = { 0 };

		while (fgets(line, 8191, f))
		{
			if (line[0] == '[')
			{
				strrchr(line, ']')[0] = '\0';

				if (HashString(&line[1]) == hash)
				{
					char data[8192] = { 0 };
					fgets(data, 8191, f);

					return ErrorData{&line[1], data};
				}
			}
		}
	}

	return ErrorData{};
}

static std::optional<std::tuple<ErrorData, uint64_t>> LoadErrorData()
{
	FILE* f = _wfopen(MakeRelativeCitPath(L"cache\\error_out").c_str(), L"rb");

	if (f)
	{
		uint32_t error;
		uint64_t retAddr;
		fread(&error, 1, 4, f);
		fread(&retAddr, 1, 8, f);
		fclose(f);

		return { { LookupError(error), retAddr } };
	}

	return {};
}

template<typename T>
static std::string ParseLinks(const T& text)
{
	// parse hyperlinks in the error text
	std::regex url_re(R"((http|ftp|https):\/\/[\w-]+(\.[\w-]+)+([\w.,@?^=%&amp;:\/~+#-]*[\w@?^=%&amp;\/~+#-])?)");

	std::ostringstream oss;
	std::regex_replace(std::ostreambuf_iterator<char>(oss),
		text.begin(), text.end(), url_re, "<A HREF=\"$&\">$&</A>");

	return oss.str();
}

static void OverloadCrashData(TASKDIALOGCONFIG* config)
{
	// error files?
	{
		auto data = LoadErrorData();

		if (data)
		{
			_wunlink(MakeRelativeCitPath(L"cache\\error_out").c_str());

			static ErrorData errData = std::get<ErrorData>(*data);
			static uint64_t retAddr = std::get<uint64_t>(*data);

			if (!errData.errorName.empty())
			{
				static std::wstring errTitle = fmt::sprintf(L"RAGE error: %s", ToWide(errData.errorName));
				static std::wstring errDescription = fmt::sprintf(L"A game error (at %016llx) caused " PRODUCT_NAME L" to stop working. "
					L"A crash report has been uploaded to the " PRODUCT_NAME L" developers.\n"
					L"If you require immediate support, please visit <A HREF=\"https://forum.fivem.net/\">FiveM.net</A> and mention the details below.\n\n%s",
					retAddr,
					ToWide(ParseLinks(errData.errorDescription)));

				config->pszMainInstruction = errTitle.c_str();
				config->pszContent = errDescription.c_str();

				return;
			}
		}
	}

	// FatalError crash pickup?
	{
		json pickup = load_error_pickup();

		if (!pickup.is_null())
		{
			_wunlink(MakeRelativeCitPath(L"cache\\error-pickup").c_str());

			static std::wstring errTitle = fmt::sprintf(PRODUCT_NAME L" has encountered an error");
			static std::wstring errDescription = ToWide(fmt::sprintf("%s\n\nIf you require immediate support, please visit <A HREF=\"https://forum.fivem.net/\">FiveM.net</A> and mention the details in this window.", ParseLinks(pickup["message"].get<std::string>())));

			config->pszMainInstruction = errTitle.c_str();
			config->pszContent = errDescription.c_str();

			return;
		}
	}

	// module blame?
	const wchar_t* blame = nullptr;
	const wchar_t* blame_two = nullptr;

	if (wcsstr(crashHash.c_str(), L"nvwgf"))
	{
		blame = L"NVIDIA GPU drivers";
		blame_two = L"This is not the fault of the " PRODUCT_NAME L" developers, and can not be resolved by them. NVIDIA does not provide any error reporting contacts to use to report this problem, nor do they provide "
			L"debugging information that the developers can use to resolve this issue.";
	}

	if (wcsstr(crashHash.c_str(), L"guard64"))
	{
		blame = L"Comodo Internet Security";
		blame_two = L"Please uninstall Comodo Internet Security and try again, or report the issue on the Comodo forums.";
	}

	if (wcsstr(crashHash.c_str(), L".asi"))
	{
		blame = va(L"a third-party game plugin (%s)", crashHash);
		blame_two = L"Please try removing the above file from the \"plugins\" folder in your " PRODUCT_NAME L" installation and restarting the game.";
	}

	if (wcsstr(crashHash.c_str(), L"atidxx"))
	{
		blame = L"AMD GPU drivers";
		blame_two = L"Please try updating your Radeon Software, restarting your PC and then starting the game again.";
	}

	if (blame)
	{
		static std::wstring errTitle = fmt::sprintf(L"%s encountered an error", blame);
		static std::wstring errDescription = fmt::sprintf(L"FiveM crashed due to %s.\n%s\n\nIf you require immediate support, please visit <A HREF=\"https://forum.fivem.net/\">FiveM.net</A> and mention the details in this window.", blame, blame_two);

		config->pszMainInstruction = errTitle.c_str();
		config->pszContent = errDescription.c_str();

		return;
	}
}

static std::string exType;
static std::string exWhat;

static std::wstring GetAdditionalData()
{
	{
		json error_pickup = load_error_pickup();

		if (!error_pickup.is_null())
		{
			if (error_pickup["line"] != 99999)
			{
				error_pickup["type"] = "error_pickup";
			}

			add_crashometry(error_pickup);

			return ToWide(error_pickup.dump());
		}
	}

	{
		auto errorData = LoadErrorData();

		if (errorData)
		{
			json jsonData = json::object({
				{ "type", "rage_error" },
				{ "key", std::get<ErrorData>(*errorData).errorName },
				{ "description", std::get<ErrorData>(*errorData).errorDescription },
				{ "retAddr", std::get<uint64_t>(*errorData) },
			});

			add_crashometry(jsonData);

			return ToWide(jsonData.dump());
		}
	}

	{
		json data = json::object();
		add_crashometry(data);

		if (!exType.empty())
		{
			data["exception"] = exType;
		}

		if (!exWhat.empty())
		{
			data["what"] = exWhat;
		}

		return ToWide(data.dump());;
	}
}

#include <psapi.h>

static const char* const wordList[256] = {
#include "CrashWordList.h"
};

static std::wstring HashCrash(const std::wstring& key)
{
	uint32_t hash = HashString(ToNarrow(key).c_str());

	return ToWide(fmt::sprintf("%s-%s-%s",
		std::string{ wordList[(hash >>  0) & 0xFF] },
		std::string{ wordList[(hash >>  8) & 0xFF] },
		std::string{ wordList[(hash >> 16) & 0xFF] }
	));
}

void NVSP_ShutdownSafely();

// a safe exception buffer to be allocated in low (32-bit) memory to contain what() data
struct ExceptionBuffer
{
	char data[4096];
};

static ExceptionBuffer* g_exceptionBuffer;

static void AllocateExceptionBuffer()
{
	auto _NtAllocateVirtualMemory = (HRESULT(WINAPI*)(
		HANDLE    ProcessHandle,
		PVOID     *BaseAddress,
		ULONG_PTR ZeroBits,
		PSIZE_T   RegionSize,
		ULONG     AllocationType,
		ULONG     Protect
		))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemory");

	PVOID baseAddr = NULL;
	SIZE_T size = sizeof(ExceptionBuffer);
	
	if (SUCCEEDED(_NtAllocateVirtualMemory(GetCurrentProcess(), &baseAddr, 0xFFFFFFFF80000000, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
	{
		g_exceptionBuffer = (ExceptionBuffer*)baseAddr;
	}
}

static DWORD RemoteExceptionFunc(LPVOID objectPtr)
{
	__try
	{
		std::exception* object = (std::exception*)objectPtr;

		if (g_exceptionBuffer)
		{
			strncpy(g_exceptionBuffer->data, object->what(), sizeof(g_exceptionBuffer->data));
			g_exceptionBuffer->data[sizeof(g_exceptionBuffer->data) - 1] = '\0';

			return (DWORD)(DWORD_PTR)g_exceptionBuffer;
		}

		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}

// c/p from ros-patches:five
// #TODO: factor out sanely

// {E091E21C-C61F-49F6-8560-CEF64DC42002}
#include <KnownFolders.h>
#include <ShlObj.h>

#include <dpapi.h>

#define INITGUID
#include <guiddef.h>

// {38D8F400-AA8A-4784-A9F0-26A08628577E}
DEFINE_GUID(CfxStorageGuid,
	0x38d8f400, 0xaa8a, 0x4784, 0xa9, 0xf0, 0x26, 0xa0, 0x86, 0x28, 0x57, 0x7e);

#pragma comment(lib, "rpcrt4.lib")

std::string GetOwnershipPath()
{
	PWSTR appDataPath;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appDataPath))) {
		std::string cfxPath = ToNarrow(appDataPath) + "\\DigitalEntitlements";
		CreateDirectory(ToWide(cfxPath).c_str(), nullptr);

		CoTaskMemFree(appDataPath);

		RPC_CSTR str;
		UuidToStringA(&CfxStorageGuid, &str);

		cfxPath += "\\";
		cfxPath += (char*)str;

		RpcStringFreeA(&str);

		return cfxPath;
	}

	return "";
}

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

std::string g_entitlementSource;

bool LoadOwnershipTicket()
{
	std::string filePath = GetOwnershipPath();

	FILE* f = _wfopen(ToWide(filePath).c_str(), L"rb");

	if (!f)
	{
		return false;
	}

	std::vector<uint8_t> fileData;
	int pos;

	// get the file length
	fseek(f, 0, SEEK_END);
	pos = ftell(f);
	fseek(f, 0, SEEK_SET);

	// resize the buffer
	fileData.resize(pos);

	// read the file and close it
	fread(&fileData[0], 1, pos, f);

	fclose(f);

	// decrypt the stored data - setup blob
	DATA_BLOB cryptBlob;
	cryptBlob.pbData = &fileData[0];
	cryptBlob.cbData = fileData.size();

	DATA_BLOB outBlob;

	// call DPAPI
	if (CryptUnprotectData(&cryptBlob, nullptr, nullptr, nullptr, nullptr, 0, &outBlob))
	{
		// parse the file
		std::string data(reinterpret_cast<char*>(outBlob.pbData), outBlob.cbData);

		// free the out data
		LocalFree(outBlob.pbData);

		rapidjson::Document doc;
		doc.Parse(data.c_str(), data.size());

		if (!doc.HasParseError())
		{
			if (doc.IsObject())
			{
				g_entitlementSource = doc["guid"].GetString();
				return true;
			}
		}
	}

	return false;
}


void InitializeDumpServer(int inheritedHandle, int parentPid)
{
	static bool g_running = true;

	HANDLE inheritedHandleBit = (HANDLE)inheritedHandle;
	static HANDLE parentProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, parentPid);

	CrashGenerationServer::OnClientConnectedCallback connectCallback = [] (void*, const ClientInfo* info)
	{

	};

	CrashGenerationServer::OnClientDumpRequestCallback dumpCallback = [] (void*, const ClientInfo* info, const std::wstring* filePath)
	{
		auto process_handle = info->process_handle();

		{
			EXCEPTION_POINTERS* ei;
			if (info->GetClientExceptionInfo(&ei))
			{
				auto readClient = [&](void* ptr, auto* out)
				{
					SIZE_T bytes_count = 0;
					if (!ReadProcessMemory(process_handle,
						ptr,
						out,
						sizeof(*out),
						&bytes_count)) {
						return false;
					}

					return bytes_count == sizeof(*out);
				};

				EXCEPTION_POINTERS ep;
				if (readClient(ei, &ep))
				{
					EXCEPTION_RECORD ex;
					CONTEXT cx;

					bool valid = readClient(ep.ExceptionRecord, &ex);
					valid = valid && readClient(ep.ContextRecord, &cx);

					if (valid)
					{
						DWORD processLen = 0;
						if (EnumProcessModules(process_handle, nullptr, 0, &processLen))
						{
							std::vector<HMODULE> buffer(processLen / sizeof(HMODULE));

							if (EnumProcessModules(process_handle, buffer.data(), buffer.size() * sizeof(HMODULE), &processLen))
							{
								for (HMODULE module : buffer)
								{
									const wchar_t* moduleBaseString = L"";
									MODULEINFO mi;

									if (GetModuleInformation(process_handle, module, &mi, sizeof(mi)))
									{
										auto base = reinterpret_cast<char*>(mi.lpBaseOfDll);

										if (ex.ExceptionAddress >= base && ex.ExceptionAddress < (base + mi.SizeOfImage))
										{
											wchar_t filename[MAX_PATH] = { 0 };
											GetModuleFileNameExW(process_handle, module, filename, _countof(filename));

											if (wcsstr(filename, L".exe") != nullptr)
											{
												wcscpy(filename, L"\\FiveM.exe");
											}

											// lowercase the filename
											for (wchar_t* p = filename; *p; ++p)
											{
												if (*p >= 'A' && *p <= 'Z')
												{
													*p += 0x20;
												}
											}

											// create the string
											moduleBaseString = va(L"%s+%X", wcsrchr(filename, '\\') + 1, (uintptr_t)((char*)ex.ExceptionAddress - (char*)module));

											crashHash = moduleBaseString;
										}
									}
								}
							}
						}

						// try parsing any C++ exception
						if (ex.ExceptionCode == 0xE06D7363 && ex.ExceptionInformation[0] == 0x19930520)
						{
							struct CatchableType
							{
								__int32 properties;
								__int32 pType;
								__int32 thisDisplacement;
								__int32 sizeOrOffset;
								__int32 copyFunction;
							};

							struct ThrowInfo
							{
								__int32 attributes;
								__int32 pmfnUnwind;
								__int32 pForwardCompat;
								__int32 pCatchableTypeArray;
							};

							struct CatchableTypeArray
							{
								__int32 count;
								__int32 pFirstType;
							};

							ThrowInfo ti;
							if (readClient((void*)ex.ExceptionInformation[2], &ti))
							{
								CatchableTypeArray cta;

								if (readClient((void*)(ex.ExceptionInformation[3] + ti.pCatchableTypeArray), &cta))
								{
									CatchableType type;

									if (cta.count > 0 && readClient((void*)(ex.ExceptionInformation[3] + cta.pFirstType), &type))
									{
										struct tid
										{
											const char* undName;
											uint8_t name[4096];
										} ti;

										if (type.pType && readClient((void*)(ex.ExceptionInformation[3] + type.pType), &ti))
										{
											ti.undName = nullptr;

											std::type_info& typeInfo = *(std::type_info*)&ti;
											exType = typeInfo.name();

											// strip `class ` prefix
											if (exType.substr(0, 6) == "class ")
											{
												exType = exType.substr(6);
											}

											// try getting exception data as well
											HANDLE hThread = CreateRemoteThread(process_handle, NULL, 0, RemoteExceptionFunc, (void*)(ex.ExceptionInformation[1] + type.thisDisplacement), 0, NULL);
											WaitForSingleObject(hThread, 5000);

											DWORD ret = 0;
											
											if (GetExitCodeThread(hThread, &ret))
											{
												void* exPtr = (void*)ret;

												ExceptionBuffer buf;

												if (exPtr && readClient(exPtr, &buf))
												{
													exWhat = buf.data;
												}
											}											

											CloseHandle(hThread);
										}
									}
								}
							}
						}
					}
				}
			}
			
		}

		std::map<std::wstring, std::wstring> parameters;
#ifdef GTA_NY
		parameters[L"ProductName"] = L"CitizenFX";
		parameters[L"Version"] = L"1.0";
		parameters[L"BuildID"] = L"20141213000000"; // todo i bet
#elif defined(GTA_FIVE)
		LoadOwnershipTicket();

		if (g_entitlementSource.empty())
		{
			g_entitlementSource = "default";
		}

		parameters[L"ProductName"] = L"FiveM";
		parameters[L"Version"] = va(L"1.3.0.%d", BASE_EXE_VERSION);
		parameters[L"BuildID"] = L"20170101";
		parameters[L"UserID"] = ToWide(g_entitlementSource);

        parameters[L"prod"] = L"FiveM";
        parameters[L"ver"] = L"1.0";
#endif

		auto crashometry = load_crashometry();

		parameters[L"ReleaseChannel"] = L"release";

		parameters[L"AdditionalData"] = GetAdditionalData();

		std::wstring responseBody;
		int responseCode;

		std::map<std::wstring, std::wstring> files;
		files[L"upload_file_minidump"] = *filePath;

		TerminateProcess(parentProcess, -2);

		static std::wstring windowTitle = PRODUCT_NAME L" Error";
		static std::wstring mainInstruction = PRODUCT_NAME L" has stopped working";
		
		std::wstring cuz = L"An error";

		if (!crashHash.empty())
		{
			auto ch = HashCrash(crashHash);

			if (crashHash.find(L".exe") != std::string::npos)
			{
				windowTitle = fmt::sprintf(L"Error %s", ch);
			}

			mainInstruction = fmt::sprintf(L"\"%s\"", ch);
			cuz = fmt::sprintf(L"A %s", ch);

			json crashData = load_json_file(L"citizen/crash-data.json");

			if (crashData.is_object())
			{
				auto cd = crashData.value(ToNarrow(ch), "");

				if (!cd.empty())
				{
					mainInstruction = L"FiveM crashed... but we're on it!";
					cd += "\n\n";
				}

				cuz = ToWide(cd) + cuz;
			}
		}

		if (!exType.empty())
		{
			mainInstruction = L"Exception, unhandled!";

			cuz = ToWide(fmt::sprintf("An unhandled exception (of type %s)", exType));
		}

		static std::wstring content = fmt::sprintf(L"%s caused " PRODUCT_NAME L" to stop working. A crash report is being uploaded to the " PRODUCT_NAME L" developers. If you require immediate support, please visit <A HREF=\"https://forum.fivem.net/\">FiveM.net</A> and mention the details below.", cuz);

		if (!exWhat.empty())
		{
			content += fmt::sprintf(L"\n\nException details: %s", ToWide(exWhat));
		}

		static std::optional<std::wstring> crashId;

		static const TASKDIALOG_BUTTON buttons[] = {
			{ 42, L"Close" }
		};

		static std::wstring tempSignature = fmt::sprintf(L"Crash signature: %s\nReport ID: ... [uploading?] (use Ctrl+C to copy)", crashHash);

		if (crashometry.find("kill_network_msg") != crashometry.end() && crashometry.find("reload_game") == crashometry.end())
		{
			windowTitle = L"Disconnected";
			mainInstruction = L"O\x448\x438\x431\x43A\x430 (Error)";

			content = ToWide(crashometry["kill_network_msg"]);
		}

		static TASKDIALOGCONFIG taskDialogConfig = { 0 };
		taskDialogConfig.cbSize = sizeof(taskDialogConfig);
		taskDialogConfig.hInstance = GetModuleHandle(nullptr);
		taskDialogConfig.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_EXPAND_FOOTER_AREA | TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER;
		taskDialogConfig.dwCommonButtons = 0;
		taskDialogConfig.cButtons = 1;
		taskDialogConfig.pButtons = buttons;
		taskDialogConfig.pszWindowTitle = windowTitle.c_str();
		taskDialogConfig.pszMainIcon = TD_ERROR_ICON;
		taskDialogConfig.pszMainInstruction = mainInstruction.c_str();
		taskDialogConfig.pszContent = content.c_str();
		taskDialogConfig.pszExpandedInformation = tempSignature.c_str();
		taskDialogConfig.pfCallback = [](HWND hWnd, UINT type, WPARAM wParam, LPARAM lParam, LONG_PTR data)
		{
			if (type == TDN_HYPERLINK_CLICKED)
			{
				ShellExecute(nullptr, L"open", (LPCWSTR)lParam, nullptr, nullptr, SW_NORMAL);
			}
			else if (type == TDN_BUTTON_CLICKED)
			{
				return S_OK;
			}
			else if (type == TDN_CREATED)
			{
				SendMessage(hWnd, TDM_ENABLE_BUTTON, 42, 0);
				SendMessage(hWnd, TDM_SET_MARQUEE_PROGRESS_BAR, 1, 0);
				SendMessage(hWnd, TDM_SET_PROGRESS_BAR_MARQUEE, 1, 15);
			}
			else if (type == TDN_TIMER)
			{
				if (crashId)
				{
					if (!crashId->empty())
					{
						SendMessage(hWnd, TDM_SET_ELEMENT_TEXT, TDE_EXPANDED_INFORMATION, (WPARAM)va(L"Crash signature: %s\nReport ID: %s (use Ctrl+C to copy)", crashHash.c_str(), crashId->c_str()));
					}
					else
					{
						SendMessage(hWnd, TDM_SET_PROGRESS_BAR_STATE, PBST_ERROR, 0);
					}

					SendMessage(hWnd, TDM_ENABLE_BUTTON, 42, 1);
					SendMessage(hWnd, TDM_SET_MARQUEE_PROGRESS_BAR, 0, 0);
					SendMessage(hWnd, TDM_SET_PROGRESS_BAR_POS, 100, 0);
					SendMessage(hWnd, TDM_SET_PROGRESS_BAR_STATE, PBST_NORMAL, 0);

					crashId.reset();
				}
			}

			return S_FALSE;
		};

		OverloadCrashData(&taskDialogConfig);

		auto thread = std::thread([=]()
		{
			TaskDialogIndirect(&taskDialogConfig, nullptr, nullptr, nullptr);
		});

		std::wstring fpath = MakeRelativeCitPath(L"CitizenFX.ini");

		bool uploadCrashes = true;

		if (GetFileAttributes(fpath.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			uploadCrashes = (GetPrivateProfileInt(L"Game", L"DisableCrashUpload", 0, fpath.c_str()) != 1);
		}

#ifdef GTA_NY
		if (HTTPUpload::SendRequest(L"http://cr.citizen.re:5100/submit", parameters, files, nullptr, &responseBody, &responseCode))
#elif defined(GTA_FIVE)
		if (uploadCrashes && HTTPUpload::SendRequest(L"http://updater.fivereborn.com:1127/post", parameters, files, nullptr, &responseBody, &responseCode))
#endif
		{
			crashId = responseBody;
		}
		else
		{
			crashId = L"";
		}

		if (thread.joinable())
		{
			thread.join();
		}
	};

	CrashGenerationServer::OnClientExitedCallback exitCallback = [] (void*, const ClientInfo* info)
	{
	};

	CrashGenerationServer::OnClientUploadRequestCallback uploadCallback = [] (void*, DWORD)
	{

	};

	std::wstring crashDirectory = MakeRelativeCitPath(L"crashes");

	std::wstring pipeName = L"\\\\.\\pipe\\CitizenFX_Dump";

	CrashGenerationServer server(pipeName, nullptr, connectCallback, nullptr, dumpCallback, nullptr, exitCallback, nullptr, uploadCallback, nullptr, true, &crashDirectory);
	if (server.Start())
	{
		SetEvent(inheritedHandleBit);
		WaitForSingleObject(parentProcess, INFINITE);
	}

	NVSP_ShutdownSafely();
}

bool InitializeExceptionHandler()
{
	AllocateExceptionBuffer();

	// don't initialize when under a debugger, as debugger filtering is only done when execution gets to UnhandledExceptionFilter in basedll
	if (IsDebuggerPresent())
	{
		/*SetUnhandledExceptionFilter([] (LPEXCEPTION_POINTERS pointers) -> LONG
		{
			__debugbreak();

			return 0;
		});*/

		return false;
	}

	std::wstring crashDirectory = MakeRelativeCitPath(L"crashes");
	CreateDirectory(crashDirectory.c_str(), nullptr);

	wchar_t* dumpServerBit = wcsstr(GetCommandLine(), L"-dumpserver");

	if (dumpServerBit)
	{
		wchar_t* parentPidBit = wcsstr(GetCommandLine(), L"-parentpid:");

		InitializeDumpServer(wcstol(&dumpServerBit[12], nullptr, 10), wcstol(&parentPidBit[11], nullptr, 10));

		return true;
	}

	CrashGenerationClient* client = new CrashGenerationClient(L"\\\\.\\pipe\\CitizenFX_Dump", (MINIDUMP_TYPE)(MiniDumpWithProcessThreadData | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo), new CustomClientInfo());

	if (!client->Register())
	{
		auto applicationName = MakeCfxSubProcess(L"DumpServer");

		// prepare initial structures
		STARTUPINFO startupInfo = { 0 };
		startupInfo.cb = sizeof(STARTUPINFO);

		PROCESS_INFORMATION processInfo = { 0 };

		// create an init handle
		SECURITY_ATTRIBUTES securityAttributes = { 0 };
		securityAttributes.bInheritHandle = TRUE;

		HANDLE initEvent = CreateEvent(&securityAttributes, TRUE, FALSE, nullptr);

		// create the command line including argument
		wchar_t commandLine[MAX_PATH * 8];
		if (_snwprintf(commandLine, _countof(commandLine), L"\"%s\" -dumpserver:%i -parentpid:%i", applicationName, (int)initEvent, GetCurrentProcessId()) >= _countof(commandLine))
		{
			return false;
		}

		BOOL result = CreateProcess(applicationName, commandLine, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startupInfo, &processInfo);

		if (result)
		{
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
		}

		DWORD waitResult = WaitForSingleObject(initEvent, 7500);
		if (!client->Register())
		{
			trace("Could not register with breakpad server.\n");
		}
	}

	g_exceptionHandler = new ExceptionHandler(
							L"",
							[](void* context, EXCEPTION_POINTERS* exinfo,
								MDRawAssertionInfo* assertion)
							{
								return true;
							},
							[] (const wchar_t* dump_path, const wchar_t* minidump_id, void* context, EXCEPTION_POINTERS* exinfo, MDRawAssertionInfo* assertion, bool succeeded)
							{
								return succeeded;
							},
							nullptr,
							ExceptionHandler::HANDLER_ALL,
							client
						);

	g_exceptionHandler->set_handle_debug_exceptions(true);

	// disable Windows' SetUnhandledExceptionFilter
#ifdef _M_AMD64
	DWORD oldProtect;

	LPVOID unhandledFilters[] = { 
		GetProcAddress(GetModuleHandle(L"kernelbase.dll"), "SetUnhandledExceptionFilter"),
		GetProcAddress(GetModuleHandle(L"kernel32.dll"), "SetUnhandledExceptionFilter"),
	};

	for (auto unhandledFilter : unhandledFilters)
	{
		if (unhandledFilter)
		{
			VirtualProtect(unhandledFilter, 4, PAGE_EXECUTE_READWRITE, &oldProtect);

			*(uint8_t*)unhandledFilter = 0xC3;
		}
	}
#endif

	return false;
}
