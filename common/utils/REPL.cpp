//---------------------------------------------------------------------------
// REPL (Read-Eval-Print Loop) Implementation
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "REPL.h"
#include "ScriptMgnIntf.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include "CharacterSet.h"
#include "LogIntf.h"

// pretty print 設定 (REPL の .depth / .compact コマンドから変更)
static int g_repl_pp_depth = 3;
static bool g_repl_pp_compact = false;

// icline の bbcode 解釈から逃すための簡易エスケープ
// ('[' でタグが始まり、'\' がエスケープ文字)
static std::string BBEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		if (c == '\\' || c == '[') out.push_back('\\');
		out.push_back(c);
	}
	return out;
}

#include "icline.h"

//---------------------------------------------------------------------------
// メインスレッド起床 (Win32 は PostThreadMessage で WaitMessage を割り込む。
// SDL3 版は SDL_AppIterate が連続呼び出しされるため wake 不要)
//---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
static DWORD g_repl_main_tid = 0;
static void ReplWakeMain() {
	if (g_repl_main_tid)
		::PostThreadMessageW(g_repl_main_tid, WM_NULL, 0, 0);
}
static void ReplCaptureMainThread() {
	g_repl_main_tid = ::GetCurrentThreadId();
}
#else
static void ReplWakeMain() {}
static void ReplCaptureMainThread() {}
#endif

//---------------------------------------------------------------------------
// ログ sink: LogImpl 側から整形済み UTF-8 行を受け取り、
// icline の bbcode でレベル別に色付けしてプロンプト上に差し込む。
//---------------------------------------------------------------------------
static bool TVPReplLogSink(TVPLogLevel level, const char *utf8_line)
{
	const char *style = nullptr;
	switch (level) {
		case TVPLOG_LEVEL_VERBOSE:  style = "gray";          break;
		case TVPLOG_LEVEL_DEBUG:    style = "cyan";          break;
		case TVPLOG_LEVEL_INFO:     style = nullptr;         break;
		case TVPLOG_LEVEL_WARNING:  style = "yellow";        break;
		case TVPLOG_LEVEL_ERROR:    style = "red";           break;
		case TVPLOG_LEVEL_CRITICAL: style = "b red";         break;
		default:                    style = nullptr;         break;
	}
	std::string escaped = BBEscape(utf8_line);
	if (style) {
		ic_printf("[%s]%s[/]\n", style, escaped.c_str());
	} else {
		ic_println(escaped.c_str());
	}
	return true;
}

//---------------------------------------------------------------------------
tTVPReplThread::tTVPReplThread()
{
	ReplCaptureMainThread();
	TVPLogSetConsoleSink(TVPReplLogSink);
	StartThread();
}

tTVPReplThread::~tTVPReplThread()
{
	Shutdown();
	WaitFor();
	TVPLogSetConsoleSink(nullptr);
}

void tTVPReplThread::Shutdown()
{
	terminating_.store(true, std::memory_order_release);
	Terminate();
	ic_async_stop();
	resp_cv_.notify_all();
}

void tTVPReplThread::PrintWelcome()
{
	ic_printf("Kirikiri Z Interactive Shell\n");
	ic_printf("Type 'exit' or 'quit' to exit, '.help' for help\n\n");
}

//---------------------------------------------------------------------------
// worker 側: リクエスト提出と応答待ち
//---------------------------------------------------------------------------
bool tTVPReplThread::SubmitAndWait(const ttstr& script, tTJSVariant& outResult, ttstr& outError)
{
	// 1. リクエストスロットに書き込み
	{
		std::lock_guard<std::mutex> lk(req_mtx_);
		req_script_ = script;
		req_pending_ = true;
	}
	// 2. メインスレッドを起こす (Win32 のみ必要)
	ReplWakeMain();

	// 3. 応答または終了指示を待つ
	std::unique_lock<std::mutex> lk(resp_mtx_);
	resp_cv_.wait(lk, [this]{
		return resp_ready_ || terminating_.load(std::memory_order_acquire);
	});

	if (terminating_.load(std::memory_order_acquire)) {
		return false;
	}

	outResult = resp_result_;
	outError = resp_error_;
	bool ok = resp_ok_;
	resp_ready_ = false;
	resp_ok_ = false;
	resp_result_.Clear();
	resp_error_.Clear();
	return ok;
}

