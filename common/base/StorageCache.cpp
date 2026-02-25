//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Universal Storage System
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "tjsUtils.h"
#include "MsgIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"

#include "StorageCache.h"
#include "ThreadIntf.h"
#include "UserEvent.h"
#include "EventIntf.h"
#include "StorageIntf.h"
#include "MsgIntf.h"
#include "TickCount.h"

#include "UserEvent.h"

#include "Application.h"
#include "BinaryStreamBuffer.h"

#include <memory>

//---------------------------------------------------------------------------


class tTVPSharedMemoryStream : public iTJSBinaryStream
{
protected:
	std::shared_ptr<tTJSBinaryStreamBuffer> Buffer;
	size_t Size;
	tjs_uint CurrentPos;

public:
	tTVPSharedMemoryStream(std::shared_ptr<tTJSBinaryStreamBuffer> buffer) 
	: Buffer(buffer), Size(buffer->size()), CurrentPos(0) {
	}
	virtual ~tTVPSharedMemoryStream() {
	}

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) {
		if (!Buffer) return 0;

		tjs_int64 newpos;
		switch(whence)
		{
		case TJS_BS_SEEK_SET:
			if(offset >= 0)
			{
				if(offset <= Size) CurrentPos = static_cast<tjs_uint>(offset);
			}
			return CurrentPos;

		case TJS_BS_SEEK_CUR:
			if((newpos = offset + (tjs_int64)CurrentPos) >= 0)
			{
				tjs_uint np = (tjs_uint)newpos;
				if(np <= Size) CurrentPos = np;
			}
			return CurrentPos;

		case TJS_BS_SEEK_END:
			if((newpos = offset + (tjs_int64)Size) >= 0)
			{
				tjs_uint np = (tjs_uint)newpos;
				if(np <= Size) CurrentPos = np;
			}
			return CurrentPos;
		}
		return CurrentPos;
	}

	tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size) {
		if(CurrentPos + read_size >= Size)
		{
			read_size = Size - CurrentPos;
		}
		memcpy(buffer, Buffer->buffer() + CurrentPos, read_size);
		CurrentPos += read_size;
		return read_size;	
	}
	tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size) {
		TVPThrowExceptionMessage(TVPWriteError);
		return 0;
	}

	void TJS_INTF_METHOD SetEndOfStorage() {
		TVPThrowExceptionMessage(TVPWriteError);
	}

	tjs_uint64 TJS_INTF_METHOD GetSize() { return Size; }
};

size_t MaxStorageCacheSize = 200 * 1024 * 1024; // 200MB
size_t CurrentStorageCacheSize = 0;

int StorageCacheKeepTime = 30; // つかって30秒したら消去候補
int StorageCacheWaitTime = 3;  // キャッシュ再開待ちフレーム数

typedef struct {
	std::shared_ptr<tTJSBinaryStreamBuffer> buffer;
	time_t lastaccess;
	int usecount;
} StorageCacheEntry;

// キャッシュデータ保持用テーブル
std::map<ttstr, StorageCacheEntry> StorageCacheTable;
tTJSCriticalSection StorageCacheCS;

// 環境用裏口
#ifdef _GENERIC_
extern std::shared_ptr<tTJSBinaryStreamBuffer> 
TVPGetStreamBuffer(iTJSBinaryStream *stream);
#endif

std::shared_ptr<tTJSBinaryStreamBuffer>
GetStreamBuffer(iTJSBinaryStream *stream)
{
	std::shared_ptr<tTJSBinaryStreamBuffer> result;

#ifdef _GENERIC_
	result = TVPGetStreamBuffer(stream);
	if (result) return result;
#endif

	size_t size = (size_t)stream->GetSize();
	auto buf = tTJSBinaryStreamBuffer::create(size);
	if (buf) {
		TVPReadBuffer(stream, (void*)buf->buffer(), buf->size());
		result.reset(buf);
	}
	return result;
}


extern iTJSBinaryStream * _InnerTVPCreateStream(const ttstr &name, tjs_uint32 flags);

