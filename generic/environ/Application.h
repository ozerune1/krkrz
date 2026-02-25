
#ifndef __T_APPLICATION_H__
#define __T_APPLICATION_H__

#include <vector>
#include <map>
#include <stack>
#include <algorithm>
#include <queue>
#include <string>
#include <assert.h>
#include <functional>

#include <mutex>
#include <condition_variable>
#include <thread>

#include "tjsCommHead.h"
#include "tjsUtils.h"
#include "tjsNative.h"
#include "RectItf.h"
#include "tjs.h"

#include "tvpinputdefs.h"
#include "WindowFormEvent.h"

//---------------------------------------------------------------------------
// memory allocation class
//---------------------------------------------------------------------------
class iTVPMemoryAllocator {
public:
	virtual ~iTVPMemoryAllocator() {};
	virtual void* allocate( size_t size ) = 0;
	virtual void free( void* mem ) = 0;
};


// 共通MoviePlayer 実装
// Bitmap 描画機能あり
class tTJSNI_VideoOverlay;


class NativeEventQueueIntarface;
struct NativeEvent;
class TTVPWindowForm;


struct EventCommand {
	NativeEventQueueIntarface*	target;
	NativeEvent*				command;
	EventCommand( NativeEventQueueIntarface* t, NativeEvent* c ) : target(t), command(c) {}
	EventCommand() : target(nullptr), command(nullptr) {}
};


/**
 * 特殊アプリ構成
 * Windowは一つ
 * 描画内容は DrawDevice を経由せず直接通知がくるのでそれを描画する
 * 
 * WindowForm.h をあわせて参照のこと
 * 
 * パス関係は UTF-16 (tjs_char*) 想定
 * 
 */
class tTVPApplication {

public:
	tTVPApplication();
	virtual ~tTVPApplication();

	TTVPWindowForm *MainWindowForm() const { return windows_.size() > 0 ? windows_[0] : nullptr; }

private:
	NativeEvent* createNativeEvent();
	void releaseNativeEvent( NativeEvent* ev );

	// アプリ固有イベント処理
	bool appDispatch(NativeEvent& ev);

public:
	// -------------------------------------------------------------------
	// アプリシステム側からの制御
	// -------------------------------------------------------------------

	// 引数設定用
	void InitArgs(int argc, char **argv);
	void InitArgs(int argc, tjs_char **argv);

	// アプリ初期化
	bool InitializeApplication();

	// スクリプト処理開始指示
	void Startup();

	// 画面サイズ変更通知
	void ResizeScreen();

	// 全ウインドウの再描画要請
	void RequestUpdate();

	// 処理実行
	void Dispatch();

	// ジョイパッドイベント処理
	// GetPadState() の結果を処理する
	void SendPadEvent();

protected:
	void OnInitialize(tTJS *tjs){}

public:
	// -------------------------------------------------------------------
	// スクリプトからの呼び出し
	// -------------------------------------------------------------------

	void postEvent( const NativeEvent* ev, NativeEventQueueIntarface* handler = nullptr );

	void addEventHandler( NativeEventQueueIntarface* handler ) {
		std::lock_guard<std::mutex> lock( event_handlers_mutex_ );
		std::vector<NativeEventQueueIntarface*>::iterator it = std::find(event_handlers_.begin(), event_handlers_.end(), handler);
		if( it == event_handlers_.end() ) {
			event_handlers_.push_back( handler );
		}
	}
	void removeEventHandler( NativeEventQueueIntarface* handler ) {
		std::lock_guard<std::mutex> lock( event_handlers_mutex_ );
		std::vector<NativeEventQueueIntarface*>::iterator it = std::remove(event_handlers_.begin(), event_handlers_.end(), handler);
		event_handlers_.erase( it, event_handlers_.end() );
	}

	/**
	 * タイトルの設定
	 */
	tjs_string GetTitle() const { return title_; }

	void SetTitle( const tjs_string& caption ) { 
		title_ = caption;
	}

	// -------------------------------------------------------------

	// Bitmap用のAllocatorを返す
	virtual iTVPMemoryAllocator *CreateBitmapAllocator();

	/**
	 * 画像の非同期読込み要求
	 */
	void LoadImageRequest( class iTJSDispatch2 *owner, class tTJSNI_Bitmap* bmp, const ttstr &name );

	void CacheFileRequest( const ttstr &name, bool fast=false );
	void CacheFileClear( const ttstr &name );
	void CacheFileClearOld(int keepTime, bool force);
	void CacheFileSetMaxSize( int maxSize);
	bool CacheIsLoading(bool fast=false) const;

	// -------------------------------------------------------------

	void AddWindow( TTVPWindowForm* window );
	void DelWindow( TTVPWindowForm* window );

	void SendMessage( void *window, tjs_int message, tjs_int64 wparam, tjs_int64 lparam );
	void SendTouchMessage( void *window, tjs_int type, float x, float y, float c, int id, tjs_uint64 tick );
	void SendMouseMessage( void *window, tjs_int type, int button, int shift, int x, int y);

	// ----------------------------------------------------------------------------
	// 環境依存機能群
	// ----------------------------------------------------------------------------

