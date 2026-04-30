//---------------------------------------------------------------------------
// LogCore
//
// ログパイプラインのハブ。旧 DebugIntf.cpp にあった以下の状態と処理を集約:
//  - リングバッファ (TVPLogDeque)
//  - important log 文字列キャッシュ (TVPImportantLogs)
//  - ファイル出力 (tTVPLogStreamHolder → krkr.console.log, UTF-16 LE + BOM)
//  - TJS logging handler (TVPAddLoggingHandler / TVPRemoveLoggingHandler)
//  - コンソール sink フック (REPL 連携)
//
// 入口は TVPLogDispatchLine(level, utf8_line)。LogImpl (plog / SDL3) が
// 整形済みの素の本文を UTF-8 で渡してきて、LogCore がタイムスタンプ付与以降
// 下流のすべてを面倒見る。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <string>
#include <time.h>

#include "LogIntf.h"
#include "DebugIntf.h"
#include "CharacterSet.h"
#include "MsgIntf.h"
#include "StorageIntf.h"
#include "SysInitIntf.h"
#include "SysInitImpl.h"
#include "NativeFile.h"

#include "tjsDebug.h"
#include "tjsDebuggerHook.h"

//---------------------------------------------------------------------------
// コンソール sink 保管
//---------------------------------------------------------------------------
static std::atomic<TVPLogConsoleSinkFn> g_console_sink{nullptr};

void TVPLogSetConsoleSink(TVPLogConsoleSinkFn hook)
{
	g_console_sink.store(hook, std::memory_order_release);
}

TVPLogConsoleSinkFn TVPLogGetConsoleSink()
{
	return g_console_sink.load(std::memory_order_acquire);
}

//---------------------------------------------------------------------------
// リングバッファと important log キャッシュ
//---------------------------------------------------------------------------
struct tTVPLogItem
{
	ttstr Log;      // 本文 (タイムスタンプを含まない)
	ttstr Time;     // "HH:MM:SS"
	TVPLogLevel Level;
	tTVPLogItem(const ttstr &log, const ttstr &time, TVPLogLevel lv)
		: Log(log), Time(time), Level(lv) {}
};
static std::deque<tTVPLogItem> *TVPLogDeque = NULL;
tjs_uint TVPLogMaxLines = 2048;

bool TVPAutoLogToFileOnError = true;
bool TVPAutoClearLogOnError = false;
bool TVPLoggingToFile = false;
static tjs_uint TVPLogToFileRollBack = 100;
static ttstr *TVPImportantLogs = NULL;
ttstr TVPLogLocation;
tjs_char TVPNativeLogLocation[MAX_PATH];

static bool TVPLogObjectsInitialized = false;
static void TVPEnsureLogObjects()
{
	if(TVPLogObjectsInitialized) return;
	TVPLogObjectsInitialized = true;
	TVPLogDeque = new std::deque<tTVPLogItem>();
	TVPImportantLogs = new ttstr();
}
static void TVPDestroyLogObjects()
{
	if(TVPLogDeque) { delete TVPLogDeque; TVPLogDeque = NULL; }
	if(TVPImportantLogs) { delete TVPImportantLogs; TVPImportantLogs = NULL; }
}
static tTVPAtExit TVPDestroyLogObjectsAtExit(TVP_ATEXIT_PRI_CLEANUP, TVPDestroyLogObjects);

//---------------------------------------------------------------------------
// TJS logging handler 列
//---------------------------------------------------------------------------
static std::vector<tTJSVariantClosure> TVPLoggingHandlerVector;
static bool TVPInDeliverLoggingEvent = false;

static void TVPCleanupLoggingHandlerVector()
{
	std::vector<tTJSVariantClosure>::iterator i;
	for(i = TVPLoggingHandlerVector.begin(); i != TVPLoggingHandlerVector.end(); )
	{
		if(!i->Object)
		{
			i->Release();
			i = TVPLoggingHandlerVector.erase(i);
		}
		else
		{
			++i;
		}
	}
}

static void TVPDestroyLoggingHandlerVector()
{
	std::vector<tTJSVariantClosure>::iterator i;
	for(i = TVPLoggingHandlerVector.begin(); i != TVPLoggingHandlerVector.end(); ++i)
	{
		i->Release();
	}
	TVPLoggingHandlerVector.clear();
}
static tTVPAtExit TVPDestroyLoggingHandlerAtExit
	(TVP_ATEXIT_PRI_PREPARE, TVPDestroyLoggingHandlerVector);

