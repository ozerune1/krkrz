#include "tjsCommHead.h"
#include "tjsString.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "app.h"

#include <SDL3/SDL_platform_defines.h>

#if defined(SDL_PLATFORM_WINDOWS)
	#include <windows.h>
#elif defined(SDL_PLATFORM_APPLE)
	#include <sys/sysctl.h>
#elif defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_LINUX)
	#include <sys/utsname.h>
#endif

static const char *GetOSVersion()
{
	static thread_local char osVersionBuffer[256] = {};
	#if defined(SDL_PLATFORM_WINDOWS)
		OSVERSIONINFOEX osvi = {};
		osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
		if (GetVersionEx((OSVERSIONINFO*)&osvi)) {
			snprintf(osVersionBuffer, sizeof(osVersionBuffer), "Windows %lu.%lu (Build %lu)",
				osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
		}
	#elif defined(SDL_PLATFORM_APPLE)
		char version[256] = {};
		size_t len = sizeof(version);
		if (sysctlbyname("kern.osrelease", version, &len, NULL, 0) == 0) {
			snprintf(osVersionBuffer, sizeof(osVersionBuffer), "macOS %s", version);
		}
	#elif defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_LINUX)
		struct utsname buf = {};
		if (uname(&buf) == 0) {
			snprintf(osVersionBuffer, sizeof(osVersionBuffer), "Linux %s", buf.release);
		}
	#else
		snprintf(osVersionBuffer, sizeof(osVersionBuffer), "%s", SDL_GetPlatform());
	#endif

	return osVersionBuffer;
}


SDL3Application::SDL3Application()
 : tTVPApplication() 
 ,_Terminated(false)
 ,_TerminateCode(0)
 ,mKirikiriStorage(nullptr)
{
	_language = "ja";
	_country = "jp";

	// SDL規定
	_ResourcePath = TJS_W("resource://./");

	// platform 
	TVPUtf8ToUtf16(_platformName, SDL_GetPlatform());
	TVPUtf8ToUtf16(_osName, GetOSVersion());
	
#ifdef USE_SPLASHWINDOW
	mSplashWindow = nullptr;
	mSplashRenderer = nullptr;
	mSplashTexture = nullptr;
#endif

}

SDL3Application::~SDL3Application()
{
#ifdef USE_SPLASHWINDOW
	DestroySplashWindow();
#endif
	
	// SDL3 Kirikiri Storageを閉じる
	if (mKirikiriStorage) {
		SDL_CloseStorage(mKirikiriStorage);
		mKirikiriStorage = nullptr;
	}
}

// アプリ処理用の WindowForm 実装を返す
TTVPWindowForm *
SDL3Application::CreateWindowForm(class tTJSNI_Window *win)
{
#ifdef USE_SPLASHWINDOW
	DestroySplashWindow();
#endif
	TTVPWindowForm *form = new SDL3WindowForm(win);
	return form;
}

tjs_int 
SDL3Application::ScreenWidth() const
{
	SDL_DisplayID display = SDL_GetPrimaryDisplay();
	if (display) {
		SDL_Rect bounds;
		if (SDL_GetDisplayBounds(display, &bounds) == 0) {
			return bounds.w;
		}
	}
	return 0;
}

tjs_int 
SDL3Application::ScreenHeight() const
{
	SDL_DisplayID display = SDL_GetPrimaryDisplay();
	if (display) {
		SDL_Rect bounds;
		if (SDL_GetDisplayBounds(display, &bounds) == 0) {
			return bounds.h;
		}
	}
	return 0;
}

// アクティブかどうか
bool
SDL3Application::GetActivating() const 
{
	SDL3WindowForm *mainForm = (SDL3WindowForm*)MainWindowForm();
	if (!mainForm) return false;
	SDL_Window *window = (SDL_Window*)(mainForm->NativeWindowHandle());

	Uint32 flags = SDL_GetWindowFlags(window);
	return (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool
SDL3Application::GetNotMinimizing() const 
{
	SDL3WindowForm *mainForm = (SDL3WindowForm*)MainWindowForm();
	if (!mainForm) return false;
	SDL_Window *window = (SDL_Window*)(mainForm->NativeWindowHandle());

	Uint32 flags = SDL_GetWindowFlags(window);
	return (flags & SDL_WINDOW_MINIMIZED) == 0;
}

// for exception showing
void
SDL3Application::MessageDlg(const tjs_string& string, const tjs_string& caption, int type, int button)
{
	SDL_MessageBoxFlags flags;
	switch (type) {
	case mtWarning:
		flags = SDL_MESSAGEBOX_WARNING;
		break;
	case mtError:
		flags = SDL_MESSAGEBOX_ERROR;
		break;
	case mtInformation:
		flags = SDL_MESSAGEBOX_INFORMATION;
		break;
	case mtConfirmation:
		flags = SDL_MESSAGEBOX_INFORMATION;
		break;
	case mtStop:
		flags = SDL_MESSAGEBOX_ERROR;
		break;
	default:
		flags = SDL_MESSAGEBOX_INFORMATION;
		break;
	}

	std::string str_utf8, cap_utf8;
	TVPUtf16ToUtf8(str_utf8, string);
	TVPUtf16ToUtf8(cap_utf8, caption);
	
	SDL_ShowSimpleMessageBox(flags, cap_utf8.c_str(), str_utf8.c_str(), NULL);
}

// 解像度情報
tjs_int 
SDL3Application::GetDensity() const
{
	// 固定値として返す（実際のDPIを取得する方法もある）
	return 96;
}

#include "SDLDrawDevice.h"
tTJSNativeClass* 
SDL3Application::GetDefaultDrawDevice()
{
	return new tTJSNC_SDLDrawDevice();
}

void
SDL3Application::Terminate(int code)
{
	_Terminated = true;
	_TerminateCode = code;
}

void
SDL3Application::Exit(int code)
{
	SDL_Quit();
}

// DLL処理
void*
SDL3Application::LoadLibrary( const tjs_char* path )
{
	std::string path_utf8;
	TVPUtf16ToUtf8(path_utf8, path);
	void* handle = SDL_LoadObject(path_utf8.c_str());
	if (!handle) {
		const char *error = SDL_GetError();
		TVPLOG_ERROR("Failed to load library: {}", error);
	}
	return handle;
}

void*
SDL3Application::GetProcAddress( void* handle, const char* func_name)
{
	SDL_SharedObject *so_handle = static_cast<SDL_SharedObject *>(handle);
	void* func = (void*)SDL_LoadFunction(so_handle, func_name);
	if (!func) {
		const char *error = SDL_GetError();
		TVPLOG_ERROR("Failed to get function address: {}", error);
	}
	return func;
}

void 
SDL3Application::FreeLibrary( void* handle )
{
	if (handle) {
		SDL_SharedObject *so_handle = static_cast<SDL_SharedObject *>(handle);
		SDL_UnloadObject(so_handle);
	}
}

#if defined(SDL_PLATFORM_WINDOWS)
#include <windows.h>
long getAvailableMemory() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    return memInfo.ullTotalPhys;
}

#elif defined(sysconf) // LinuxやmacOSなどのUnix系
#include <unistd.h>
long getAvailableMemory() {
    return sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
}
#else
long getAvailableMemory() {
    return 0;
}
#endif

tjs_uint64
SDL3Application::GetTotalPhysMemory()
{
	return getAvailableMemory();;
}

//< システムフォント一覧取得
void 
SDL3Application::GetSystemFontList(std::vector<tjs_string>& fontFiles)
{
}

// SDL3のイベント処理関数
// この関数はアプリケーションのPollEventSystem内で呼び出される
SDL_AppResult
SDL3Application::AppEvent(const SDL_Event& event)
{
	SDL_Window* window = SDL_GetWindowFromID(event.window.windowID);
	if (!window) return SDL_APP_CONTINUE;
	
	SDL3WindowForm* form = (SDL3WindowForm*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), "form", nullptr);
	if (form) {
		form->AppEvent(event); // イベントを無視			
	}
	return SDL_APP_CONTINUE;
}


// SDL3 Kirikiri Storage関連の実装
SDL_Storage*
SDL3Application::GetKirikiriStorage()
{
	if (!mKirikiriStorage) {
		mKirikiriStorage = SDL3KirikiriStorage::CreateStorage();
		if (!mKirikiriStorage) {
			const char *error = SDL_GetError();
			TVPLOG_ERROR("Failed to create SDL3 Kirikiri Storage: ", error);
		} else {
			TVPLOG_DEBUG("SDL3 Kirikiri Storage created successfully");
		}
	}
	return mKirikiriStorage;
}

extern void InitStorageSystem(const char *orgname, const char *appname);

#if defined(SDL_PLATFORM_WINDOWS)

#include "ApplicationSpecialPath.h"
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "shlwapi.lib")
static tjs_string GetDataPathDirectory( tjs_string datapath, const tjs_string& exename ) {
	return ApplicationSpecialPath::GetDataPathDirectory(datapath, exename);
}

