
#ifndef __STORAGE_CACHE_H__
#define __STORAGE_CACHE_H__

#include <queue>
#include <set>
#include <vector>
#include "ThreadIntf.h"
#include "NativeEventQueue.h"
#include "UtilStreams.h"
#include <memory>


class tTVPStorageCacheThread : public tTVPThread {

	/** 読込み要求のキュー用CS */
	tTJSCriticalSection RequestQueueCS;
	/**  読込みスレッドへ読込み要求があったことを伝えるイベント */
	tTVPThreadEvent RequestQueueEvent;
	/** 読込み要求コマンドキュー */
	std::deque<ttstr> RequestQueue;
	/** 読込み要求コマンドキュー・優先ロード */
	std::deque<ttstr> RequestQueueFast;
	/** 読み込み中フラグ */
	bool loading;
	bool loadingFast;
	int waitTick;
	tjs_uint64 prevTick;
	
private:
	/*
	 * ロード要求をキャンセルする
	*/
	void CancelLoadQueue( const ttstr &name );

	void CancelAllQueue();

	/**
	 * 読込みスレッドメイン
	 */
	void Execute();

public:
    tTVPStorageCacheThread();
    ~tTVPStorageCacheThread();

	/**
	 読込みスレッドの終了を要求する(終了は待たない)
	 */
	void ExitRequest();
	
	/**
	 * 読込み要求
	 * メインスレッドから読込みスレッドへ読込みを要求する。
	 * 読込み前にエラーが発生した場合やキャッシュ上に画像があった場合は要求は行われない
	 */
	void LoadRequest( const ttstr &name, bool fast=false);

    /**
     * 登録されているキャッシュをクリアする
     * 空文字の場合は全消去
    */
    void ClearCache(const ttstr &name);

	/*
	 * キャッシュ処理中かどうか
	 */
	bool IsLoading(bool fast=false) const;

};

bool TVPCheckStorageCache(const ttstr &name, bool update=false);
iTJSBinaryStream *TVPGetStorageCache(const ttstr &name, bool entry=false);
bool TVPClearStorageCache(const ttstr &name);
void TVPClearOldStorageCache(int keepTime, bool force=false);
void TVPClearAllStorageCache();

size_t TVPGetStorageCacheSize();
void TVPSetMaxStorageCacheSize(size_t size);


#endif // __SOTRAGE_CACHE_H__