void TVPAddLoggingHandler(tTJSVariantClosure clo)
{
	std::vector<tTJSVariantClosure>::iterator i;
	i = std::find(TVPLoggingHandlerVector.begin(),
		TVPLoggingHandlerVector.end(), clo);
	if(i == TVPLoggingHandlerVector.end())
	{
		clo.AddRef();
		TVPLoggingHandlerVector.push_back(clo);
	}
}

void TVPRemoveLoggingHandler(tTJSVariantClosure clo)
{
	std::vector<tTJSVariantClosure>::iterator i;
	i = std::find(TVPLoggingHandlerVector.begin(),
		TVPLoggingHandlerVector.end(), clo);
	if(i != TVPLoggingHandlerVector.end())
	{
		i->Release();
		i->Object = i->ObjThis = NULL;
	}
	if(!TVPInDeliverLoggingEvent)
	{
		TVPCleanupLoggingHandlerVector();
	}
}

static void TVPDeliverLoggingEvent(const ttstr &timestampedLine)
{
	if(TVPInDeliverLoggingEvent) return;
	if(TVPLoggingHandlerVector.empty()) return;
	TVPInDeliverLoggingEvent = true;
	try
	{
		bool emptyflag = false;
		tTJSVariant vline(timestampedLine);
		tTJSVariant *pvline[] = { &vline };
		for(tjs_uint i = 0; i < TVPLoggingHandlerVector.size(); i++)
		{
			if(TVPLoggingHandlerVector[i].Object)
			{
				tjs_error er;
				try
				{
					er = TVPLoggingHandlerVector[i].FuncCall(
						0, NULL, NULL, NULL, 1, pvline, NULL);
				}
				catch(...)
				{
					TVPLoggingHandlerVector[i].Release();
					TVPLoggingHandlerVector[i].Object =
					TVPLoggingHandlerVector[i].ObjThis = NULL;
					throw;
				}
				if(TJS_FAILED(er))
				{
					TVPLoggingHandlerVector[i].Release();
					TVPLoggingHandlerVector[i].Object =
					TVPLoggingHandlerVector[i].ObjThis = NULL;
					emptyflag = true;
				}
			}
			else
			{
				emptyflag = true;
			}
		}
		if(emptyflag) TVPCleanupLoggingHandlerVector();
	}
	catch(...)
	{
		TVPInDeliverLoggingEvent = false;
		throw;
	}
	TVPInDeliverLoggingEvent = false;
}

//---------------------------------------------------------------------------
// ファイル出力
//---------------------------------------------------------------------------
class tTVPLogStreamHolder
{
	NativeFile Stream;
	bool Alive;
	bool OpenFailed;

public:
	tTVPLogStreamHolder() { Alive = true; OpenFailed = false; }
	~tTVPLogStreamHolder() { Stream.Close(); Alive = false; }

private:
	void Open(const tjs_char *mode);

public:
	void Clear();
	void Log(const ttstr &text);
	void Reopen() { Stream.Close(); Alive = false; OpenFailed = false; }
} static TVPLogStreamHolder;

static const tjs_char *WDAY[] = {
	TJS_W("Sunday"), TJS_W("Monday"), TJS_W("Tuesday"), TJS_W("Wednesday"),
	TJS_W("Thursday"), TJS_W("Friday"), TJS_W("Saturday")
};
static const tjs_char *MDAY[] = {
	TJS_W("January"), TJS_W("February"), TJS_W("March"), TJS_W("April"),
	TJS_W("May"), TJS_W("June"), TJS_W("July"), TJS_W("August"),
	TJS_W("September"), TJS_W("October"), TJS_W("November"), TJS_W("December")
};