//---------------------------------------------------------------------------
// main 側 drain: pending があれば 1 件だけ実行する
//---------------------------------------------------------------------------
void tTVPReplThread::DrainMain()
{
	ttstr script;
	{
		std::lock_guard<std::mutex> lk(req_mtx_);
		if (!req_pending_) return;
		script = req_script_;
		req_pending_ = false;
		req_script_.Clear();
	}

	tTJSVariant result;
	ttstr error;
	bool ok = false;
	try {
		TVPExecuteExpression(script, &result);
		ok = true;
	} catch (eTJSScriptError &e) {
		error = ttstr(TJS_W("Error: ")) + e.GetMessage();
	} catch (eTJS &e) {
		error = ttstr(TJS_W("Error: ")) + e.GetMessage();
	} catch (...) {
		error = ttstr(TJS_W("Unknown error occurred"));
	}

	{
		std::lock_guard<std::mutex> lk(resp_mtx_);
		resp_result_ = result;
		resp_error_ = error;
		resp_ok_ = ok;
		resp_ready_ = true;
	}
	resp_cv_.notify_one();
}

//---------------------------------------------------------------------------
// worker thread 本体
//---------------------------------------------------------------------------
void tTVPReplThread::Execute()
{
	try {
		ic_set_history(".krkrz_history", 500);
		ic_enable_multiline(true);
		ic_enable_color(true);
		ic_enable_history_duplicates(false);
		ic_enable_brace_matching(true);
		ic_enable_brace_insertion(false);

		PrintWelcome();
	} catch (...) {
		return;
	}

	std::string multiline_input;

	while (!GetTerminated()) {

		const char *prompt = multiline_input.empty() ? "krkrz" : "  ...";
		char *line = ic_readline(prompt);

		if (!line) {
			if (GetTerminated()) break;
			printf("\n");
			TVPTerminateAsync(0);
			break;
		}

		std::string input(line);
		ic_free(line);

		if (input.empty() && multiline_input.empty()) continue;

		if (multiline_input.empty()) {
			if (input == "exit" || input == "quit") {
				TVPTerminateAsync(0);
				break;
			}
			if (input == ".help") {
				ic_printf("Available commands:\n");
				ic_printf("  exit, quit       - Exit the REPL\n");
				ic_printf("  .help            - Show this help\n");
				ic_printf("  .clear           - Clear multiline input\n");
				ic_printf("  .depth [N]       - Show/set pretty-print depth (current: %d)\n", g_repl_pp_depth);
				ic_printf("  .compact [on|off]- Show/toggle compact mode (current: %s)\n",
					g_repl_pp_compact ? "on" : "off");
				ic_printf("\nEnter TJS expressions or statements to evaluate.\n");
				continue;
			}
			if (input == ".clear") { multiline_input.clear(); continue; }
			if (input.rfind(".depth", 0) == 0) {
				std::string arg = input.size() > 6 ? input.substr(6) : "";
				while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
				if (arg.empty()) {
					ic_printf("depth = %d\n", g_repl_pp_depth);
				} else {
					int n = atoi(arg.c_str());
					if (n < 0) n = 0;
					g_repl_pp_depth = n;
					ic_printf("depth = %d\n", g_repl_pp_depth);
				}
				continue;
			}
			if (input.rfind(".compact", 0) == 0) {
				std::string arg = input.size() > 8 ? input.substr(8) : "";
				while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
				if (arg.empty()) {
					g_repl_pp_compact = !g_repl_pp_compact;
				} else if (arg == "on" || arg == "true" || arg == "1") {
					g_repl_pp_compact = true;
				} else if (arg == "off" || arg == "false" || arg == "0") {
					g_repl_pp_compact = false;
				}
				ic_printf("compact = %s\n", g_repl_pp_compact ? "on" : "off");
				continue;
			}
		}

		if (!multiline_input.empty()) {
			multiline_input += "\n";
			multiline_input += input;
		} else {
			multiline_input = input;
		}

		if (!IsCompleteStatement(multiline_input)) continue;

		tjs_string script_u16;
		TVPUtf8ToUtf16(script_u16, multiline_input);

		ic_history_add(multiline_input.c_str());

		tTJSVariant result;
		ttstr error;
		bool ok = SubmitAndWait(ttstr(script_u16.c_str()), result, error);

		if (terminating_.load(std::memory_order_acquire)) break;

		if (ok) {
			ttstr resultStr = TVPPrettyPrint(result, g_repl_pp_depth, g_repl_pp_compact);
			std::string resultUTF8;
			TVPUtf16ToUtf8(resultUTF8, resultStr.AsStdString());
			ic_printf("[green]=>[/] %s\n", BBEscape(resultUTF8).c_str());
		} else {
			std::string errorUTF8;
			TVPUtf16ToUtf8(errorUTF8, error.AsStdString());
			ic_printf("[red]%s[/]\n", BBEscape(errorUTF8).c_str());
		}

		multiline_input.clear();
	}
}

