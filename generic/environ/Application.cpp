/**
 * メモ
 */
#include "tjsCommHead.h"

#include "tjsError.h"
#include "tjsDebug.h"

#include <errno.h>
#include <string>
#include <stdio.h>
#include <climits>
#include "Application.h"

#include "ScriptMgnIntf.h"
#include "SystemIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "TickCount.h"
#include "NativeEventQueue.h"
#include "CharacterSet.h"
#include "WindowForm.h"
#include "SysInitImpl.h"
#include "MsgImpl.h"
#include "FontSystem.h"
#include "GraphicsLoadThread.h"
#include "MsgLoad.h"
#include "Random.h"
#include "Exception.h"
#include "StorageImpl.h"
#include "StorageCache.h"
#include "StorageIntf.h"
#include "ScriptMgnIntf.h"

#include <picojson/picojson.h>
#include <algorithm>

tTVPApplication* Application;

/**
 * コンストラクタ
 */
tTVPApplication::tTVPApplication()
: image_load_thread_(nullptr)
, file_cache_thread_(NULL)
, ContinuousEventCalling(false)
{
	Application = this;
}

/**
 * デストラクタ
 */
tTVPApplication::~tTVPApplication() {
	if(image_load_thread_) {
		try {
			// image_load_thread_->ExitRequest();
			delete image_load_thread_;
			image_load_thread_ = nullptr;
		} catch(...) {
			// ignore errors
		}
	}
	if(file_cache_thread_) {
		try {
			// image_load_thread_->ExitRequest();
			delete file_cache_thread_;
			file_cache_thread_ = nullptr;
		} catch(...) {
			// ignore errors
		}
	}
}

// ---------------------------------------------------------
// private
// ---------------------------------------------------------

NativeEvent* tTVPApplication::createNativeEvent() {
	std::lock_guard<std::mutex> lock( command_cache_mutex_ );
	if( command_cache_.empty() ) {
		return new NativeEvent();
	} else {
		NativeEvent* ret = command_cache_.back();
		command_cache_.pop_back();
		return ret;
	}
}

void tTVPApplication::releaseNativeEvent( NativeEvent* ev ) {
	std::lock_guard<std::mutex> lock( command_cache_mutex_ );
	command_cache_.push_back( ev );
}


bool tTVPApplication::appDispatch(NativeEvent& ev) {
	switch( ev.Message ) {
	case AM_STARTUP_SCRIPT:
		TVPInitializeStartupScript();
		return true;
	default:
		break;
	}
	return false;
}

// -------------------------------------------------------------------
// アプリシステム側からの制御
// -------------------------------------------------------------------

void tTVPApplication::ShowException( const tjs_char* e ) 
{
	ttstr error_message(e);
	TVPLOG_CRITICAL("{}", error_message);
	tjs_string caption = (const tjs_char*)TVPFatalError;
	MessageDlg( e, caption, 0, 0 );
}

// 引数処理・UTF-8用
void 
tTVPApplication::InitArgs(int argc, char **argv)
{
	int args_done = false;
	for (int i=0;i<argc;i++) {
		std::string arg = argv[i];
		tjs_string warg;
		TVPUtf8ToUtf16( warg, arg);
		_args.push_back(warg);

		// オプション以外の引数を　_nargs に記録する
		if (!args_done) {
			if (warg[0] == TJS_W('-')) {
				// -- があるとそこで本体用の引数は終了
				if (warg[1] == TJS_W('-') && warg[2] == TJS_W('\0')) {
					args_done = true;
				}
			} else {
				_nargs.push_back(warg);
			}
		}
	}
}

// 引数処理・UTF-16用
void
tTVPApplication::InitArgs(int argc, tjs_char **argv)
{
	int args_done = false;
	for (int i=0;i<argc;i++) {
		tjs_string warg = argv[i];
		_args.push_back(warg);

		// オプション以外の引数を　_nargs に記録する
		if (!args_done) {
			if (warg[0] == TJS_W('-')) {
				// -- があるとそこで本体用の引数は終了
				if (warg[1] == TJS_W('-') && warg[2] == TJS_W('\0')) {
					args_done = true;
				}
			} else {
				_nargs.push_back(warg);
			}
		}
	}
}