void tTVPLogStreamHolder::Open(const tjs_char *mode)
{
	if(OpenFailed) return;
	try
	{
		tjs_char filename[MAX_PATH];
		if(TVPLogLocation.IsEmpty())
		{
			Stream.Close();
			OpenFailed = true;
		}
		else
		{
			TJS_strcpy(filename, TVPNativeLogLocation);
			TJS_strcat(filename, TJS_W("/krkr.console.log"));
			TVPEnsureDataPathDirectory();
			Stream.Open(filename, mode);
			if(!Stream.IsOpen()) OpenFailed = true;
		}
		if(Stream.IsOpen())
		{
			Stream.Seek(0, SEEK_END);
			if(Stream.Tell() == 0)
			{
				Stream.Write(TJS_N("\xff\xfe"), 2);
			}
#ifdef TJS_TEXT_OUT_CRLF
			ttstr separator(TVPSeparatorCRLF);
#else
			ttstr separator(TVPSeparatorCR);
#endif
			Log(separator);
			static tjs_char timebuf[80];
			{
				time_t timer; timer = time(&timer);
				tm* t = localtime(&timer);
				TJS_snprintf(timebuf, 79,
					TJS_W("%s, %s %02d, %04d %02d:%02d:%02d"),
					WDAY[t->tm_wday], MDAY[t->tm_mon], t->tm_mday,
					t->tm_year+1900, t->tm_hour, t->tm_min, t->tm_sec);
			}
			Log(ttstr(TJS_W("Logging to ")) + ttstr(filename)
				+ TJS_W(" started on ") + timebuf);
		}
	}
	catch(...) { OpenFailed = true; }
}

void tTVPLogStreamHolder::Clear()
{
	Stream.Close();
	Open(TJS_W("wb"));
}

void tTVPLogStreamHolder::Log(const ttstr &text)
{
	if(!Stream.IsOpen()) Open(TJS_W("ab"));
	try
	{
		if(Stream.IsOpen())
		{
			size_t len = text.GetLen() * sizeof(tjs_char);
			if(len != Stream.Write(text.c_str(), len))
			{
				Stream.Close();
				OpenFailed = true;
				return;
			}
#ifdef TJS_TEXT_OUT_CRLF
			Stream.Write(TJS_W("\r\n"), 2 * sizeof(tjs_char));
#else
			Stream.Write(TJS_W("\n"), 1 * sizeof(tjs_char));
#endif
			Stream.Flush();
		}
	}
	catch(...)
	{
		try { Stream.Close(); } catch(...) {}
		OpenFailed = true;
	}
}

//---------------------------------------------------------------------------
// コンソール既定書き出し (sink が無いとき / sink が false を返したとき)
//---------------------------------------------------------------------------
static void TVPLogConsoleDefaultWrite(const ttstr &timestampedLine)
{
#ifdef _WIN32
	HANDLE hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
	if(hStdOutput == INVALID_HANDLE_VALUE) return;
	DWORD mode;
	const tjs_char *mes = timestampedLine.c_str();
	tjs_int len = (tjs_int)timestampedLine.GetLen();
	if(GetConsoleMode(hStdOutput, &mode))
	{
		::WriteConsoleW(hStdOutput, mes, len, NULL, NULL);
		::WriteConsoleW(hStdOutput, L"\r\n", 2, NULL, NULL);
	}
	else
	{
		static std::vector<char> cache(256);
		tjs_int u8len = TVPWideCharToUtf8String(mes, len, nullptr) + 2;
		if((tjs_int)cache.size() < u8len) cache.resize(u8len);
		tjs_int written = TVPWideCharToUtf8String(mes, len, &cache[0]);
		cache[written++] = '\n';
		::WriteFile(hStdOutput, &cache[0], written, NULL, NULL);
	}
#else
	std::string u8;
	TVPUtf16ToUtf8(u8, timestampedLine.AsStdString());
	fwrite(u8.c_str(), 1, u8.size(), stderr);
	fputc('\n', stderr);
	fflush(stderr);
#endif
}

