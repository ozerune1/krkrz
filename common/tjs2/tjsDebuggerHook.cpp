//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Debugger hook entry points.
// VM (tjsInterCodeExec.cpp) と LogCore から呼ばれ、DebuggerCore に転送する。
// KRKRZ_ENABLE_DAP=OFF の場合は no-op。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "tjsDebuggerHook.h"
#include "tjsDebug.h"

#ifdef KRKRZ_ENABLE_DAP
#include "tjsDebuggerCore.h"
#endif

namespace TJS
{

std::atomic<bool> TVPDebuggerAttachedFlag{false};

void TJSDebuggerHook(tjs_int evtype, const tjs_char *filename,
                     tjs_int lineno, tTJSInterCodeContext *ctx,
                     tTJSVariant *exception)
{
#ifdef KRKRZ_ENABLE_DAP
	if (auto* core = DebuggerCore::Instance()) {
		core->OnHook(evtype, filename, lineno, ctx, exception);
	}
#else
	(void)evtype; (void)filename; (void)lineno; (void)ctx; (void)exception;
#endif
}

void TJSDebuggerLog(const ttstr &line, bool important)
{
#ifdef KRKRZ_ENABLE_DAP
	if (auto* core = DebuggerCore::Instance()) {
		core->OnLog(line, important);
	}
#else
	(void)line; (void)important;
#endif
}

} // namespace TJS
