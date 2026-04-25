//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Debugger hot-path attachment flag
//---------------------------------------------------------------------------
#ifndef tjsDebuggerHookH
#define tjsDebuggerHookH

#include <atomic>
#include "tjsString.h"  // tjs_string が Debugger.h で使われる
#include "Debugger.h"   // DBGHOOK_PREV_* enum, Breakpoints struct

namespace TJS
{

// DAP クライアント接続中のみ true。VM の毎ライン/コール/リターン hook で
// 参照されるホットパスなので inline + relaxed atomic load で公開する。
extern std::atomic<bool> TVPDebuggerAttachedFlag;

inline bool TVPDebuggerWantsHook()
{
	return TVPDebuggerAttachedFlag.load(std::memory_order_relaxed);
}

} // namespace TJS

#endif