//---------------------------------------------------------------------------
// TVPLogDispatchLine (中央ディスパッチャ)
//---------------------------------------------------------------------------
void TVPLogDispatchLine(TVPLogLevel level, const char *utf8_line)
{
	if(!utf8_line) return;

	TVPEnsureLogObjects();

	// UTF-8 → UTF-16 (以降の処理は全部 ttstr ベース)
	tjs_string wide;
	TVPUtf8ToUtf16(wide, utf8_line);
	// 末尾の改行は LogCore 側で付与するので剥がす
	while(!wide.empty() && (wide.back() == TJS_W('\n') || wide.back() == TJS_W('\r')))
		wide.pop_back();
	ttstr line(wide.c_str());

	// タイムスタンプ (1 秒単位でキャッシュ)
	static time_t prevlogtime = 0;
	static ttstr prevtimebuf;
	static tjs_char timebuf[40];
	time_t timer = time(NULL);
	if(prevlogtime != timer)
	{
		tm *struct_tm = localtime(&timer);
		TJS_snprintf(timebuf, 39, TJS_W("%02d:%02d:%02d"),
			struct_tm->tm_hour, struct_tm->tm_min, struct_tm->tm_sec);
		prevlogtime = timer;
		prevtimebuf = timebuf;
	}

	bool important = (level >= TVPLOG_LEVEL_WARNING);

	// リングバッファ
	if(TVPLogDeque)
	{
		TVPLogDeque->push_back(tTVPLogItem(line, prevtimebuf, level));
		while(TVPLogDeque->size() >= TVPLogMaxLines + 100)
		{
			std::deque<tTVPLogItem>::iterator i = TVPLogDeque->begin();
			TVPLogDeque->erase(i, i + 100);
		}
	}

	// "HH:MM:SS [marker ]本文" 形式に組み立て
	ttstr stamped = prevtimebuf + TJS_W(" ");
	if(important) stamped += TJS_W("! ");
	stamped += line;

	// important log cache
	if(important && TVPImportantLogs)
	{
#ifdef TJS_TEXT_OUT_CRLF
		*TVPImportantLogs += stamped + TJS_W("\r\n");
#else
		*TVPImportantLogs += stamped + TJS_W("\n");
#endif
	}

	// TJS logging handlers
	TVPDeliverLoggingEvent(stamped);

	// debugger 接続中はログを DAP `output` event として転送する (Phase 1: stub)
	if(TJS::TVPDebuggerWantsHook()) TJS::TJSDebuggerLog(stamped, important);

	// ファイル出力
	if(TVPLoggingToFile) TVPLogStreamHolder.Log(stamped);

	// コンソール出力: sink (REPL) 優先、無ければ既定書き出し
	if(auto hook = g_console_sink.load(std::memory_order_acquire))
	{
		// sink には「タイムスタンプ付きの行」を UTF-8 で渡す
		std::string u8;
		TVPUtf16ToUtf8(u8, stamped.AsStdString());
		if(hook(level, u8.c_str())) return;
	}
	TVPLogConsoleDefaultWrite(stamped);
}

//---------------------------------------------------------------------------
// ttstr 版 (旧 TVPAddLog/TVPAddImportantLog 経路) - 内部経由用
//---------------------------------------------------------------------------
static void TVPLogDispatchWide(TVPLogLevel level, const ttstr &line)
{
	std::string u8;
	TVPUtf16ToUtf8(u8, line.AsStdString());
	TVPLogDispatchLine(level, u8.c_str());
}

//---------------------------------------------------------------------------
// TVPAddLog / TVPAddImportantLog
//
// 旧 API。呼び元互換のため残すが、中身は TVPLogMsg 経由に統一。
//---------------------------------------------------------------------------
void TVPAddLog(const ttstr &line)
{
	TVPLogDispatchWide(TVPLOG_LEVEL_INFO, line);
}

void TVPAddImportantLog(const ttstr &line)
{
	TVPLogDispatchWide(TVPLOG_LEVEL_WARNING, line);
}

//---------------------------------------------------------------------------
// 以下は旧 DebugIntf.cpp の公開 API (状態をここに移動したので定義もここ)
//---------------------------------------------------------------------------
ttstr TVPGetImportantLog()
{
	if(!TVPImportantLogs) return ttstr();
	return *TVPImportantLogs;
}