	/**
	 * メッセージポンプを回す
	 * 全メッセージを処理
	 */
	virtual void ProcessMessages() {};

	/**
	 * メッセージ処理を一回回す
	 */
	virtual void HandleMessage() {};

	// ----------------------------------------------------------------------
	// システム諸元取得
	// ----------------------------------------------------------------------

	virtual const tjs_string& ExePath() const = 0; //< 実行ファイルのパス
	virtual const tjs_string& AppPath() const = 0; //< 実行ファイルのある場所のパス
	virtual const tjs_string& ResourcePath() const = 0; //< リソースフォルダのパス
	virtual const tjs_string& PluginPath() const = 0; //< プラグインフォルダのパス
	virtual const tjs_string& TempPath() const = 0; //< テンポラリ領域のパス
	virtual const tjs_string& ProjectPath() const = 0; //< プロジェクトデータのパス
	virtual const tjs_string& DataPath() const = 0; //< データ書き出し先のパス
	virtual const tjs_string& LogPath() const = 0; //< ログデータのパス

	virtual const std::string& getLanguage() const = 0; //< 言語名取得
	virtual const std::string& getCountry() const = 0; //< 国名取得

	// 全体引数
	tjs_int GetArgumentCount() const { return _args.size(); }
	const tjs_char* GetArgument(tjs_uint no) { return no < _args.size() ? _args[no].c_str() : TJS_W(""); }

	// オプション以外の引数
	tjs_int GetNormalArgumentCount() const { return _nargs.size(); }
	const tjs_char* GetNormalArgument(tjs_uint no) { return no < _nargs.size() ? _nargs[no].c_str() : TJS_W(""); }

	virtual tjs_int DrawThreadNum() { return 0; }

	// ----------------------------------------------------------------------

	// スクリーンサイズを返す
	virtual tjs_int ScreenWidth() const = 0;
	virtual tjs_int ScreenHeight() const = 0;

	// アクティブかどうか
	virtual bool GetActivating() const = 0;
	virtual bool GetNotMinimizing() const = 0;

	// アプリ処理用の標準の DrawDevice実装を返す
	virtual tTJSNativeClass* GetDefaultDrawDevice() = 0;

	// アプリ処理用の WindowFOrm 実装を返す
	virtual TTVPWindowForm *CreateWindowForm(class tTJSNI_Window *win) = 0;


	// for exception showing
	virtual void MessageDlg( const tjs_string& string, const tjs_string& caption, int type, int button ) = 0;

	// 終了開始
	virtual void Terminate(int code=0) = 0; //< 終了要求
	virtual void Exit(int code) = 0; //< 強制終了処理（そのままシステム終了）

	// DLL処理
	virtual void* LoadLibrary( const tjs_char* path ) = 0;
	virtual void* GetProcAddress( void* handle, const char* func_name) = 0;
	virtual void  FreeLibrary( void* handle ) = 0;

	/// 物理メモリ量取得
	virtual tjs_uint64 GetTotalPhysMemory() = 0;

	// シェル実行
	virtual bool ShellExecute(const tjs_char *target, const tjs_char *param) {
		return false; // デフォルトは実装しない
	};

	// アプリロック取得
	virtual bool CreateAppLock(const ttstr &lockname) { 
		return true; // デフォルトはロックしない
	}

	// 乱数初期化用
	virtual void InitRandomGenerator();

	// ----------------------------------------------------------------------
    // ファイル処理系
	// ----------------------------------------------------------------------

	//< システムフォント一覧取得
	virtual void GetSystemFontList(std::vector<tjs_string>& fontFiles) {}; 

public:
	// 解像度情報
	virtual tjs_int GetDensity() const;

	// キー押し下げ状態取得
	virtual bool GetAsyncKeyState(tjs_uint keycode, bool getcurrent);

	virtual tjs_uint32 GetPadState(int no) = 0;

	// SystemControl から移管
	// イベント処理からのコールバック
	void BeginContinuousEvent();
	void EndContinuousEvent();

	virtual tjs_string GetJoypadType(int no) { return TJS_W(""); } //< joypadの種別（環境依存値）

	// ----------------------------------------------------------------------
    // 動画関係処理
	// ----------------------------------------------------------------------

protected:
	void UpdateVideoOverlay();

	// タイトル
	tjs_string title_;

	// 引数記録用
	std::vector<tjs_string> _args;
	std::vector<tjs_string> _nargs;

	void ShowException( const tjs_char* e );

	std::vector<TTVPWindowForm *> windows_;

private:

	std::vector<NativeEventQueueIntarface*>	event_handlers_;
	std::vector<NativeEvent*>				command_cache_;
	std::queue<EventCommand>				command_que_;

	class tTVPAsyncImageLoader* image_load_thread_;
	class tTVPStorageCacheThread* file_cache_thread_;

	std::mutex event_handlers_mutex_;
	std::mutex command_cache_mutex_;
	std::mutex command_que_mutex_;

	void DeliverEvents();

	bool ContinuousEventCalling;

};

extern class tTVPApplication* Application;

#endif // __T_APPLICATION_H__
