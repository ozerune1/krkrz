#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"
#include "app.h"


SDL3Application::SDL3Application()
 : tTVPApplication() 
 ,_Terminated(false)
 ,_TerminateCode(0)
 ,mKirikiriStorage(nullptr)
{
	_language = "ja";
	_country = "jp";

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

#ifdef _WIN32
#include <windows.h>
long getAvailableMemory() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    return memInfo.ullTotalPhys;
}

#else
#include <unistd.h>
#ifdef sysconf
long getAvailableMemory() {
    return sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
}
#else
long getAvailableMemory() {
    return 0;
}
#endif
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

void 
SDL3Application::OnInitialize(tTJS* scriptEngine)
{
	// 基底クラスの初期化
	tTVPApplication::OnInitialize(scriptEngine);
	scriptEngine->SetPPValue( TJS_W("sdl"), 1 );
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
