//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Thread base class
//---------------------------------------------------------------------------
#define NOMINMAX
#include "tjsCommHead.h"

#include "ThreadIntf.h"
#include "MsgIntf.h"

#include <algorithm>
#include <assert.h>
#include <system_error>

//---------------------------------------------------------------------------
// tTVPThread : a wrapper class for thread
//---------------------------------------------------------------------------
tTVPThread::tTVPThread()
 : Thread(nullptr), Terminated(false), ThreadStarting(false), ThreadPriority(ttpNormal)
{
	Thread = TVPCreateNativeThread();
}

//---------------------------------------------------------------------------
tTVPThread::~tTVPThread()
{
	delete Thread;
}
//---------------------------------------------------------------------------

void tTVPThread::StartFunc(void *arg)
{
	tTVPThread *thread = (tTVPThread*)arg;
	thread->StartProc();
}

void tTVPThread::StartProc()
{
	{	// スレッドが開始されたのでフラグON
		std::lock_guard<std::mutex> lock( Mtx );
		ThreadStarting = true;
	}
	Cond.notify_all();
	Execute();
	// return 0;
}

//---------------------------------------------------------------------------
void tTVPThread::StartThread()
{
	if (Thread) {
		try {
			Thread->Start(StartFunc, (void*)this, ThreadPriority);
			// スレッドが開始されるのを待つ
			std::unique_lock<std::mutex> lock( Mtx );
			Cond.wait( lock, [this] { return ThreadStarting; } );
		} catch( std::system_error & ) {
			TVPThrowInternalError;
		}
	}

}

//---------------------------------------------------------------------------
void tTVPThread::WaitFor() 
{
	if (Thread) {
		Thread->WaitFor();
	}
}

//---------------------------------------------------------------------------
tTVPThreadPriority tTVPThread::GetPriority()
{
	return ThreadPriority;
}

//---------------------------------------------------------------------------
void tTVPThread::SetPriority(tTVPThreadPriority pri)
{
	ThreadPriority = pri;
	if (!ThreadStarting) {
		return;
	}
	if (Thread) {
		Thread->SetPriority(pri);
	}
}

void
tTVPThread::SetProcessorNo(int no)
{
	if (Thread) {
		Thread->SetProcessorNo(no);
	}
}

#ifdef _WIN32
HANDLE
tTVPThread::GetHandle() const {
	return Thread ? Thread->GetHandle() : nullptr;
}
#endif

//---------------------------------------------------------------------------
tjs_int TVPDrawThreadNum = 1;
//---------------------------------------------------------------------------
extern tjs_int TVPGetProcessorNum( void );

//---------------------------------------------------------------------------
tjs_int TVPGetThreadNum( void )
{
	tjs_int threadNum = TVPDrawThreadNum ? TVPDrawThreadNum : TVPGetProcessorNum();
	threadNum = std::min( threadNum, TVPMaxThreadNum );
	return threadNum;
}
//---------------------------------------------------------------------------
static void TJS_USERENTRY DummyThreadTask( void * ) {}
//---------------------------------------------------------------------------
class DrawThreadPool;
class DrawThread : public tTVPThread {
	std::mutex mtx;
	std::condition_variable cv;
	TVP_THREAD_TASK_FUNC  lpStartAddress;
	TVP_THREAD_PARAM lpParameter;
	DrawThreadPool* parent;
protected:
	virtual void Execute();

public:
	DrawThread( DrawThreadPool* p ) : parent( p ), lpStartAddress( nullptr ), lpParameter( nullptr ) {}
	void SetTask( TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param ) {
		std::lock_guard<std::mutex> lock( mtx );
		lpStartAddress = func;
		lpParameter = param;
		cv.notify_one();
	}
};
//---------------------------------------------------------------------------
class DrawThreadPool {
	std::vector<DrawThread*> workers;
	std::atomic<int> running_thread_count;
	tjs_int task_num;
	tjs_int task_count;
private:
	void PoolThread( tjs_int taskNum );

public:
	DrawThreadPool() : running_thread_count( 0 ), task_num( 0 ), task_count( 0 ) {}
	~DrawThreadPool() {
		for( auto i = workers.begin(); i != workers.end(); ++i ) {
			DrawThread *th = *i;
			th->Terminate();
			th->SetTask( DummyThreadTask, nullptr );
			th->WaitFor();
			delete th;
		}
	}
	inline void DecCount() { running_thread_count--; }
	void BeginTask( tjs_int taskNum ) {
		task_num = taskNum;
		task_count = 0;
		PoolThread( taskNum );
	}
	void ExecTask( TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param ) {
		if( task_count >= task_num - 1 ) {
			func( param );
			return;
		}
		running_thread_count++;
		DrawThread* thread = workers[task_count];
		task_count++;
		thread->SetTask( func, param );
		TVPYieldNativeThread();
	}
	void WaitForTask() {
		int expected = 0;
		while( false == std::atomic_compare_exchange_weak( &running_thread_count, &expected, 0 ) ) {
			expected = 0;
			TVPYieldNativeThread();
		}
	}
};
//---------------------------------------------------------------------------
void DrawThread::Execute() {
	while( !GetTerminated() ) {
		{
			std::unique_lock<std::mutex> uniq_lk( mtx );
			cv.wait( uniq_lk, [this] { return lpStartAddress != nullptr; } );
		}
		if( lpStartAddress != nullptr ) ( lpStartAddress )( lpParameter );
		lpStartAddress = nullptr;
		parent->DecCount();
	}
}
//---------------------------------------------------------------------------
void DrawThreadPool::PoolThread( tjs_int taskNum ) {
	tjs_int extraThreadNum = TVPGetThreadNum();


	// スレッド数がextraThreadNumに達していないので(suspend状態で)生成する
	while( (tjs_int)workers.size() < extraThreadNum ) {
		DrawThread* th = new DrawThread( this );
		th->StartThread();
		// プロセッサ番号指定
		th->SetProcessorNo(workers.size()+1);
		workers.push_back( th );
	}
	// スレッド数が多い場合終了させる
	while( (tjs_int)workers.size() > extraThreadNum ) {
		DrawThread *th = workers.back();
		th->Terminate();
		running_thread_count++;
		th->SetTask( DummyThreadTask, nullptr );
		th->WaitFor();
		workers.pop_back();
		delete th;
	}
}
//---------------------------------------------------------------------------
static DrawThreadPool TVPTheadPool;
//---------------------------------------------------------------------------
void TVPBeginThreadTask( tjs_int taskNum ) {
	TVPTheadPool.BeginTask( taskNum );
}
//---------------------------------------------------------------------------
void TVPExecThreadTask( TVP_THREAD_TASK_FUNC func, TVP_THREAD_PARAM param ) {
	TVPTheadPool.ExecTask( func, param );
}
//---------------------------------------------------------------------------
void TVPEndThreadTask( void ) {
	TVPTheadPool.WaitForTask();
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------