ttstr TVPGetLastLog(tjs_uint n)
{
	TVPEnsureLogObjects();
	if(!TVPLogDeque) return TJS_W("");

	tjs_uint size = (tjs_uint)TVPLogDeque->size();
	if(n > size) n = size;
	if(n == 0) return ttstr();

	tjs_uint len = 0;
	std::deque<tTVPLogItem>::iterator i = TVPLogDeque->end();
	i -= n;
	for(tjs_uint c = 0; c < n; ++c, ++i)
	{
#ifdef TJS_TEXT_OUT_CRLF
		len += i->Time.GetLen() + 1 + i->Log.GetLen() + 2;
#else
		len += i->Time.GetLen() + 1 + i->Log.GetLen() + 1;
#endif
	}

	ttstr buf((tTJSStringBufferLength)len);
	tjs_char *p = buf.Independ();

	i = TVPLogDeque->end();
	i -= n;
	for(tjs_uint c = 0; c < n; ++c, ++i)
	{
		TJS_strcpy(p, i->Time.c_str());
		p += i->Time.GetLen();
		*p++ = TJS_W(' ');
		TJS_strcpy(p, i->Log.c_str());
		p += i->Log.GetLen();
#ifdef TJS_TEXT_OUT_CRLF
		*p++ = TJS_W('\r');
		*p++ = TJS_W('\n');
#else
		*p++ = TJS_W('\n');
#endif
	}
	return buf;
}

void TVPStartLogToFile(bool clear)
{
	TVPEnsureLogObjects();
	if(!TVPImportantLogs) return;
	if(TVPLoggingToFile) return;
	if(clear) TVPLogStreamHolder.Clear();

	// important log を先頭にダンプ
	TVPLogStreamHolder.Log(*TVPImportantLogs);

#ifdef TJS_TEXT_OUT_CRLF
	ttstr separator(TJS_W("\r\n")
		TJS_W("------------------------------------------------------------------------------\r\n"));
#else
	ttstr separator(TJS_W("\n")
		TJS_W("------------------------------------------------------------------------------\n"));
#endif
	TVPLogStreamHolder.Log(separator);

	ttstr content = TVPGetLastLog(TVPLogToFileRollBack);
	TVPLogStreamHolder.Log(content);

	TVPLoggingToFile = true;
}

void TVPSetLogLocation(const ttstr &loc)
{
	TVPLogLocation = TVPNormalizeStorageName(loc);
	if(loc.IsEmpty())
	{
		TVPNativeLogLocation[0] = 0;
		TVPLogLocation.Clear();
	}
	else
	{
		ttstr native = TVPGetLocallyAccessibleName(TVPLogLocation);
		if(native.IsEmpty())
		{
			TVPNativeLogLocation[0] = 0;
			TVPLogLocation.Clear();
		}
		else
		{
			TJS_strcpy(TVPNativeLogLocation, native.AsStdString().c_str());
			if(TVPNativeLogLocation[TJS_strlen(TVPNativeLogLocation)-1] != TJS_W('/'))
				TJS_strcat(TVPNativeLogLocation, TJS_W("/"));
		}
	}

	TVPLogStreamHolder.Reopen();

	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-forcelog"), &val))
	{
		ttstr str(val);
		if(str == TJS_W("yes"))
		{
			TVPLoggingToFile = false;
			TVPStartLogToFile(false);
		}
		else if(str == TJS_W("clear"))
		{
			TVPLoggingToFile = false;
			TVPStartLogToFile(true);
		}
	}
	if(TVPGetCommandLine(TJS_W("-logerror"), &val))
	{
		ttstr str(val);
		if(str == TJS_W("no"))
		{
			TVPAutoLogToFileOnError = false;
		}
		else if(str == TJS_W("clear"))
		{
			TVPAutoClearLogOnError = true;
			TVPAutoLogToFileOnError = true;
		}
	}
}

//---------------------------------------------------------------------------
// TVPOnError はログのファイルフラッシュ専用。旧 DebugIntf にあったが
// ログ系状態がこちらに移ったため一緒に持ってくる。
//---------------------------------------------------------------------------
void TVPOnError()
{
	if(TVPAutoLogToFileOnError) TVPStartLogToFile(TVPAutoClearLogOnError);
}

//---------------------------------------------------------------------------
// tvpfmt::vformat 実装
//
// ログ用途に限定した最小の {} 書式整形器。fmtlib / <format> への依存を
// 完全に断つための自前実装 (LogIntf.h のコメント参照)。
//---------------------------------------------------------------------------
namespace {

// spec = 波括弧の中身 (':' は既に剥がしてある)
// buf には %…形式の printf フォーマット文字列を組み立てる
static void format_one(std::string& out, const tvpfmt::detail::FormatArg& a,
                       const char *spec_begin, const char *spec_end)
{
    using K = tvpfmt::detail::FormatArg;

    bool zero = false;
    int  width = 0;
    char type  = 0;

    const char *p = spec_begin;
    if (p < spec_end && *p == '0') { zero = true; ++p; }
    while (p < spec_end && *p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); ++p; }
    if (p < spec_end) type = *p++;
    // 残りは無視 (精度/アライン等は非対応)

