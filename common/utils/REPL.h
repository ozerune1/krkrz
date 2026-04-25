//---------------------------------------------------------------------------
// REPL (Read-Eval-Print Loop) Interface
//---------------------------------------------------------------------------
#pragma once

#include "tjsNative.h"
#include "ThreadIntf.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

class tTVPReplThread : public tTVPThread
{
private:
	// --- request slot (worker → main) ---
	std::mutex req_mtx_;
	ttstr req_script_;
	bool req_pending_ = false;

	// --- response slot (main → worker) ---
	std::mutex resp_mtx_;
	std::condition_variable resp_cv_;
	tTJSVariant resp_result_;
	ttstr resp_error_;
	bool resp_ok_ = false;
	bool resp_ready_ = false;

	// shutdown
	std::atomic<bool> terminating_{false};

public:
	tTVPReplThread();
	~tTVPReplThread();

	// メインスレッドから毎 frame 呼ばれる drain。
	// pending リクエストがあれば 1 件だけ TVPExecuteExpression を実行して
	// 結果をレスポンススロットに詰め、worker を起こす。
	void DrainMain();

	static bool ShouldStartREPL();
	void Shutdown();

protected:
	void Execute();

private:
	// worker 側: 式を提出して結果が返るまで待つ
	bool SubmitAndWait(const ttstr& script, tTJSVariant& outResult, ttstr& outError);

	bool IsCompleteStatement(const std::string& script);
	void PrintWelcome();
};

void TVPCreateREPL();
void TVPDestroyREPL();
void TVPDrainREPL();