bool tTVPReplThread::ShouldStartREPL()
{
	// -repl のみを起動条件とする。
	// 明示的に -repl=no / -repl=off / -repl=false が指定された場合は抑止。
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-repl"), &val)) return false;

	ttstr s(val);
	if (s == TJS_W("no") || s == TJS_W("off") || s == TJS_W("false") || s == TJS_W("0"))
		return false;
	return true;
}

//---------------------------------------------------------------------------
// Windows GUI サブシステムで親プロセスから継承した / 持っていない console に
// きちんと stdio をつなぎ直す。-repl 指定時にだけ呼ばれる。
//---------------------------------------------------------------------------
#ifdef _WIN32
// REPL 起動時に console が無ければ (アプリ起動時の TVPAttachWindowsConsole で
// 親プロセスに attach できなかったケース) 最後の砦として AllocConsole を試みる。
// stdio を CONIN$/CONOUT$ に張り直すのは icline が C stdin で isatty を見るため。
static void ReplEnsureWindowsConsole()
{
	if (::GetConsoleWindow() == NULL) {
		if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
			if (!::AllocConsole()) return;
		}
	}
	FILE *dummy;
	freopen_s(&dummy, "CONIN$",  "r", stdin);
	freopen_s(&dummy, "CONOUT$", "w", stdout);
	freopen_s(&dummy, "CONOUT$", "w", stderr);
}
#else
static void ReplEnsureWindowsConsole() {}
#endif

// Simple bracket/quote balance check to decide if input is complete.
bool tTVPReplThread::IsCompleteStatement(const std::string& script)
{
	int paren = 0, brace = 0, bracket = 0;
	bool in_single = false, in_double = false;
	bool in_line_comment = false, in_block_comment = false;
	bool last_backslash_line = false;
	size_t i = 0;
	const size_t n = script.size();
	while (i < n) {
		char c = script[i];
		char next = (i + 1 < n) ? script[i + 1] : 0;

		if (in_line_comment) {
			if (c == '\n') in_line_comment = false;
			++i; continue;
		}
		if (in_block_comment) {
			if (c == '*' && next == '/') { in_block_comment = false; i += 2; continue; }
			++i; continue;
		}
		if (in_single) {
			if (c == '\\' && next != 0) { i += 2; continue; }
			if (c == '\'') in_single = false;
			++i; continue;
		}
		if (in_double) {
			if (c == '\\' && next != 0) { i += 2; continue; }
			if (c == '"') in_double = false;
			++i; continue;
		}
		if (c == '/' && next == '/') { in_line_comment = true; i += 2; continue; }
		if (c == '/' && next == '*') { in_block_comment = true; i += 2; continue; }
		if (c == '\'') { in_single = true; ++i; continue; }
		if (c == '"')  { in_double = true; ++i; continue; }
		if (c == '(') ++paren;
		else if (c == ')') --paren;
		else if (c == '{') ++brace;
		else if (c == '}') --brace;
		else if (c == '[') ++bracket;
		else if (c == ']') --bracket;
		++i;
	}

	if (n > 0 && script[n - 1] == '\\') last_backslash_line = true;

	if (in_single || in_double || in_block_comment) return false;
	if (paren > 0 || brace > 0 || bracket > 0) return false;
	if (last_backslash_line) return false;
	return true;
}

//---------------------------------------------------------------------------
// Global functions
//---------------------------------------------------------------------------

static tTVPReplThread *TVPScriptREPL = nullptr;

void TVPCreateREPL()
{
	if (TVPScriptREPL) return;
	if (!tTVPReplThread::ShouldStartREPL()) return;
	ReplEnsureWindowsConsole();
	TVPScriptREPL = new tTVPReplThread();
}

void TVPDestroyREPL()
{
	if (TVPScriptREPL) {
		delete TVPScriptREPL;
		TVPScriptREPL = nullptr;
	}
}

void TVPDrainREPL()
{
	if (TVPScriptREPL) TVPScriptREPL->DrainMain();
}
