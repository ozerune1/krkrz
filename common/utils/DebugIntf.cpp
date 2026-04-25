//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Utilities for Debugging
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "CharacterSet.h"

#include <deque>
#include <algorithm>
#include <time.h>
#include "DebugIntf.h"
#include "LogIntf.h"
#include "MsgIntf.h"
#include "StorageIntf.h"
#include "SysInitIntf.h"
#include "SysInitImpl.h"
#include "tjsDebug.h"

#include "Application.h"
#include "NativeFile.h"

//---------------------------------------------------------------------------
// LogCore 側で保持している状態の extern 参照
//---------------------------------------------------------------------------
extern tjs_uint TVPLogMaxLines;
extern ttstr    TVPLogLocation;

#include "tjsUtils.h"

//---------------------------------------------------------------------------
// NOTE: ログ関連状態 (リングバッファ、ファイル出力、important cache、
//       handler 列、TVPAddLog / TVPAddImportantLog / TVPGetLastLog /
//       TVPGetImportantLog / TVPStartLogToFile / TVPSetLogLocation /
//       TVPOnError) は common/utils/LogCore.cpp に移動した。
//---------------------------------------------------------------------------

// tTJSNC_Debug
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_Debug::ClassID = -1;
//---------------------------------------------------------------------------
// TVPPrettyPrint 実装
//---------------------------------------------------------------------------
namespace {

struct PPContext
{
	int remaining_depth;
	bool compact;
	std::vector<iTJSDispatch2*> stack; // 循環検出
};

static ttstr PrettyIndent(int level)
{
	ttstr s;
	for (int i = 0; i < level; ++i) s += TJS_W("  ");
	return s;
}

static ttstr PrettyPrintImpl(const tTJSVariant &v, PPContext &ctx, int indentLevel);

// dict の各エントリを拾って並べるコールバック
struct PPDictCallback : public tTJSDispatch
{
	PPContext *ctx;
	int indentLevel;
	std::vector<std::pair<ttstr, ttstr>> *entries;

	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 /*flag*/, const tjs_char * /*membername*/, tjs_uint32 * /*hint*/,
		tTJSVariant *result, tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 * /*objthis*/) override
	{
		if (numparams < 3) return TJS_E_BADPARAMCOUNT;
		tjs_uint32 flags = (tjs_int)*param[1];
		if (flags & TJS_HIDDENMEMBER) {
			if (result) *result = (tjs_int)1;
			return TJS_S_OK;
		}
		ttstr key = ttstr(TJS_W("\"")) + ttstr(*param[0]).EscapeC() + TJS_W("\"");
		ttstr val = PrettyPrintImpl(*param[2], *ctx, indentLevel);
		entries->emplace_back(std::move(key), std::move(val));
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
};

static bool IsInstanceOfName(iTJSDispatch2 *dsp, const tjs_char *name)
{
	if (!dsp) return false;
	return dsp->IsInstanceOf(0, NULL, NULL, name, dsp) == TJS_S_TRUE;
}

static ttstr PrettyPrintArray(iTJSDispatch2 *dsp, PPContext &ctx, int indentLevel)
{
	tTJSVariant countVar;
	if (TJS_FAILED(dsp->PropGet(0, TJS_W("count"), NULL, &countVar, dsp))) {
		return TJS_W("(Array)");
	}
	tjs_int count = (tjs_int)countVar.AsInteger();
	if (count <= 0) return TJS_W("[]");
	if (ctx.remaining_depth <= 0) return TJS_W("[...]");

	--ctx.remaining_depth;
	std::vector<ttstr> elems;
	elems.reserve(count);
	for (tjs_int i = 0; i < count; ++i) {
		tTJSVariant val;
		if (TJS_FAILED(dsp->PropGetByNum(0, i, &val, dsp))) {
			elems.emplace_back(TJS_W("(?)"));
			continue;
		}
		elems.push_back(PrettyPrintImpl(val, ctx, indentLevel + 1));
	}
	++ctx.remaining_depth;

	ttstr out;
	if (ctx.compact) {
		out = TJS_W("[");
		for (size_t i = 0; i < elems.size(); ++i) {
			if (i) out += TJS_W(", ");
			out += elems[i];
		}
		out += TJS_W("]");
	} else {
		ttstr inner = PrettyIndent(indentLevel + 1);
		ttstr outer = PrettyIndent(indentLevel);
		out = TJS_W("[\n");
		for (size_t i = 0; i < elems.size(); ++i) {
			out += inner;
			out += elems[i];
			if (i + 1 < elems.size()) out += TJS_W(",");
			out += TJS_W("\n");
		}
		out += outer;
		out += TJS_W("]");
	}
	return out;
}

static ttstr PrettyPrintDict(iTJSDispatch2 *dsp, PPContext &ctx, int indentLevel)
{
	if (ctx.remaining_depth <= 0) return TJS_W("%[...]");

	std::vector<std::pair<ttstr, ttstr>> entries;
	--ctx.remaining_depth;

	PPDictCallback cb;
	cb.ctx = &ctx;
	cb.indentLevel = indentLevel + 1;
	cb.entries = &entries;
	tTJSVariantClosure clo(&cb, NULL);
	dsp->EnumMembers(TJS_IGNOREPROP, &clo, dsp);

	++ctx.remaining_depth;

	if (entries.empty()) return TJS_W("%[]");

	ttstr out;
	if (ctx.compact) {
		out = TJS_W("%[");
		for (size_t i = 0; i < entries.size(); ++i) {
			if (i) out += TJS_W(", ");
			out += entries[i].first;
			out += TJS_W(" => ");
			out += entries[i].second;
		}
		out += TJS_W("]");
	} else {
		ttstr inner = PrettyIndent(indentLevel + 1);
		ttstr outer = PrettyIndent(indentLevel);
		out = TJS_W("%[\n");
		for (size_t i = 0; i < entries.size(); ++i) {
			out += inner;
			out += entries[i].first;
			out += TJS_W(" => ");
			out += entries[i].second;
			if (i + 1 < entries.size()) out += TJS_W(",");
			out += TJS_W("\n");
		}
		out += outer;
		out += TJS_W("]");
	}
	return out;
}

static ttstr PrettyPrintObject(const tTJSVariant &v, PPContext &ctx, int indentLevel)
{
	tTJSVariantClosure clo = v.AsObjectClosureNoAddRef();
	iTJSDispatch2 *dsp = clo.SelectObjectNoAddRef();
	if (!dsp) return TJS_W("(null)");

	// 循環検出
	for (auto *p : ctx.stack) {
		if (p == dsp) return TJS_W("(recursion)");
	}

	// Array
	if (IsInstanceOfName(dsp, TJS_W("Array"))) {
		ctx.stack.push_back(dsp);
		ttstr s = PrettyPrintArray(dsp, ctx, indentLevel);
		ctx.stack.pop_back();
		return s;
	}

	// Dictionary
	if (IsInstanceOfName(dsp, TJS_W("Dictionary"))) {
		ctx.stack.push_back(dsp);
		ttstr s = PrettyPrintDict(dsp, ctx, indentLevel);
		ctx.stack.pop_back();
		return s;
	}

	// Function
	if (IsInstanceOfName(dsp, TJS_W("Function"))) {
		return TJS_W("(function)");
	}

	// Class
	if (IsInstanceOfName(dsp, TJS_W("Class"))) {
		return TJS_W("(class)");
	}

	// Property
	if (IsInstanceOfName(dsp, TJS_W("Property"))) {
		return TJS_W("(property)");
	}

	// その他: アドレス付きで表示
	tjs_char buf[64];
	TJS_snprintf(buf, sizeof(buf)/sizeof(buf[0]), TJS_W("(object: 0x%p)"), (void*)dsp);
	return ttstr(buf);
}

static ttstr PrettyPrintImpl(const tTJSVariant &v, PPContext &ctx, int indentLevel)
{
	switch (v.Type()) {
		case tvtVoid:
			return TJS_W("(void)");
		case tvtObject:
			return PrettyPrintObject(v, ctx, indentLevel);
		case tvtString:
		case tvtInteger:
		case tvtReal:
		case tvtOctet:
		default:
			return TJSVariantToExpressionString(v);
	}
}

} // anonymous namespace

