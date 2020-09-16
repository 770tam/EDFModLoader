#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#define PLOG_OMIT_LOG_DEFINES
//#define PLOG_EXPORT

#include <vector>

#include <Windows.h>
#include <cstdio>
#include <funchook.h>
#include <memory.h>

#include <plog/Log.h>
#include <plog/Initializers/RollingFileInitializer.h>

#include "proxy.h"
#include "PluginAPI.h"
#include "LoggerTweaks.h"


typedef struct {
	PluginInfo *info;
	void *module;
} PluginData;

static std::vector<PluginData*> plugins; // Holds all plugins loaded
typedef bool (*LoadDef)(PluginInfo*); 

static funchook_t *funchook;
// Called one at beginning and end of game, hooked to perform additional initialization
typedef int(__fastcall *fnk3d8f00_func)(char);
static fnk3d8f00_func fnk3d8f00_orig;
// Called for every file required used (and more?), hooked to redirect file access to Mods folder
typedef void *(__fastcall *fnk27380_func)(void*, const wchar_t*, unsigned long long);
static fnk27380_func fnk27380_orig;
// printf-like logging stub, usually given wide strings, sometimes normal strings
typedef void (__fastcall *fnk27680_func)(const wchar_t*);
static fnk27680_func fnk27680_orig;

// Verify PluginData->module can store a HMODULE
static_assert(sizeof(HMODULE) == sizeof(PluginData::module), "module field cannot store an HMODULE");

static inline BOOL FileExistsW(LPCWSTR szPath) {
	DWORD dwAttrib = GetFileAttributesW(szPath);
	return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

// Search and load all *.dll files in Mods\Plugins\ folder
static void LoadPlugins(void) {
	WIN32_FIND_DATAW ffd;
	PLOG_INFO << "Loading plugins";
	HANDLE hFind = FindFirstFileW(L"Mods\\Plugins\\*.dll", &ffd);

	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				PLOG_INFO << "Loading Plugin: " << ffd.cFileName;
				wchar_t *plugpath = new wchar_t[MAX_PATH];
				wcscpy(plugpath, L"Mods\\Plugins\\");
				wcscat(plugpath, ffd.cFileName);
				HMODULE plugin = LoadLibraryW(plugpath);
				delete[] plugpath;
				if (plugin != NULL) {
					LoadDef loadfunc = (LoadDef)GetProcAddress(plugin, "EML5_Load");
					bool unload = false;
					if (loadfunc != NULL) {
						PluginInfo *pluginInfo = new PluginInfo();
						pluginInfo->infoVersion = 0;
						if (loadfunc(pluginInfo)) {
							// Validate PluginInfo
							if (pluginInfo->infoVersion == 0) {
								PLOG_ERROR << "PluginInfo infoVersion 0, expected " << PluginInfo::MaxInfoVer;
								unload = true;
							} else if (pluginInfo->name == NULL) {
								PLOG_ERROR << "Plugin missing name";
								unload = true;
							} else if (pluginInfo->infoVersion > PluginInfo::MaxInfoVer) {
								PLOG_ERROR << "Plugin has unsupported infoVersion " << pluginInfo->infoVersion << " expected " << PluginInfo::MaxInfoVer;
								unload = true;
							} else {
								switch (pluginInfo->infoVersion) {
								case 1:
								default:
									// Latest info version
									PluginData *pluginData = new PluginData;
									pluginData->info = pluginInfo;
									pluginData->module = plugin;
									plugins.push_back(pluginData);
									break;
								}
								static_assert(PluginInfo::MaxInfoVer == 1, "Supported version changed, update version handling and this number");
							}
						} else {
							PLOG_INFO << "Unloading plugin";
							unload = true;
						}
						if (unload) {
							delete pluginInfo;
						}
					} else {
						PLOG_WARNING << "Plugin does not contain EML5_Load function";
						unload = true;
					}
					if (unload) {
						FreeLibrary(plugin);
					}
				} else {
					DWORD dwError = GetLastError();
					PLOG_ERROR << "Failed to load plugin: error " << dwError;
				}
			}
		} while (FindNextFileW(hFind, &ffd) != 0);
		// Check if finished with error
		DWORD dwError = GetLastError();
		if (dwError != ERROR_NO_MORE_FILES) {
			PLOG_ERROR << "Failed to search for plugins: error " << dwError;
		}
		FindClose(hFind);
	} else {
		DWORD dwError = GetLastError();
		if (dwError != ERROR_FILE_NOT_FOUND && dwError != ERROR_PATH_NOT_FOUND) {
			PLOG_ERROR << "Failed to search for plugins: error " << dwError;
		}
	}
}