// キャッシュデータ登録
static void EntryStorageCache(const ttstr &name) 
{
	iTJSBinaryStream *Stream = nullptr;
	try {
		Stream = _InnerTVPCreateStream(name, TJS_BS_READ);
		if (Stream) {
			StorageCacheEntry entry;
			entry.buffer = GetStreamBuffer(Stream);
			entry.lastaccess = time(NULL);
			entry.usecount = 1;
			{
				tTJSCriticalSectionHolder Lock(StorageCacheCS);
				StorageCacheTable[name] = entry;
			}
			CurrentStorageCacheSize += entry.buffer->size();
			TVPLOG_DEBUG("StorageCache:entry:{}", name);
			Stream->Destruct();
			Stream = NULL;
		}
	} catch(...) {
		if (Stream) {
			Stream->Destruct();
			Stream = NULL;
		}
		TVPLOG_ERROR("StorageCache:entry:failed:{}", name);
		throw;
	}
}

// キャッシュデータ存在チェック
bool TVPCheckStorageCache(const ttstr &name, bool update) 
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	//TVPLOG_DEBUG("StorageCache: check:{}", name);
	auto i = StorageCacheTable.find(name);
	if (i != StorageCacheTable.end()) {
		if (update) {
			i->second.lastaccess = time(NULL);
			i->second.usecount++;
		}
		return true;
	}
	return false;	
}

// TVPEntryStorageCacheで登録されたキャッシュデータを取得
iTJSBinaryStream *TVPGetStorageCache(const ttstr &_name, bool entry) 
{
	ttstr name = TVPGetPlacedPath(_name);
	if(name.IsEmpty()) TVPThrowExceptionMessage(TVPCannotOpenStorage, _name);

	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	auto i = StorageCacheTable.find(name);
	if (i == StorageCacheTable.end() && entry) {
		// キャッシュに無いので登録して取得
		EntryStorageCache(name);
		i = StorageCacheTable.find(name);
	}
	if (i != StorageCacheTable.end()) {
		i->second.lastaccess = time(NULL);
		i->second.usecount--;
		TVPLOG_DEBUG("StorageCache:get:{}", name);
		return new tTVPSharedMemoryStream(i->second.buffer);
	}
	return NULL;
}

// キャッシュを破棄
bool TVPClearStorageCache(const ttstr &_name) 
{
	ttstr name = TVPGetPlacedPath(_name);
	if(name.IsEmpty()) TVPThrowExceptionMessage(TVPCannotOpenStorage, _name);

	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	auto i = StorageCacheTable.find(name);
	if (i != StorageCacheTable.end()) {
		CurrentStorageCacheSize -= i->second.buffer->size();
		StorageCacheTable.erase(i);
		TVPLOG_DEBUG("StorageCache:clear:{}", name);
		return true;
	}
	return false;
}

// 古いキャッシュを破棄
void TVPClearOldStorageCache(int keepTime, bool force)
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	for (auto it : StorageCacheTable) {
		if (it.second.lastaccess < time(NULL) - keepTime && (force || it.second.usecount <= 0)) {
			StorageCacheTable.erase(it.first);
			CurrentStorageCacheSize -= it.second.buffer->size();
		}
	}	
}

// 全キャッシュ消去
void TVPClearAllStorageCache()
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	StorageCacheTable.clear();
	CurrentStorageCacheSize = 0;
}

// キャッシュサイズを返す
size_t TVPGetStorageCacheSize()
{
	return CurrentStorageCacheSize;
}

// キャッシュサイズを設定
void TVPSetMaxStorageCacheSize(size_t size)
{
	MaxStorageCacheSize = size;
}

// キャッシュサイズが規定を超えているかどうか
bool IsOverMaxStorageCacheSize()
{
	return TVPGetStorageCacheSize() > MaxStorageCacheSize;
}


//---------------------------------------------------------------------------

tTVPStorageCacheThread::tTVPStorageCacheThread()
 : loading(false), loadingFast(false), waitTick(0), prevTick(0)
{
}

tTVPStorageCacheThread::~tTVPStorageCacheThread() 
{
	ExitRequest();
	WaitFor();
	RequestQueue.clear();
	RequestQueueFast.clear();
}

void tTVPStorageCacheThread::ExitRequest() 
{
	Terminate();
	RequestQueueEvent.Set();
}