ttstr TVPPrettyPrint(const tTJSVariant &variant, int depth, bool compact)
{
	PPContext ctx;
	ctx.remaining_depth = depth;
	ctx.compact = compact;
	return PrettyPrintImpl(variant, ctx, 0);
}
//---------------------------------------------------------------------------

tTJSNC_Debug::tTJSNC_Debug() : tTJSNativeClass(TJS_W("Debug"))
{
	TJS_BEGIN_NATIVE_MEMBERS(Debug)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL_NO_INSTANCE(/*TJS class name*/Debug)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/Debug)
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/message)
{
	if(numparams<1) return TJS_E_BADPARAMCOUNT;

	if(numparams == 1)
	{
		TVPAddLog(*param[0]);
	}
	else
	{
		// display the arguments separated with ", "
		ttstr args;
		for(int i = 0; i<numparams; i++)
		{
			if(i != 0) args += TJS_W(", ");
			args += ttstr(*param[i]);
		}
		TVPAddLog(args);
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/message)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/notice)
{
	if(numparams<1) return TJS_E_BADPARAMCOUNT;

	if(numparams == 1)
	{
		TVPAddImportantLog(*param[0]);
	}
	else
	{
		// display the arguments separated with ", "
		ttstr args;
		for(int i = 0; i<numparams; i++)
		{
			if(i != 0) args += TJS_W(", ");
			args += ttstr(*param[i]);
		}
		TVPAddImportantLog(args);
	}

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/notice)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/startLogToFile)
{
	bool clear = false;

	if(numparams >= 1)
		clear = param[0]->operator bool();

	TVPStartLogToFile(clear);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/startLogToFile)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/logAsError)
{
	TVPOnError();

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/logAsError)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/addLoggingHandler)
{
	// add function to logging handler list

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();

	TVPAddLoggingHandler(clo);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/addLoggingHandler)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/removeLoggingHandler)
{
	// remove function from logging handler list

	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();

	TVPRemoveLoggingHandler(clo);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/removeLoggingHandler)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getLastLog)
{
	tjs_uint lines = TVPLogMaxLines + 100;

	if(numparams >= 1) lines = (tjs_uint)param[0]->AsInteger();

	if(result) *result = TVPGetLastLog(lines);

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getLastLog)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/prettyPrint)
{
	// Debug.prettyPrint(value [, depth = 2 [, compact = false]])
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	int depth = 2;
	bool compact = false;
	if(numparams >= 2) depth = (int)param[1]->AsInteger();
	if(numparams >= 3) compact = param[2]->operator bool();

	if(result) *result = TVPPrettyPrint(*param[0], depth, compact);
	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/prettyPrint)