// Add "+ModLoader" to game window
static void ModifyTitle(void) {
	HWND edfHWND = FindWindowW(L"xgs::Framework", L"EarthDefenceForce 5 for PC");
	if (edfHWND != NULL) {
		int length = GetWindowTextLengthW(edfHWND);
		const wchar_t *suffix = L" +ModLoader";
		wchar_t *buffer = new wchar_t[length + wcslen(suffix) + 1];
		GetWindowTextW(edfHWND, buffer, length + wcslen(suffix) + 1);
		wcscat(buffer, suffix);
		SetWindowTextW(edfHWND, buffer);
		delete[] buffer;
	} else {
		PLOG_WARNING << "Failed to get window handle to EDF5";
	}
}

// Early hook into game process
static int __fastcall fnk3d8f00_hook(char unk) {
	// Called with 0 at beginning, 1 later
	if (unk == 0) {
		PLOG_INFO << "Additional initialization";

		// Load plugins
		LoadPlugins();

		PLOG_INFO << "Initialization finished";
	}
	return fnk3d8f00_orig(unk);
}

// app:/ to Mods folder redirector
static void *__fastcall fnk27380_hook(void *unk, const wchar_t *path, unsigned long long length) {
	// path is not always null terminated
	// length does not include null terminator
	if (path != NULL && length >= wcslen(L"app:/") && _wcsnicmp(L"app:/", path, wcslen(L"app:/")) == 0) {
		IF_PLOG(plog::verbose) {
			wchar_t* npath = new wchar_t[length + 1];
			wmemcpy(npath, path, length);
			npath[length] = L'\0';
			PLOG_VERBOSE << "Hook: " << npath;
			delete[] npath;
		}

		wchar_t *modpath = new wchar_t[length + 3];
		wcscpy(modpath, L"./Mods/");
		wmemcpy(modpath + wcslen(modpath), path + wcslen(L"app:/"), length - wcslen(L"app:/"));
		modpath[length + 2] = L'\0';
		PLOG_DEBUG << "Checking for " << modpath;
		if (FileExistsW(modpath)) {
			PLOG_DEBUG << "Redirecting access to " << modpath;
			void *ret = fnk27380_orig(unk, modpath, length + 2);
			delete[] modpath;
			return ret;
		} else {
			delete[] modpath;
		}
	}

	void *ret = fnk27380_orig(unk, path, length);
	return ret;
}

// Internal logging hook
extern "C" {
void __fastcall fnk27680_hook(const wchar_t *fmt, ...); // wrapper to preserve registers
void __fastcall fnk27680_hook_main(const char *fmt, ...); // actual logging implementaton
}

void __fastcall fnk27680_hook_main(const char *fmt, ...) {
	if (fmt != NULL) {
		va_list args;
		va_start(args, fmt);
		if (fmt[0] == 'L' && fmt[1] == '\0' && !wcscmp((wchar_t*)fmt, L"LoadComplete:%s %s %d\n")) {
			// This wide string is formatted with normal strings
			fmt = "LoadComplete:%s %s %d";
		}
		// This is sometimes called with wide strings and normal strings
		// Try to automatically detect
		if (fmt[0] != '\0' && fmt[1] == '\0') {
			int required = _vsnwprintf(NULL, 0, (wchar_t*)fmt, args);
			wchar_t *buffer = new wchar_t[(size_t)required + 1];
			_vsnwprintf(buffer, (size_t)required + 1, (wchar_t*)fmt, args);
			va_end(args);
			// Remove new line from end of message if present
			if (required >= 1 && buffer[required - 1] == L'\n') {
				buffer[required - 1] = L'\0';
			}
			PLOG_INFO_(1) << buffer;
			delete[] buffer;
		} else {
			int required = _vsnprintf(NULL, 0, fmt, args);
			char *buffer = new char[(size_t)required + 1];
			_vsnprintf(buffer, (size_t)required + 1, fmt, args);
			va_end(args);
			// See above comment
			if (required >= 1 && buffer[required - 1] == '\n') {
				buffer[required - 1] = '\0';
			}
			PLOG_INFO_(1) << buffer;
			delete[] buffer;
		}
	} else {
		PLOG_INFO_(1) << "(null)";
	}
}