bool tTVPApplication::InitializeApplication() 
{
	try {
		{
			tjs_string path = Application->ResourcePath() + TJS_W("messages.json");
			tjs_uint64 flen;
			auto buf = TVPReadStream(path.c_str(), &flen);
			if (buf.get() == nullptr ) {
				TVPAddImportantLog( TJS_W("failed to load message file") );
				return false;
			}
			picojson::value v;
			std::string errorstr;
			picojson::parse( v, (const char*)buf.get(), (const char*)buf.get()+flen, &errorstr );
			if (errorstr.empty() != true ) {
				tjs_string errmessage;
				if (TVPUtf8ToUtf16( errmessage, errorstr ) ) {
					TVPAddImportantLog( errmessage.c_str() );
				}
				return false;
			}
			TVPLoadMessage(v.get<picojson::array>());
		}

		// 初期カレントディレクトリ設定
		TVPSetCurrentDirectory( AppPath() );

		// ログへOS名等出力
		TVPAddImportantLog( TVPFormatMessage(TVPProgramStartedOn, TVPGetOSName(), TVPGetPlatformName()) );

		// アーカイブデリミタ、msgmap.tjsの実行 と言った初期化処理
		TVPInitializeBaseSystems();

		// -userconf 付きで起動されたかどうかチェックする。Android だと Activity 分けた方が賢明
		// if(TVPExecuteUserConfig()) return;

		// 非同期画像読み込みは後で実装する
		image_load_thread_ = new tTVPAsyncImageLoader();
		file_cache_thread_ = new tTVPStorageCacheThread();

		// システム初期化
		TVPSystemInit();

		// スクリプトエンジン取得
		OnInitialize(TVPGetScriptEngine());

		SetTitle( tjs_string(TVPKirikiri) );

#ifndef TVP_IGNORE_LOAD_TPM_PLUGIN
//		TVPLoadPluigins(); // load plugin module *.tpm
#endif
		// start image load thread
		image_load_thread_->StartThread();
		file_cache_thread_->StartThread();

		// run main loop from activity resume.
		return true;

	} catch( const EAbort & ) {
		// nothing to do
	} catch( const Exception &exception ) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException(exception.what());
	} catch( const TJS::eTJSScriptError &e ) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage().c_str() );
	} catch( const TJS::eTJS &e) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage().c_str() );
	} catch( const std::exception &e ) {
		ShowException( ttstr(e.what()).c_str() );
	} catch( const char* e ) {
		ShowException( ttstr(e).c_str() );
	} catch( const tjs_char* e ) {
		ShowException( e );
	} catch(...) {
		ShowException( (const tjs_char*)TVPUnknownError );
	}

	return false;
}

void tTVPApplication::Startup() 
{
	// スクリプト起動指示
	NativeEvent ev(AM_STARTUP_SCRIPT,0,0);
	postEvent( &ev, nullptr );
}

void tTVPApplication::ResizeScreen()
{
    // resize 通知
	// 全ウインドウの更新実行
    for (auto it = windows_.begin();it != windows_.end();it++) {
		(*it)->SendMessage(AM_DISPLAY_RESIZE, 0, 0);
	}
}

void tTVPApplication::RequestUpdate()
{
	// 全ウインドウの表示更新要請
    for (auto it = windows_.begin();it != windows_.end();it++) {
		(*it)->SendMessage(AM_REQUEST_UPDATE, 0, 0);
	}
}

/**
 * イベントキューからすべてのイベントをディスパッチ
 */
void 
tTVPApplication::Dispatch() 
{
	try {
		// アプリむけにきたイベントを全部処理
		NativeEventQueueIntarface* handler;
		NativeEvent* event;
		do {
			handler = nullptr;
			event = nullptr;
			{
				std::lock_guard<std::mutex> lock( command_que_mutex_ );
				if( !command_que_.empty() ) {
					handler = command_que_.front().target;
					event = command_que_.front().command;
					command_que_.pop();
				}
			}
			if( event ) {
				if (handler != nullptr) {
					// ハンドラ指定付きの場合はハンドラから探して見つからったらディスパッチ
					std::lock_guard<std::mutex> lock(event_handlers_mutex_);
					auto result = std::find_if(event_handlers_.begin(), event_handlers_.end(),
												[handler](NativeEventQueueIntarface *x) {
													return x == handler;
												});
					if (result != event_handlers_.end()) {
						(*result)->Dispatch(*event);
					}
				} else {
					if (appDispatch(*event) == false) {
						// ハンドラ指定のない場合でアプリでディスパッチしないものは、すべてのハンドラでディスパッチ
						std::lock_guard<std::mutex> lock(event_handlers_mutex_);
						for (std::vector<NativeEventQueueIntarface *>::iterator it = event_handlers_.begin();
								it != event_handlers_.end(); it++) {
							if ((*it) != nullptr) {
								(*it)->Dispatch(*event);
							}
						}
					}
				}
				releaseNativeEvent(event);
			}
		} while( event );

		UpdateVideoOverlay();

		// 吉里吉里イベント配信実行
		if(!TVPSystemUninitCalled) {
			DeliverEvents();
		}

	} catch( const EAbort & ) {
		// nothing to do
	} catch( const Exception &exception ) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException(exception.what());
	} catch( const TJS::eTJSScriptError &e ) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage().c_str() );
	} catch( const TJS::eTJS &e) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage().c_str() );
	} catch( const std::exception &e ) {
		if(!TVPSystemUninitCalled)
			ShowException( ttstr(e.what()).c_str() );
	} catch( const char* e ) {
		if(!TVPSystemUninitCalled)
			ShowException( ttstr(e).c_str() );
	} catch( const tjs_char* e ) {
		if(!TVPSystemUninitCalled)
			ShowException( e );
	} catch(...) {
		if(!TVPSystemUninitCalled)
			ShowException( (const tjs_char*)TVPUnknownError );
	}

}