//---------------------------------------------------------------------------

//-- properies

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(logLocation)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPLogLocation;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPSetLogLocation(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(logLocation)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(logToFileOnError)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPAutoLogToFileOnError;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPAutoLogToFileOnError = param->operator bool();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(logToFileOnError)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(clearLogFileOnError)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = TVPAutoClearLogOnError;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPAutoClearLogOnError = param->operator bool();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(clearLogFileOnError)
//----------------------------------------------------------------------

//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS

	// put version information to DMS
	TVPAddImportantLog(TVPGetVersionInformation());
	TVPAddImportantLog(ttstr(TVPVersionInformation2));
} // end of tTJSNC_Debug::tTJSNC_Debug
//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_Debug::CreateNativeInstance()
{
	return NULL;
}
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_Debug()
{
	tTJSNativeClass *cls = new tTJSNC_Debug();
	return cls;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TJS2 Console Output Gateway
//---------------------------------------------------------------------------
class tTVPTJS2ConsoleOutputGateway : public iTJSConsoleOutput
{
	void ExceptionPrint(const tjs_char *msg)
	{
		TVPAddLog(msg);
	}

	void Print(const tjs_char *msg)
	{
		TVPAddLog(msg);
	}
} static TVPTJS2ConsoleOutputGateway;
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TJS2 Dump Output Gateway
//---------------------------------------------------------------------------
static ttstr TVPDumpOutFileName;
static NativeFile TVPDumpOutFile; // use traditional output routine
//---------------------------------------------------------------------------
class tTVPTJS2DumpOutputGateway : public iTJSConsoleOutput
{
	void ExceptionPrint(const tjs_char *msg) { Print(msg); }

	void Print(const tjs_char *msg)
	{
		if(TVPDumpOutFile.IsOpen())
		{
			TVPDumpOutFile.Write( msg, TJS_strlen(msg)*sizeof(tjs_char) );
#ifdef TJS_TEXT_OUT_CRLF
			TVPDumpOutFile.Write( TJS_W("\r\n"), 2 * sizeof(tjs_char) );
#else
			TVPDumpOutFile.Write( TJS_W("\n"), 1 * sizeof(tjs_char) );
#endif
		}
	}
} static TVPTJS2DumpOutputGateway;
//---------------------------------------------------------------------------
void TVPTJS2StartDump()
{
	tjs_char filename[MAX_PATH];
	TJS_strcpy(filename, Application->ExePath().c_str());
	TJS_strcat(filename, TJS_W(".dump.txt"));
	TVPDumpOutFileName = filename;
	TVPDumpOutFile.Open(filename, TJS_W("wb+"));
	if(TVPDumpOutFile.IsOpen())
	{
		// TODO: 32-bit unicode support
		TVPDumpOutFile.Write( TJS_N("\xff\xfe"), 2 ); // indicate unicode text
	}
}
//---------------------------------------------------------------------------
void TVPTJS2EndDump()
{
	if(TVPDumpOutFile.IsOpen())
	{
		TVPDumpOutFile.Close();
		TVPAddLog(ttstr(TJS_W("Dumped to ")) + TVPDumpOutFileName);
	}
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// console interface retrieving functions
//---------------------------------------------------------------------------
iTJSConsoleOutput *TVPGetTJS2ConsoleOutputGateway()
{
	return & TVPTJS2ConsoleOutputGateway;
}
//---------------------------------------------------------------------------
iTJSConsoleOutput *TVPGetTJS2DumpOutputGateway()
{
	return & TVPTJS2DumpOutputGateway;
}
//---------------------------------------------------------------------------

/*
//---------------------------------------------------------------------------
// on-error hook
//---------------------------------------------------------------------------
void TVPOnErrorHook()
{
	if(TVPMainForm) TVPMainForm->NotifySystemError();
}
//---------------------------------------------------------------------------
*/