// Names for the log formatter
static const char ModLoaderStr[] = "ModLoader";
static const char GameStr[] = "Game";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	int rv;
	static plog::RollingFileAppender<eml::TxtFormatter<ModLoaderStr>> mlLogOutput("ModLoader.log");
	static plog::RollingFileAppender<eml::TxtFormatter<GameStr>> gameLogOutput("game.log");

	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH: {
		// For optimization
		DisableThreadLibraryCalls(hModule);

		// Open Log file
		DeleteFileW(L"ModLoader.log");
		DeleteFileW(L"game.log");
		plog::init(plog::debug, &mlLogOutput);
		plog::init<1>(plog::debug, &gameLogOutput);

		// Add ourself to plugin list for future reference
		PluginInfo *selfInfo = new PluginInfo;
		selfInfo->infoVersion = PluginInfo::MaxInfoVer;
		selfInfo->name = "EDF5ModLoader";
		selfInfo->version = PLUG_VER(1, 0, 2, 1);
		PluginData *selfData = new PluginData;
		selfData->info = selfInfo;
		selfData->module = hModule;
		plugins.push_back(selfData);

		PluginVersion v = selfInfo->version;
		PLOG_INFO.printf("EDF5ModLoader v%u.%u.%u Initializing\n", v.major, v.minor, v.patch);

		// Setup DLL proxy
		wchar_t path[MAX_PATH];
		if (!GetWindowsDirectoryW(path, _countof(path))) {
			DWORD dwError = GetLastError();
			PLOG_ERROR << "Failed to get windows directory path: error " << dwError;
			ExitProcess(EXIT_FAILURE);
		}

		wcscat_s(path, L"\\System32\\winmm.dll");

		PLOG_INFO << "Loading real winmm.dll";
		PLOG_INFO << "Setting up dll proxy functions";
		setupFunctions(LoadLibraryW(path));

		// Create ModLoader folders
		CreateDirectoryW(L"Mods", NULL);
		CreateDirectoryW(L"Mods\\Plugins", NULL);

		// Setup funchook
		HMODULE hmodEXE = GetModuleHandleW(NULL);
		// funchook_set_debug_file("funchook.log");
		funchook = funchook_create();

		// Hook function for additional ModLoader initialization
		PLOG_INFO << "Hooking EDF5.exe+3d8f00 (Additional initialization)";
		fnk3d8f00_orig = (fnk3d8f00_func)((PBYTE)hmodEXE + 0x3d8f00);
		rv = funchook_prepare(funchook, (void**)&fnk3d8f00_orig, fnk3d8f00_hook);
		if (rv != 0) {
			// Error
			PLOG_ERROR << "Failed to setup EDF5.exe+3d8f00 hook: " << funchook_error_message(funchook) << " (" << rv << ")";
		}

		// Add Mods folder redirector
		PLOG_INFO << "Hooking EDF5.exe+27380 (Mods folder redirector)";
		fnk27380_orig = (fnk27380_func)((PBYTE)hmodEXE + 0x27380);
		rv = funchook_prepare(funchook, (void**)&fnk27380_orig, fnk27380_hook);
		if (rv != 0) {
			// Error
			PLOG_ERROR << "Failed to setup EDF5.exe+27380 hook: " << funchook_error_message(funchook) << " (" << rv << ")";
		}
		/*
		// Add logging stub hook
		PLOG_INFO << "Hooking EDF5.exe+27680 (Logging stub hook)";
		fnk27680_orig = (fnk27680_func)((PBYTE)hmodEXE + 0x27680);
		rv = funchook_prepare(funchook, (void**)&fnk27680_orig, fnk27680_hook);
		if (rv != 0) {
			// Error
			PLOG_ERROR << "Failed to setup EDF5.exe+27680 hook: " << funchook_error_message(funchook) << " (" << rv << ")";
		}
		//*/
		// Install hooks
		rv = funchook_install(funchook, 0);
		if (rv != 0) {
			// Error
			PLOG_ERROR << "Failed to install hooks: " << funchook_error_message(funchook) << " (" << rv << ")";
		} else {
			PLOG_INFO << "Installed hooks";
		}

		// Finished
		PLOG_INFO << "Basic initialization complete";

		break;
	}
	case DLL_PROCESS_DETACH: {
		PLOG_INFO << "EDF5ModLoader Unloading";

		// Disable hooks
		rv = funchook_uninstall(funchook, 0);
		if (rv != 0) {
			// Error
			PLOG_ERROR << "Failed to uninstall hooks: " << funchook_error_message(funchook) << " (" << rv << ")";
		} else {
			PLOG_INFO << "Uninstalled hooks";
			rv = funchook_destroy(funchook);
			if (rv != 0) {
				// Error
				PLOG_ERROR << "Failed to destroy Funchook instance: " << funchook_error_message(funchook) << " (" << rv << ")";
			}
		}

		// Unload all plugins
		PLOG_INFO << "Unloading plugins";
		for (PluginData *pluginData : plugins) {
			delete pluginData->info;
			if (pluginData->module != hModule) {
				FreeLibrary((HMODULE)(pluginData->module));
			}
			delete pluginData;
		}
		plugins.clear();

		// Unload real winmm.dll
		PLOG_INFO << "Unloading real winmm.dll";
		cleanupProxy();
		break;

		// TODO: Close log file?
	}
	}
	return TRUE;
}