void tTVPApplication::UpdateVideoOverlay() 
{
	// 全ウインドウの更新実行
    for (auto it = windows_.begin();it != windows_.end();it++) {
		(*it)->UpdateVideoOverlay();
    }
}

// -------------------------------------------------------------------
// アプリ本体側から通知
// -------------------------------------------------------------------

// コマンドをメインのメッセージループに投げる
void tTVPApplication::postEvent( const NativeEvent* ev, NativeEventQueueIntarface* handler ) {
	NativeEvent* e = createNativeEvent();
	e->Message = ev->Message;
	e->WParam = ev->WParam;
	e->LParam = ev->LParam;
	{
		std::lock_guard<std::mutex> lock( command_que_mutex_ );
		command_que_.push( EventCommand( handler, e ) );
	}
}

void tTVPApplication::AddWindow( TTVPWindowForm* window ) 
{
	windows_.push_back(window);
}

void tTVPApplication::DelWindow( TTVPWindowForm* window ) 
{
	auto i = std::remove(windows_.begin(), windows_.end(), window );
	windows_.erase( i, windows_.end() );
}

void tTVPApplication::SendMessage( void *window, tjs_int message, tjs_int64 wparam, tjs_int64 lparam )
{
	// ウインドウにメッセージを送る
	for (auto it = windows_.begin();it != windows_.end();it++) {
		if( (*it)->NativeWindowHandle() == window ) {
			(*it)->SendMessage( message, wparam, lparam );
			break;
		}
	}
}

void tTVPApplication::SendTouchMessage( void *window, tjs_int type, float x, float y, float c, int id, tjs_uint64 tick )
{
	// ウインドウにメッセージを送る
	for (auto it = windows_.begin();it != windows_.end();it++) {
		if( (*it)->NativeWindowHandle() == window ) {
			(*it)->SendTouchMessage( type, x, y, c, id, tick );
			break;
		}
	}
}

void tTVPApplication::SendMouseMessage( void *window, tjs_int type, int button, int shift, int x, int y)
{
	// ウインドウにメッセージを送る
	for (auto it = windows_.begin();it != windows_.end();it++) {
		if( (*it)->NativeWindowHandle() == window ) {
			(*it)->SendMouseMessage( type, button, shift, x, y );
			break;
		}
	}
}

void tTVPApplication::LoadImageRequest( class iTJSDispatch2 *owner, class tTJSNI_Bitmap* bmp, const ttstr &name ) 
{
	if( image_load_thread_ ) {
		image_load_thread_->LoadRequest( owner, bmp, name );
	}
}

void tTVPApplication::CacheFileRequest( const ttstr &name, bool fast )
{
	if (file_cache_thread_) {
		file_cache_thread_->LoadRequest(name, fast);
	}
}

void tTVPApplication::CacheFileClear( const ttstr &name)
{
	if (file_cache_thread_) {
		file_cache_thread_->ClearCache(name);
	}
}

void tTVPApplication::CacheFileClearOld(int keepTime, bool force)
{
	TVPClearOldStorageCache(keepTime, force);
}

void tTVPApplication::CacheFileSetMaxSize( int maxSize)
{
	TVPSetMaxStorageCacheSize(maxSize);
}

bool tTVPApplication::CacheIsLoading(bool fast) const
{
	if (file_cache_thread_) {
		return file_cache_thread_->IsLoading(fast);
	}
	return false;
}

#include "NullDrawDevice.h"

// DrawDevice実装を返す
tTJSNativeClass* 
tTVPApplication::GetDefaultDrawDevice()
{
	return new tTJSNC_NullDrawDevice();
}

class BasicAllocator : public iTVPMemoryAllocator
{
public:
	BasicAllocator() {
		TVPAddLog( TJS_W("(info) Use malloc for Bitmap") );
	}
	void* allocate( size_t size ) { return malloc(size); }
	void free( void* mem ) { ::free( mem ); }
};

// Bitmap用のAllocatorを返す
iTVPMemoryAllocator *
tTVPApplication::CreateBitmapAllocator()
{
	return new BasicAllocator();
}

void 
tTVPApplication::InitRandomGenerator()
{
#ifdef pid_t
	pid_t id = gettid(); // pthread
	TVPPushEnvironNoise(&id, sizeof(id));
#endif
}

tjs_int 
tTVPApplication::GetDensity() const
{
	return 96;
}

void tTVPApplication::BeginContinuousEvent() 
{
	if (!ContinuousEventCalling) {
		ContinuousEventCalling = true;
	}
}

void tTVPApplication::EndContinuousEvent() {
	if (ContinuousEventCalling) {
		ContinuousEventCalling = false;
	}
}

void tTVPApplication::DeliverEvents() {
	if(ContinuousEventCalling)
		TVPProcessContinuousHandlerEventFlag = true; // set flag

	TVPDeliverAllEvents();
}