#else

static tjs_string GetDataPathDirectory( tjs_string datapath, const tjs_string& exename ) {
if(datapath == TJS_W("")) datapath = tjs_string(TJS_W("$(exepath)/savedata"));
	ttstr basepath = TVPExtractStoragePath(Application->ExePath());
	tjs_string_view exepath  = tjs_string_view(basepath.c_str()); 
	tjs_string_view userpath = tjs_string_view(TJS_W("user://./")); // SDLデフォルト
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(exepath)")), exepath);
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(personalpath)")), userpath);
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(appdatapath)")), userpath);
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(vistapath)")), userpath );
	datapath = string_replace_all(datapath, tjs_string_view(TJS_W("$(savedgamespath)")), userpath);
	return datapath;
}

#endif

const tjs_string& 
SDL3Application::InitDataPath()
{
	// user:// を初期化
	std::string orgname = "wamsoft";
    std::string appname = "krkrz";
	tTJSVariant val;
	if (TVPGetCommandLine(TJS_W("-orgname"), &val)) {
		tjs_string orgname_str = val.GetString();
		TVPUtf16ToUtf8(orgname, orgname_str.c_str());
	}
	if (TVPGetCommandLine(TJS_W("-appname"), &val)) {
		tjs_string appname_str = val.GetString();
		TVPUtf16ToUtf8(appname, appname_str.c_str());
	}
    InitStorageSystem(orgname.c_str(), appname.c_str());

#if defined(SDL_PLATFORM_WINDOWS) || defined(SDL_PLATFORM_LINUX)
	// -datapth オプションで保存先を差し替え・未定義時は実行ファイルの場所にある savedata
	tjs_string config_datapath;
	if (TVPGetCommandLine(TJS_W("-datapath"), &val)) {
		config_datapath = ((ttstr)val).AsStdString();
	}
	_DataPath = GetDataPathDirectory(config_datapath, ExePath());
#else
	_DataPath = TJS_W("user://./");
#endif

	return _DataPath;
}

void 
SDL3Application::OnInitialize(tTJS* scriptEngine)
{
	// 基底クラスの初期化
	tTVPApplication::OnInitialize(scriptEngine);
	scriptEngine->SetPPValue( TJS_W("sdl"), 1 );
	scriptEngine->SetPPValue( TJS_W("kirikiriz_sdl"), 1 );
}

// SDL3 Kirikiri IOStream関連の実装
SDL_IOStream*
SDL3Application::CreateIOStreamFromPath(const tjs_string& path, tjs_uint32 flags)
{
	return SDL3KirikiriIOStreamWrapper::CreateFromPath(path, flags);
}

SDL_IOStream*
SDL3Application::CreateIOStreamFromBinaryStream(iTJSBinaryStream* stream, bool ownsStream)
{
	return SDL3KirikiriIOStreamWrapper::CreateFromBinaryStream(stream, ownsStream);
}