void tTVPStorageCacheThread::Execute() 
{
	// プライオリティは最低にする
	SetPriority(ttpIdle);

	while( !GetTerminated() ) {
		// キュー追加イベント待ち
		RequestQueueEvent.WaitFor(0);
		if( GetTerminated() ) break;
		do {
			loading = false;
			loadingFast = false;

			if ( RequestQueueFast.size() ) {
				loadingFast = true;
				ttstr name;
				{
					tTJSCriticalSectionHolder cs(RequestQueueCS);
					if( RequestQueueFast.size() ) {
						name = RequestQueueFast.front();
						RequestQueueFast.pop_front();
					}
				}
				if (TVPIsCacheTargetFile(name) && !TVPCheckStorageCache(name, true) ) {
					EntryStorageCache(name);
				}
			}

			if ( RequestQueue.size() ) {
				loading = true;

				// 待ち処理
				if (waitTick > 0) {
					uint64_t tick = TVPGetTickCount();
					uint64_t diff = tick - prevTick;
					prevTick = tick;
					if (diff < waitTick) {
						waitTick -= diff;
					} else {
						waitTick = 0;
					}
				}

				if (waitTick == 0) {
					// 空きが無い場合はすこし待つ
					if (IsOverMaxStorageCacheSize()) {
						TVPLOG_INFO("StorageCache: over max size, wait {} sec", StorageCacheWaitTime);
						// 一定以上古い利用済みファイルを破棄
						TVPClearOldStorageCache(StorageCacheKeepTime * 1000);
						prevTick = TVPGetTickCount();
						waitTick = StorageCacheWaitTime * 1000;
					} else {
						ttstr name;
						{
							tTJSCriticalSectionHolder cs(RequestQueueCS);
							if (RequestQueue.size()) {
								name = RequestQueue.front();
								RequestQueue.pop_front();
							}
						}
						if (TVPIsCacheTargetFile(name) && !TVPCheckStorageCache(name, true) ) {
							EntryStorageCache(name);
						}
					}
				}
			}

			// 適宜処理移管
			TVPYieldNativeThread();

		} while( (loading || loadingFast) && !GetTerminated() );
	}
}

//---------------------------------------------------------------------------

// onLoaded( dic, is_async, is_error, error_mes ); エラーは
// sync ( main thead )
void tTVPStorageCacheThread::LoadRequest( const ttstr &_name, bool fast ) {
	ttstr name = TVPGetPlacedPath(_name); // file must exist
	if (name.IsEmpty()) {
		TVPThrowExceptionMessage(TVPCannotOpenStorage, _name);
	}
	if (fast) {
		tTJSCriticalSectionHolder cs(RequestQueueCS);
		RequestQueueFast.push_back(name);
		RequestQueueEvent.Set();
	} else {
		tTJSCriticalSectionHolder cs(RequestQueueCS);
		RequestQueue.push_back(name);
		RequestQueueEvent.Set();
	}
}


void tTVPStorageCacheThread::CancelLoadQueue( const ttstr &name ) 
{
	// キューをロックしてキャンセル
	tTJSCriticalSectionHolder cs(RequestQueueCS);
	for (auto it=RequestQueue.begin(); it!=RequestQueue.end();) {
		if (*it == name) {
			it = RequestQueue.erase(it);
		} else {
			++it;
		}
	}
	for (auto it=RequestQueueFast.begin(); it!=RequestQueueFast.end();) {
		if (*it == name) {
			it = RequestQueueFast.erase(it);
		} else {
			++it;
		}
	}
}

void tTVPStorageCacheThread::CancelAllQueue()
{
	// キューをロックしてキャンセル
	tTJSCriticalSectionHolder cs(RequestQueueCS);
	RequestQueue.clear();
	RequestQueueFast.clear();
}

void tTVPStorageCacheThread::ClearCache( const ttstr &name ) 
{
	if (name != TJS_W("")) {
		TVPClearStorageCache(name);
		CancelLoadQueue(name);
	} else {
		TVPClearAllStorageCache();
		CancelAllQueue();
	}
}

bool tTVPStorageCacheThread::IsLoading(bool fast) const
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	int count;
	bool ret;
	if (fast) {
		count = RequestQueueFast.size(); 
		ret = count  > 0 || loadingFast;
 	} else {
		count = RequestQueue.size(); 
		ret = count  > 0 || loading;
	} 
	return ret;
}