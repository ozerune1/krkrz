#ifndef __SDL3Application_h
#define __SDL3Application_h

#include "Application.h"
#include "WindowForm.h"
#include "SDL3KirikiriStorage.h"
#include "SDL3KirikiriIOStream.h"

#include <SDL3/SDL.h>

class SDL3WindowForm : public TTVPWindowForm 
{
	friend class SDL3Application;

protected:
	SDL3WindowForm(class tTJSNI_Window* win);
	~SDL3WindowForm();

	SDL_Window *mWindow;
	bool mVisible;
	bool mEnableTouch;

	bool checkTouchDevice();

public:
	virtual void *NativeWindowHandle() const {
		return mWindow;
	}
	virtual void DestroyNativeWindow();

	virtual void GetSurfaceSize(int &w, int &h) const;
	virtual void ResizeWindow(int w, int h);

	// メインウインドウのキャプション設定
	virtual void SetCaption(const tjs_string& caption);

	virtual void OnCloseCancel();

	// 表示制御
	virtual bool GetVisible() const;
	virtual void SetVisible(bool b);

	virtual void GetCursorPos(tjs_int &x, tjs_int &y);
	virtual void SetCursorPos(tjs_int x, tjs_int y);

	virtual void SetEnableTouch( bool b );
	virtual bool GetEnableTouch() const;

	virtual void SetEnableTouchMouse( bool b );
	virtual bool GetEnableTouchMouse() const;

	//< SDLイベント処理
	bool AppEvent(const SDL_Event& event);
};


class SDL3Application : public tTVPApplication {

public:
    SDL3Application();
	virtual ~SDL3Application();

	virtual void AppInit() {}
	virtual void AppInitDone() {}
	virtual void AppIterate() {}

	//< SDLイベント処理
	virtual SDL_AppResult AppEvent(const SDL_Event& event);

	virtual void AppQuit(){}

	// システムパス初期化
	virtual void InitPath() = 0;

#ifdef USE_SPLASHWINDOW
	// スプラッシュ画面関連
	void CreateSplashWindow(const char *imagePath);
	void DestroySplashWindow();
#endif
	// SDL3 Kirikiri Storage関連
	SDL_Storage* GetKirikiriStorage();

	// SDL3 Kirikiri IOStream関連
	SDL_IOStream* CreateIOStreamFromPath(const tjs_string& path, tjs_uint32 flags = 0);
	SDL_IOStream* CreateIOStreamFromBinaryStream(iTJSBinaryStream* stream, bool ownsStream = true);

	// ----------------------------------------------------------------------
	// システム諸元取得
	// ----------------------------------------------------------------------

	virtual const tjs_string& ExePath() const { return _ExePath; }//< 実行ファイルのパス
	virtual const tjs_string& AppPath() const { return _AppPath; } //< 既定のパス
	virtual const tjs_string& ResourcePath() const {return _ResourcePath; } //< リソースフォルダのパス
	virtual const tjs_string& PluginPath() const {return _PluginPath; } //< プラグインフォルダのパス
	virtual const tjs_string& ProjectPath() const { return _ProjectPath; } //< 実行対象データのパス
	virtual const tjs_string& DataPath() const { return _DataPath; } //< 実行対象データのパス
	virtual const tjs_string& LogPath() const { return _LogPath; }; //< 実行対象データのパス

	virtual const std::string& getLanguage() const { return _language; }; //< 言語名取得
	virtual const std::string& getCountry() const { return _country; }; //< 国名取得

	// アプリ処理用の WindowForm 実装を返す
	virtual TTVPWindowForm *CreateWindowForm(class tTJSNI_Window *win);

	// スクリーンサイズを返す
	virtual tjs_int ScreenWidth() const;
	virtual tjs_int ScreenHeight() const;

	// アクティブかどうか
	virtual bool GetActivating() const;
	virtual bool GetNotMinimizing() const;

	// for exception showing
	virtual void MessageDlg(const tjs_string& string, const tjs_string& caption, int type, int button);

	virtual bool GetAsyncKeyState(tjs_uint keycode, bool getcurrent);

	virtual tjs_uint32 GetPadState(int no);

	// 解像度情報
	virtual tjs_int GetDensity() const;

	// アプリ処理用の DrawDevice実装を返す
	virtual tTJSNativeClass* GetDefaultDrawDevice();

	virtual void Terminate(int code);
	virtual void Exit(int code);

	// DLL処理
	virtual void* LoadLibrary( const tjs_char* path );
	virtual void* GetProcAddress( void* handle, const char* func_name);
	virtual void  FreeLibrary( void* handle );

	/// 物理メモリ量取得
	virtual tjs_uint64 GetTotalPhysMemory();

	// -----------------------------------------------------------------------

	bool IsTerminated() const { return _Terminated; }
	int TerminateCode() const { return _TerminateCode; }

	// ----------------------------------------------------------------------
    // ファイル処理系
	// ※パス系は全て UTF-16 で渡ってくるので注意
	// ----------------------------------------------------------------------

	//< システムフォント一覧取得
	virtual void GetSystemFontList(std::vector<tjs_string>& fontFiles); 

	// -----------------------------------------------------------------------

	/// @brief  ジョイパッドの種別
	virtual tjs_string GetJoypadType(int no);

	/**
	 * アプリ終了通知の開始と終了
	 */
	virtual void OnTerminatingStart() {}
	virtual void OnTerminatingEnd() {}

protected:
	tjs_string _ExePath;    //< 実行ファイルのパス
	tjs_string _PluginPath; //< プラグインフォルダのパス
	tjs_string _ProjectPath;   //< プロジェクトデータのパス
	tjs_string _DataPath;   //< 書き出しデータのパス
	tjs_string _LogPath;   //<  ログデータのパス
	tjs_string _AppPath;    //< 実行ファイルのある場所のパス
	tjs_string _ResourcePath; //< リソースデータのパス
	std::string _language; //< 言語名
	std::string _country; //< 国名

	bool _Terminated;
	int _TerminateCode;

#ifdef USE_SPLASHWINDOW
	// スプラッシュ画面用メンバー変数
	SDL_Window* mSplashWindow;
	SDL_Renderer* mSplashRenderer;
	SDL_Texture* mSplashTexture;
#endif

	// SDL3 Kirikiri Storage用メンバー変数
	SDL_Storage* mKirikiriStorage;

	virtual void OnInitialize(tTJS* scriptEngine);
};

// 生成用
extern SDL3Application *GetSDL3Application();

// for iTVPLocalFileSystem
extern bool SDL_NormalizeStorageName(tjs_string &name);
extern void SDL_GetLocallyAccessibleName(tjs_string &name);

// リスト処理ファイル情報取得が極端に遅いアーキテクチャがあるので分離…
bool SDL_GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withdir);
extern bool SDL_CommitSavedata();
extern bool SDL_RollbackSavedata();

#endif