    auto build_int_fmt = [&](char conv) {
        // 例: "%08llx", "%lld", "%5llu"
        std::string f = "%";
        if (zero)      f += '0';
        if (width > 0) { char wb[16]; std::snprintf(wb, sizeof(wb), "%d", width); f += wb; }
        f += "ll";
        f += conv;
        return f;
    };

    char buf[128];

    switch (a.kind) {
    case K::K_S64: {
        char conv = 'd';
        bool is_unsigned = false;
        if      (type == 'x') { conv = 'x'; is_unsigned = true; }
        else if (type == 'X') { conv = 'X'; is_unsigned = true; }
        else if (type == 'o') { conv = 'o'; is_unsigned = true; }
        else if (type == 'u') { conv = 'u'; is_unsigned = true; }
        // type == 0, 'd', 'i' → 10 進符号付き
        std::string f = build_int_fmt(conv);
        if (is_unsigned)
            std::snprintf(buf, sizeof(buf), f.c_str(), static_cast<unsigned long long>(a.i64));
        else
            std::snprintf(buf, sizeof(buf), f.c_str(), static_cast<long long>(a.i64));
        out.append(buf);
        break;
    }
    case K::K_U64:
    case K::K_BOOL: {
        char conv = 'u';
        if      (type == 'x') conv = 'x';
        else if (type == 'X') conv = 'X';
        else if (type == 'o') conv = 'o';
        else if (type == 'd' || type == 'i') conv = 'd';
        std::string f = build_int_fmt(conv);
        if (conv == 'd')
            std::snprintf(buf, sizeof(buf), f.c_str(), static_cast<long long>(a.u64));
        else
            std::snprintf(buf, sizeof(buf), f.c_str(), static_cast<unsigned long long>(a.u64));
        out.append(buf);
        break;
    }
    case K::K_DBL: {
        // {}, {:f}, {:g}, {:e}, {:.Nf} など最低限。精度指定は上記の簡易 parser では
        // 拾えていないので、ここでは常に '%g' 既定 ('f'/'e'/'g'/'G' が明示されたら従う)。
        char conv = 'g';
        if (type == 'f' || type == 'F' || type == 'e' || type == 'E' || type == 'g' || type == 'G')
            conv = type;
        char f[16];
        if (width > 0)
            std::snprintf(f, sizeof(f), "%%%s%d%c", zero ? "0" : "", width, conv);
        else
            std::snprintf(f, sizeof(f), "%%%c", conv);
        std::snprintf(buf, sizeof(buf), f, a.dbl);
        out.append(buf);
        break;
    }
    case K::K_PTR:
        std::snprintf(buf, sizeof(buf), "%p", a.ptr);
        out.append(buf);
        break;
    case K::K_CSTR:
        out.append(a.cstr ? a.cstr : "(null)");
        break;
    case K::K_STR:
        out.append(a.str);
        break;
    case K::K_NONE:
        break;
    }
}

} // namespace

namespace tvpfmt {

std::string vformat(const char *fmt_, const format_args& fa)
{
    std::string out;
    if (!fmt_) return out;

    size_t idx = 0;
    const char *p = fmt_;
    while (*p) {
        char c = *p;
        if (c == '{') {
            if (p[1] == '{') { out.push_back('{'); p += 2; continue; }
            const char *end = std::strchr(p, '}');
            if (!end) throw format_error("tvpfmt: unterminated '{' in format string");
            const char *spec_begin = p + 1;
            if (spec_begin < end && *spec_begin == ':') ++spec_begin;
            // 位置指定 ({0}, {1}) は未対応 — 常に順次割り当て
            if (idx >= fa.args.size())
                throw format_error("tvpfmt: argument index out of range");
            format_one(out, fa.args[idx++], spec_begin, end);
            p = end + 1;
        } else if (c == '}') {
            if (p[1] == '}') { out.push_back('}'); p += 2; continue; }
            throw format_error("tvpfmt: unmatched '}' in format string");
        } else {
            out.push_back(c);
            ++p;
        }
    }
    return out;
}

} // namespace tvpfmt
