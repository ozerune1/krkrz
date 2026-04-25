//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// DAP DebuggerCore (Phase 2 minimal).
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "tjsDebuggerCore.h"

#ifdef KRKRZ_ENABLE_DAP

#include "tjsDebuggerHook.h"
#include "tjsInterCodeGen.h"
#include "tjsScriptBlock.h"
#include "tjsObject.h"     // tTJSDispatch (member 列挙コールバック基底)
#include "tjsDictionary.h" // TJSCreateDictionaryObject
#include "tjs.h"
#include "DAPServer.h"
#include "CharacterSet.h"
#include "LogIntf.h"
#include "SysInitIntf.h"   // TVPGetCommandLine
#include "ScriptMgnIntf.h" // TVPExecuteExpression
#include "DebugIntf.h"     // TVPPrettyPrint
#include "StorageIntf.h"   // TVPNormalizeStorageName, TVPGetLocallyAccessibleName

#ifdef KRKRZ_USE_REPL
#include "REPL.h"          // TVPDrainREPL: 停止中も REPL を回す
#endif

#include <chrono>

namespace TJS {

DebuggerCore* DebuggerCore::instance_ = nullptr;

//---------------------------------------------------------------------------
// パス正規化: lowercase + 区切りを '/' に + "file://" prefix を除去。
// VSCode は絶対パス (例: "C:/proj/data/scene.tjs") を、TJS は内部パス (例:
// "file://./scene.tjs" や "scene.tjs") を渡してくるため、両者を共通化して
// suffix マッチで突き合わせる。
//---------------------------------------------------------------------------
static tjs_string NormalizePath(const tjs_string& path)
{
	tjs_string s = path;
	// "file://" prefix を除去 (Windows の "file:///C:/..." の追加 '/' も)
	static const tjs_string file_prefix(TJS_W("file://"));
	if (s.size() >= file_prefix.size() &&
		s.compare(0, file_prefix.size(), file_prefix) == 0) {
		s = s.substr(file_prefix.size());
		if (s.size() >= 3 && s[0] == TJS_W('/') && s[2] == TJS_W(':')) {
			// "/C:/..." → "C:/..."
			s = s.substr(1);
		}
	}
	for (auto& c : s) {
		if (c == TJS_W('\\')) c = TJS_W('/');
		else if (c >= TJS_W('A') && c <= TJS_W('Z'))
			c = (tjs_char)(c - TJS_W('A') + TJS_W('a'));
	}
	return s;
}

// 完全一致 OR 一方が他方の path 接尾辞 ('/' 境界付き) なら true。
// 例: "c:/proj/data/scene.tjs" と "data/scene.tjs" → true
//     "c:/proj/data/scene.tjs" と "ene.tjs"        → false (境界なし)
static bool PathsMatch(const tjs_string& a, const tjs_string& b)
{
	if (a == b)              return true;
	if (a.empty() || b.empty()) return false;

	const tjs_string& longer  = (a.size() >= b.size()) ? a : b;
	const tjs_string& shorter = (a.size() >= b.size()) ? b : a;
	size_t off = longer.size() - shorter.size();
	if (longer.compare(off, shorter.size(), shorter) != 0) return false;
	if (off == 0) return true;
	return longer[off - 1] == TJS_W('/');
}

//---------------------------------------------------------------------------
// 起動 / 停止
//---------------------------------------------------------------------------
void DebuggerCore::Init(int port)
{
	if (instance_) return;
	instance_ = new DebuggerCore(port);
}

void DebuggerCore::Shutdown()
{
	if (instance_) {
		delete instance_;
		instance_ = nullptr;
	}
}

DebuggerCore::DebuggerCore(int port)
	: port_(port)
{
	// マルチフレーム stack trace のために StackTracer を有効化
	// (これがないと CALL/RETURN hook で frame が記録されない)
	TJSAddRefStackTracer();
	server_.reset(new tTVPDAPServerThread(port, [this] { OnServerMessage(); }));
}

DebuggerCore::~DebuggerCore()
{
	terminating_.store(true);
	pause_cv_.notify_all();
	server_.reset();
	TVPDebuggerAttachedFlag.store(false, std::memory_order_release);
	TJSReleaseStackTracer();
}

//---------------------------------------------------------------------------
// server worker から呼ばれる: pause_cv を起こす
//---------------------------------------------------------------------------
void DebuggerCore::OnServerMessage()
{
	std::lock_guard<std::mutex> lk(pause_mtx_);
	pause_cv_.notify_one();
}

//---------------------------------------------------------------------------
// VM hook 受信
//---------------------------------------------------------------------------
void DebuggerCore::OnHook(tjs_int evtype, const tjs_char* filename,
						   tjs_int lineno, tTJSInterCodeContext* ctx,
						   tTJSVariant* exception)
{
	if (!server_ || !server_->IsClientConnected()) return;

	// 停止中 REPL 評価 / DAP evaluate などで再帰的に VM が動いている時は
	// step tracking や BP 判定を全て skip する。
	if (in_repl_eval_) return;

	// Step depth tracking は filename != nullptr の制約なしに更新する
	if (evtype == DBGHOOK_PREV_CALL) {
		step_depth_++;
		return;
	}
	if (evtype == DBGHOOK_PREV_RETURN) {
		step_depth_--;
		return;
	}

	if (filename == nullptr) return;

	bool should_stop = false;
	std::string reason;

	if (evtype == DBGHOOK_PREV_BREAK) {
		// TJS の `debugger;` 文
		should_stop = true;
		reason = "breakpoint";
	} else if (evtype == DBGHOOK_PREV_EXE_LINE) {
		ExecMode mode = exec_mode_.load();
		switch (mode) {
		case EXEC_PAUSE:
			should_stop = true;
			reason = "pause";
			exec_mode_.store(EXEC_RUN);
			break;
		case EXEC_STEP_IN:
			should_stop = true;
			reason = "step";
			exec_mode_.store(EXEC_RUN);
			break;
		case EXEC_STEP_OVER:
			if (step_depth_ <= step_baseline_depth_) {
				should_stop = true;
				reason = "step";
				exec_mode_.store(EXEC_RUN);
			}
			break;
		case EXEC_STEP_OUT:
			if (step_depth_ < step_baseline_depth_) {
				should_stop = true;
				reason = "step";
				exec_mode_.store(EXEC_RUN);
			}
			break;
		case EXEC_RUN:
		default:
			break;
		}
		// step / pause で停止しなければ BP 判定 (BP は step より優先度低)
		if (!should_stop) {
			tjs_string norm_hook = NormalizePath(tjs_string(filename));
			// 全 BP エントリと suffix match (BP のあるファイルは普通数個なので O(N) で十分)
			for (auto& kv : breakpoints_.BreakPoint) {
				if (!PathsMatch(norm_hook, kv.first)) continue;
				const BPMeta* meta = kv.second.GetMeta((int)lineno);
				if (!meta) continue;

				// log point: 停止せず output event を発火
				// current_ctx_ はまだ更新されていないので、現 hook の ctx を渡す
				if (!meta->logMessage.empty()) {
					EmitLogPoint(meta->logMessage, ctx);
					break;
				}
				// 条件付き BP: 評価して truthy のときだけ停止
				if (!meta->condition.empty() && !EvalBPCondition(meta->condition, ctx)) {
					break;
				}
				should_stop = true;
				reason = "breakpoint";
				break;
			}
		}
	} else if (evtype == DBGHOOK_PREV_EXCEPT) {
		if (break_on_uncaught_exception_) {
			should_stop = true;
			reason = "exception";
		}
	}

	if (should_stop) {
		// 停止前に変数 ref を破棄 (DAP 仕様: stop 毎に variablesReference は invalidate)
		ClearVarRefs();
		current_ctx_ = ctx;
		current_filename_ = filename ? tjs_string(filename) : tjs_string();
		current_lineno_ = (int)lineno;
		// 例外停止時は値を保存 (HandleExceptionInfo の応答に使う)
		if (evtype == DBGHOOK_PREV_EXCEPT && exception) {
			current_exception_     = *exception;
			has_current_exception_ = true;
		} else {
			has_current_exception_ = false;
			current_exception_.Clear();
		}
		BlockUntilResume(ctx, reason);
	}
}

void DebuggerCore::OnLog(const ttstr& line, bool important)
{
	if (!server_ || !server_->IsClientConnected()) return;

	std::string utf8;
	TVPUtf16ToUtf8(utf8, line.AsStdString());

	picojson::object body;
	body["category"] = picojson::value(important ? std::string("stderr")
	                                              : std::string("console"));
	body["output"]   = picojson::value(utf8 + "\n");
	SendEvent("output", body);
}

//---------------------------------------------------------------------------
// メインループからの drain (停止中でなければ呼ばれる)
//---------------------------------------------------------------------------
void DebuggerCore::DrainMain()
{
	if (!server_) return;
	picojson::value v;
	while (server_->TryPopMessage(v)) {
		ProcessRequest(v);
	}
}

//---------------------------------------------------------------------------
// 1 リクエスト dispatch
//---------------------------------------------------------------------------
void DebuggerCore::ProcessRequest(const picojson::value& req)
{
	if (!req.is<picojson::object>()) return;
	const auto& obj = req.get<picojson::object>();
	auto it = obj.find("command");
	if (it == obj.end() || !it->second.is<std::string>()) return;
	const std::string& cmd = it->second.get<std::string>();

	if      (cmd == "initialize")              HandleInitialize(req);
	else if (cmd == "attach")                  HandleAttach(req);
	else if (cmd == "launch")                  HandleAttach(req); // 同等扱い (アプリは既起動)
	else if (cmd == "setBreakpoints")          HandleSetBreakpoints(req);
	else if (cmd == "setExceptionBreakpoints") HandleSetExceptionBreakpoints(req);
	else if (cmd == "configurationDone")       HandleConfigurationDone(req);
	else if (cmd == "threads")                 HandleThreads(req);
	else if (cmd == "stackTrace")              HandleStackTrace(req);
	else if (cmd == "scopes")                  HandleScopes(req);
	else if (cmd == "variables")               HandleVariables(req);
	else if (cmd == "evaluate")                HandleEvaluate(req);
	else if (cmd == "continue")                HandleContinue(req);
	else if (cmd == "pause")                   HandlePause(req);
	else if (cmd == "exceptionInfo")           HandleExceptionInfo(req);
	else if (cmd == "next")                    HandleNext(req);
	else if (cmd == "stepIn")                  HandleStepIn(req);
	else if (cmd == "stepOut")                 HandleStepOut(req);
	else if (cmd == "disconnect")              HandleDisconnect(req);
	else {
		// 未対応 command は success=false で返す
		SendResponse(req, false, {}, std::string("unsupported command: ") + cmd);
	}
}

//---------------------------------------------------------------------------
void DebuggerCore::HandleInitialize(const picojson::value& req)
{
	picojson::object caps;
	caps["supportsConfigurationDoneRequest"]   = picojson::value(true);
	caps["supportsConditionalBreakpoints"]     = picojson::value(true);
	caps["supportsHitConditionalBreakpoints"]  = picojson::value(false);
	caps["supportsLogPoints"]                  = picojson::value(true);
	caps["supportsEvaluateForHovers"]          = picojson::value(true);
	caps["supportsStepBack"]                   = picojson::value(false);
	caps["supportsSetVariable"]                = picojson::value(false);
	caps["supportsRestartFrame"]               = picojson::value(false);
	caps["supportsRestartRequest"]             = picojson::value(false);
	caps["supportsExceptionInfoRequest"]       = picojson::value(true);
	caps["supportsTerminateRequest"]           = picojson::value(false);
	caps["supportTerminateDebuggee"]           = picojson::value(true);

	// 例外ブレークフィルタ広告: VSCode の Breakpoints パネルに表示される
	picojson::array exc_filters;
	{
		picojson::object f;
		f["filter"]  = picojson::value(std::string("uncaught"));
		f["label"]   = picojson::value(std::string("Uncaught Exceptions"));
		f["default"] = picojson::value(true);
		exc_filters.push_back(picojson::value(f));
	}
	caps["exceptionBreakpointFilters"] = picojson::value(exc_filters);

	SendResponse(req, true, caps);

	// 仕様: initialize response の後に initialized event を送る
	SendEvent("initialized");
}

void DebuggerCore::HandleAttach(const picojson::value& req)
{
	// 接続を有効化: VM hook を発火させ始める
	TVPDebuggerAttachedFlag.store(true, std::memory_order_release);

	// stopOnEntry: 最初の hook で停止する (launch/attach 共通)
	const auto& obj = req.get<picojson::object>();
	auto args_it = obj.find("arguments");
	if (args_it != obj.end() && args_it->second.is<picojson::object>()) {
		auto stop_it = args_it->second.get<picojson::object>().find("stopOnEntry");
		if (stop_it != args_it->second.get<picojson::object>().end()
			&& stop_it->second.is<bool>()
			&& stop_it->second.get<bool>()) {
			exec_mode_.store(EXEC_PAUSE);
		}
	}

	SendResponse(req, true);
}

//---------------------------------------------------------------------------
void DebuggerCore::HandleSetBreakpoints(const picojson::value& req)
{
	picojson::object resp_body;
	picojson::array  bp_results;

	const auto& obj = req.get<picojson::object>();
	auto args_it = obj.find("arguments");
	if (args_it != obj.end() && args_it->second.is<picojson::object>()) {
		const auto& args = args_it->second.get<picojson::object>();

		// source.path 取得
		tjs_string filename;
		auto src_it = args.find("source");
		if (src_it != args.end() && src_it->second.is<picojson::object>()) {
			const auto& src = src_it->second.get<picojson::object>();
			auto path_it = src.find("path");
			if (path_it != src.end() && path_it->second.is<std::string>()) {
				const std::string& path_utf8 = path_it->second.get<std::string>();
				TVPUtf8ToUtf16(filename, path_utf8);
			}
		}
		tjs_string key = NormalizePath(filename);

		// 既存 BP を削除して再登録 (DAP 仕様: setBreakpoints は当該ファイルの全 BP を上書き)
		BreakpointLine* lines = breakpoints_.GetBreakPointLines(key);
		if (lines) lines->Lines.clear();

		auto bps_it = args.find("breakpoints");
		if (bps_it != args.end() && bps_it->second.is<picojson::array>()) {
			for (const auto& v : bps_it->second.get<picojson::array>()) {
				if (!v.is<picojson::object>()) continue;
				const auto& bp = v.get<picojson::object>();
				auto line_it = bp.find("line");
				if (line_it == bp.end() || !line_it->second.is<int64_t>()) continue;
				int line_no = (int)line_it->second.get<int64_t>();

				BPMeta meta;
				auto cond_it = bp.find("condition");
				if (cond_it != bp.end() && cond_it->second.is<std::string>()) {
					tjs_string s;
					TVPUtf8ToUtf16(s, cond_it->second.get<std::string>());
					meta.condition = s;
				}
				auto log_it = bp.find("logMessage");
				if (log_it != bp.end() && log_it->second.is<std::string>()) {
					tjs_string s;
					TVPUtf8ToUtf16(s, log_it->second.get<std::string>());
					meta.logMessage = s;
				}

				breakpoints_.SetBreakPoint(key, line_no, meta);

				picojson::object result;
				result["verified"] = picojson::value(true);
				result["line"]     = picojson::value((int64_t)line_no);
				bp_results.push_back(picojson::value(result));
			}
		}
	}

	resp_body["breakpoints"] = picojson::value(bp_results);
	SendResponse(req, true, resp_body);
}

void DebuggerCore::HandleSetExceptionBreakpoints(const picojson::value& req)
{
	// Phase 2 はフィルタ ID "uncaught" が含まれているかだけ見る
	break_on_uncaught_exception_ = false;
	const auto& obj = req.get<picojson::object>();
	auto args_it = obj.find("arguments");
	if (args_it != obj.end() && args_it->second.is<picojson::object>()) {
		auto fil_it = args_it->second.get<picojson::object>().find("filters");
		if (fil_it != args_it->second.get<picojson::object>().end() && fil_it->second.is<picojson::array>()) {
			for (const auto& v : fil_it->second.get<picojson::array>()) {
				if (v.is<std::string>() && v.get<std::string>() == "uncaught") {
					break_on_uncaught_exception_ = true;
					break;
				}
			}
		}
	}
	SendResponse(req, true);
}

void DebuggerCore::HandleConfigurationDone(const picojson::value& req)
{
	configuration_done_.store(true);
	SendResponse(req, true);
}

void DebuggerCore::HandleThreads(const picojson::value& req)
{
	picojson::object thread;
	thread["id"]   = picojson::value((int64_t)1);
	thread["name"] = picojson::value(std::string("main"));
	picojson::array threads;
	threads.push_back(picojson::value(thread));
	picojson::object body;
	body["threads"] = picojson::value(threads);
	SendResponse(req, true, body);
}

void DebuggerCore::HandleStackTrace(const picojson::value& req)
{
	// StackTracer から最新の frame 一覧を取得して current_frames_ に保存。
	// frame ID は 1..N (top=1)。VSCode はこの ID を scopes(frameId) に使う。
	std::vector<TJSStackFrame> raw;
	TJSGetStackTraceFrames(raw);

	current_frames_.clear();
	if (raw.empty() && current_ctx_ && !current_filename_.empty()) {
		// StackTracer 無効 / 空の場合は現停止位置で 1 frame を作る
		FrameInfo fi;
		fi.ctx      = current_ctx_;
		fi.filename = current_filename_;
		fi.line     = current_lineno_;
		ttstr cls = current_ctx_->GetClassName();
		const tjs_char* fname = current_ctx_->GetName();
		ttstr disp;
		if (!cls.IsEmpty()) {
			disp = cls + ttstr(TJS_W("."));
			disp += fname ? ttstr(fname) : ttstr(TJS_W("(anon)"));
		} else if (fname) disp = ttstr(fname);
		else              disp = ttstr(TJS_W("(top)"));
		fi.funcname = disp.AsStdString();
		current_frames_.push_back(std::move(fi));
	} else {
		current_frames_.reserve(raw.size());
		for (auto& f : raw) {
			FrameInfo fi;
			fi.ctx      = f.ctx;
			fi.filename = f.filename;
			fi.funcname = f.funcname;
			fi.line     = f.line;
			current_frames_.push_back(std::move(fi));
		}
		// top frame の line/filename は OnHook で渡された値の方が正確
		// (StackTracer の codePtr は VM ループの 1 命令前を指してることがあるため)
		if (!current_frames_.empty() && current_lineno_ > 0) {
			current_frames_[0].line = current_lineno_;
			if (!current_filename_.empty())
				current_frames_[0].filename = current_filename_;
		}
	}

	picojson::array frames;
	for (size_t i = 0; i < current_frames_.size(); ++i) {
		const auto& fi = current_frames_[i];
		picojson::object frame;
		frame["id"] = picojson::value((int64_t)(i + 1));

		std::string funcname_utf8;
		TVPUtf16ToUtf8(funcname_utf8, fi.funcname);
		frame["name"] = picojson::value(funcname_utf8);

		// TJS 内部パス (例: "file://./scene.tjs") → OS のローカルファイルパス
		// (例: "D:\path\to\data\scene.tjs") に変換することで VSCode が Call
		// Stack の行クリックでソースを開けるようにする。XP3 archive 内など
		// ローカルアクセス不能な場合は元のパスをフォールバックとして使う。
		ttstr os_path;
		try {
			ttstr canonical = TVPNormalizeStorageName(ttstr(fi.filename.c_str()));
			os_path = TVPGetLocallyAccessibleName(canonical);
		} catch (...) {
			// ignore — fall through to original
		}
		std::string path_utf8;
		if (!os_path.IsEmpty()) {
			TVPUtf16ToUtf8(path_utf8, os_path.AsStdString());
		} else {
			TVPUtf16ToUtf8(path_utf8, fi.filename);
		}
		picojson::object src;
		src["path"] = picojson::value(path_utf8);
		src["name"] = picojson::value(path_utf8);
		frame["source"] = picojson::value(src);
		frame["line"]   = picojson::value((int64_t)(fi.line > 0 ? fi.line : 0));
		frame["column"] = picojson::value((int64_t)0);

		frames.push_back(picojson::value(frame));
	}

	picojson::object body;
	body["stackFrames"] = picojson::value(frames);
	body["totalFrames"] = picojson::value((int64_t)frames.size());
	SendResponse(req, true, body);
}

//---------------------------------------------------------------------------
// 条件付き BP / log point (Phase 5b)
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// 評価式 context 用カスタム iTJSDispatch2。
// 以下の resolve 順序を実現する:
//   1. 注入された local 変数 → 該当
//   2. 元の `this` (objthis) のメンバ → 該当
//   3. どちらもなければ TJS_E_MEMBERNOTFOUND を返して proxy を発火させ、
//      TJS engine 側の global fallback に流す。
//
// Dictionary を context に渡す方式だと missing member で TJS_S_OK + void が
// 返るため proxy fallback が起きず global 解決もできなくなる。これを回避する
// ためにカスタム dispatch が必要。
//---------------------------------------------------------------------------
class EvalContextDispatch : public tTJSDispatch
{
public:
	std::vector<std::pair<tjs_string, tTJSVariant>> locals;
	iTJSDispatch2* this_obj = nullptr;  // borrowed; AddRef はしない

	tjs_error TJS_INTF_METHOD PropGet(
		tjs_uint32 flag, const tjs_char* membername, tjs_uint32* hint,
		tTJSVariant* result, iTJSDispatch2* /*objthis*/) override
	{
		if (!membername) return TJS_E_NOTIMPL;
		// 1. local 変数を検索
		for (auto& kv : locals) {
			if (kv.first == membername) {
				if (result) *result = kv.second;
				return TJS_S_OK;
			}
		}
		// 2. 元の this のメンバを参照
		if (this_obj) {
			tjs_error hr = this_obj->PropGet(flag, membername, hint, result, this_obj);
			if (hr != TJS_E_MEMBERNOTFOUND) return hr;
		}
		// 3. どちらもなし → MEMBERNOTFOUND を返して proxy → global fallback
		return TJS_E_MEMBERNOTFOUND;
	}
};

// frame_ctx の local + 元の this を含む dispatch を作って返す。
// 戻り値は Release() 必要 (heap 確保)。null なら何も注入できなかった。
iTJSDispatch2* DebuggerCore::BuildEvalContext(tTJSInterCodeContext* frame_ctx)
{
	if (!frame_ctx) return nullptr;
	auto* d = new EvalContextDispatch();

	auto* ra = frame_ctx->GetDebuggerRegisterArea();
	if (ra) {
		// local 変数を注入
		std::vector<TJSDebuggerVar> locals;
		TJSDebuggerGetLocalVariables(frame_ctx->GetDebuggerScopeKey(), ra, locals);
		for (auto& v : locals) {
			d->locals.emplace_back(v.name, v.value);
		}
		// 元の this を保存 (frame の objthis は ra[-1])
		tTJSVariant& this_var = TJS_GET_VM_REG(ra, TJS_TO_VM_REG_ADDR(-1));
		if (this_var.Type() == tvtObject) {
			d->this_obj = this_var.AsObjectNoAddRef();
		}
	}
	return d;
}

bool DebuggerCore::EvalBPCondition(const tjs_string& expr, tTJSInterCodeContext* ctx)
{
	// 現 frame の local 変数を context に注入して評価する。
	// bare な i は this.i (= injected local) に解決される。
	iTJSDispatch2* ctx_dict = BuildEvalContext(ctx);
	tTJSVariant result;
	bool ok = false;
	try {
		TVPExecuteExpression(ttstr(expr.c_str()), ctx_dict, &result);
		ok = result.AsInteger() != 0; // TJS truthy
	} catch (...) {
		ok = false; // 評価失敗時は停止しない
	}
	if (ctx_dict) ctx_dict->Release();
	return ok;
}

void DebuggerCore::EmitLogPoint(const tjs_string& msg, tTJSInterCodeContext* ctx)
{
	// 1 回だけ context dict を作って {expr} 全てに使い回す
	iTJSDispatch2* ctx_dict = BuildEvalContext(ctx);

	// {expr} 形式の埋め込み式を置換しながら組み立てる
	ttstr out;
	size_t i = 0;
	const size_t n = msg.size();
	while (i < n) {
		size_t lb = msg.find(TJS_W('{'), i);
		if (lb == tjs_string::npos) {
			out += ttstr(msg.substr(i).c_str());
			break;
		}
		if (lb > i) out += ttstr(msg.substr(i, lb - i).c_str());
		size_t rb = msg.find(TJS_W('}'), lb + 1);
		if (rb == tjs_string::npos) {
			// unmatched '{': そのまま残して打ち切り
			out += ttstr(msg.substr(lb).c_str());
			break;
		}
		tjs_string expr = msg.substr(lb + 1, rb - lb - 1);
		try {
			tTJSVariant r;
			TVPExecuteExpression(ttstr(expr.c_str()), ctx_dict, &r);
			out += TVPPrettyPrint(r, 1, true);
		} catch (...) {
			out += ttstr(TJS_W("(err)"));
		}
		i = rb + 1;
	}

	if (ctx_dict) ctx_dict->Release();

	std::string utf8;
	TVPUtf16ToUtf8(utf8, out.AsStdString());
	picojson::object body;
	body["category"] = picojson::value(std::string("console"));
	body["output"]   = picojson::value(utf8 + "\n");
	SendEvent("output", body);
}

//---------------------------------------------------------------------------
// Phase 3: scopes / variables / evaluate
//---------------------------------------------------------------------------
void DebuggerCore::HandleScopes(const picojson::value& req)
{
	int64_t frame_id = 1;
	const auto& obj = req.get<picojson::object>();
	auto args_it = obj.find("arguments");
	if (args_it != obj.end() && args_it->second.is<picojson::object>()) {
		auto fi_it = args_it->second.get<picojson::object>().find("frameId");
		if (fi_it != args_it->second.get<picojson::object>().end() && fi_it->second.is<int64_t>()) {
			frame_id = fi_it->second.get<int64_t>();
		}
	}

	picojson::array scopes;

	// frame_id は 1-based。範囲外なら Globals だけ返す (それでも有用)。
	size_t idx = (size_t)(frame_id - 1);
	tTJSInterCodeContext* ctx =
		(idx < current_frames_.size()) ? current_frames_[idx].ctx : nullptr;

	if (ctx) {
		int64_t locals_ref = RegisterScope(VarRefEntry::FRAME_LOCALS, ctx);
		picojson::object locals;
		locals["name"]               = picojson::value(std::string("Locals"));
		locals["variablesReference"] = picojson::value(locals_ref);
		locals["expensive"]          = picojson::value(false);
		scopes.push_back(picojson::value(locals));

		int64_t this_ref = RegisterScope(VarRefEntry::FRAME_THIS, ctx);
		picojson::object self;
		self["name"]               = picojson::value(std::string("this"));
		self["variablesReference"] = picojson::value(this_ref);
		self["expensive"]          = picojson::value(false);
		scopes.push_back(picojson::value(self));
	}

	// Globals は frame に依存しないので常に出す
	int64_t globals_ref = RegisterScope(VarRefEntry::FRAME_GLOBALS, nullptr);
	picojson::object glob;
	glob["name"]               = picojson::value(std::string("Globals"));
	glob["variablesReference"] = picojson::value(globals_ref);
	glob["expensive"]          = picojson::value(true); // 数百〜千個のメンバが返る可能性
	scopes.push_back(picojson::value(glob));

	picojson::object body;
	body["scopes"] = picojson::value(scopes);
	SendResponse(req, true, body);
}

//---------------------------------------------------------------------------
// 動的 variablesReference の管理
//---------------------------------------------------------------------------
void DebuggerCore::ClearVarRefs()
{
	var_ref_table_.clear();
	current_frames_.clear();
	next_var_ref_ = 1;
}

int64_t DebuggerCore::RegisterIfHasChildren(const tTJSVariant& v)
{
	if (v.Type() != tvtObject) return 0;
	if (v.AsObjectNoAddRef() == nullptr) return 0;
	int64_t ref = next_var_ref_++;
	VarRefEntry e;
	e.kind  = VarRefEntry::OBJECT_PROPS;
	e.value = v;
	e.ctx   = nullptr;
	var_ref_table_[ref] = std::move(e);
	return ref;
}

int64_t DebuggerCore::RegisterScope(VarRefEntry::Kind kind, tTJSInterCodeContext* ctx)
{
	// FRAME_GLOBALS は ctx 不要 (TVPGetScriptDispatch から取る)。
	// Locals/this は ctx 必須。
	if (kind != VarRefEntry::FRAME_GLOBALS && !ctx) return 0;
	int64_t ref = next_var_ref_++;
	VarRefEntry e;
	e.kind  = kind;
	e.ctx   = ctx;
	var_ref_table_[ref] = std::move(e);
	return ref;
}

//---------------------------------------------------------------------------
// iTJSDispatch2 メンバ列挙用コールバック
// (BuildVariablesFromFrame / FromVariant の両方から使うため早めに定義)
//---------------------------------------------------------------------------
struct EnumMembersCallback : public tTJSDispatch
{
	std::vector<TJSDebuggerVar>* out;

	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 /*flag*/, const tjs_char* /*membername*/, tjs_uint32* /*hint*/,
		tTJSVariant* result, tjs_int numparams, tTJSVariant** param,
		iTJSDispatch2* /*objthis*/) override
	{
		if (numparams < 3) return TJS_E_BADPARAMCOUNT;
		tjs_uint32 flags = (tjs_int)*param[1];
		if (flags & TJS_HIDDENMEMBER) {
			if (result) *result = (tjs_int)1;
			return TJS_S_OK;
		}
		TJSDebuggerVar v;
		v.name  = ttstr(*param[0]).AsStdString();
		v.value = *param[2];
		out->push_back(std::move(v));
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
};

picojson::array DebuggerCore::BuildVariablesFromFrame(VarRefEntry::Kind kind, tTJSInterCodeContext* ctx)
{
	picojson::array vars;
	// FRAME_GLOBALS は ctx 不要なので先に処理する (この時点で ctx==null でも OK)。

	if (kind == VarRefEntry::FRAME_GLOBALS) {
		iTJSDispatch2* global = TVPGetScriptDispatch();  // AddRef'd
		if (!global) return vars;

		std::vector<TJSDebuggerVar> entries;
		EnumMembersCallback cb;
		cb.out = &entries;
		tTJSVariantClosure clo(&cb, NULL);
		try {
			global->EnumMembers(TJS_IGNOREPROP, &clo, global);
		} catch (...) {
			global->Release();
			return vars;
		}

		// 各エントリで MakeVariableObject が例外投げる可能性があるので個別に try/catch
		// (ホットな global メンバ、特に Property 経由のものが TVPPrettyPrint 中にエラーすることがある)
		for (auto& e : entries) {
			try {
				vars.push_back(picojson::value(MakeVariableObject(e.name, e.value)));
			} catch (...) {
				// 個別エラーはスキップ
			}
		}
		global->Release();
		return vars;
	}

	if (!ctx) return vars;
	std::vector<TJSDebuggerVar> entries;
	if (kind == VarRefEntry::FRAME_LOCALS) {
		TJSDebuggerGetLocalVariables(ctx->GetDebuggerScopeKey(),
			ctx->GetDebuggerRegisterArea(), entries);
	} else if (kind == VarRefEntry::FRAME_THIS) {
		TJSDebuggerGetClassVariables(ctx->GetSelfClassName().c_str(),
			ctx->GetDebuggerRegisterArea(),
			ctx->GetDebuggerDataArea(), entries);
	}
	for (auto& e : entries) {
		vars.push_back(picojson::value(MakeVariableObject(e.name, e.value)));
	}
	return vars;
}

picojson::object DebuggerCore::MakeVariableObject(const tjs_string& name, const tTJSVariant& v)
{
	std::string name_utf8;
	TVPUtf16ToUtf8(name_utf8, name);

	ttstr value_str = TVPPrettyPrint(v, 1, true);  // 1 行 compact 表示
	std::string value_utf8;
	TVPUtf16ToUtf8(value_utf8, value_str.AsStdString());

	int64_t child_ref = RegisterIfHasChildren(v);

	picojson::object var;
	var["name"]               = picojson::value(name_utf8);
	var["value"]              = picojson::value(value_utf8);
	var["variablesReference"] = picojson::value(child_ref);
	return var;
}

//---------------------------------------------------------------------------
// 子要素 picojson 配列を組み立てる: Array なら count + PropGetByNum、
// それ以外は EnumMembers でメンバを舐める。
//---------------------------------------------------------------------------
picojson::array DebuggerCore::BuildVariablesFromVariant(const tTJSVariant& v)
{
	picojson::array vars;
	if (v.Type() != tvtObject) return vars;
	iTJSDispatch2* dsp = v.AsObjectNoAddRef();
	if (!dsp) return vars;

	// Array 判定: "count" プロパティが取れて整数なら Array とみなす
	bool is_array = false;
	tjs_int array_count = 0;
	{
		tTJSVariant cv;
		if (TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("count"), NULL, &cv, dsp))
			&& (cv.Type() == tvtInteger)) {
			is_array = true;
			array_count = (tjs_int)cv.AsInteger();
		}
	}

	if (is_array && array_count >= 0) {
		for (tjs_int i = 0; i < array_count; ++i) {
			tTJSVariant elem;
			if (TJS_FAILED(dsp->PropGetByNum(0, i, &elem, dsp))) continue;
			tjs_char buf[32];
			TJS_snprintf(buf, sizeof(buf)/sizeof(buf[0]), TJS_W("[%d]"), (int)i);
			vars.push_back(picojson::value(MakeVariableObject(tjs_string(buf), elem)));
		}
		return vars;
	}

	// Dict / 一般 Object: EnumMembers
	std::vector<TJSDebuggerVar> entries;
	EnumMembersCallback cb;
	cb.out = &entries;
	tTJSVariantClosure clo(&cb, NULL);
	dsp->EnumMembers(TJS_IGNOREPROP, &clo, dsp);
	for (auto& e : entries) {
		vars.push_back(picojson::value(MakeVariableObject(e.name, e.value)));
	}
	return vars;
}

void DebuggerCore::HandleVariables(const picojson::value& req)
{
	int64_t var_ref = 0;
	const auto& obj = req.get<picojson::object>();
	auto args_it = obj.find("arguments");
	if (args_it != obj.end() && args_it->second.is<picojson::object>()) {
		auto vr_it = args_it->second.get<picojson::object>().find("variablesReference");
		if (vr_it != args_it->second.get<picojson::object>().end() && vr_it->second.is<int64_t>()) {
			var_ref = vr_it->second.get<int64_t>();
		}
	}

	picojson::array vars;
	auto it = var_ref_table_.find(var_ref);
	if (it != var_ref_table_.end()) {
		const VarRefEntry& e = it->second;
		switch (e.kind) {
		case VarRefEntry::OBJECT_PROPS:
			vars = BuildVariablesFromVariant(e.value);
			break;
		case VarRefEntry::FRAME_LOCALS:
		case VarRefEntry::FRAME_THIS:
		case VarRefEntry::FRAME_GLOBALS:
			vars = BuildVariablesFromFrame(e.kind, e.ctx);
			break;
		}
	}

	picojson::object body;
	body["variables"] = picojson::value(vars);
	SendResponse(req, true, body);
}

void DebuggerCore::HandleEvaluate(const picojson::value& req)
{
	std::string expr_utf8;
	int64_t frame_id = 0;
	const auto& obj = req.get<picojson::object>();
	auto args_it = obj.find("arguments");
	if (args_it != obj.end() && args_it->second.is<picojson::object>()) {
		const auto& args = args_it->second.get<picojson::object>();
		auto e_it = args.find("expression");
		if (e_it != args.end() && e_it->second.is<std::string>()) {
			expr_utf8 = e_it->second.get<std::string>();
		}
		auto fi_it = args.find("frameId");
		if (fi_it != args.end() && fi_it->second.is<int64_t>()) {
			frame_id = fi_it->second.get<int64_t>();
		}
	}

	if (expr_utf8.empty()) {
		SendResponse(req, false, {}, "empty expression");
		return;
	}

	tjs_string expr_u16;
	TVPUtf8ToUtf16(expr_u16, expr_utf8);
	ttstr expr(expr_u16.c_str());

	// frameId 指定があれば対応 frame の ctx を、なければ topmost を使う
	tTJSInterCodeContext* eval_ctx = nullptr;
	if (frame_id > 0 && (size_t)(frame_id - 1) < current_frames_.size()) {
		eval_ctx = current_frames_[frame_id - 1].ctx;
	} else {
		eval_ctx = current_ctx_;
	}
	iTJSDispatch2* ctx_dict = BuildEvalContext(eval_ctx);

	tTJSVariant result;
	std::string error_msg;
	bool ok = false;
	try {
		TVPExecuteExpression(expr, ctx_dict, &result);
		ok = true;
	} catch (eTJS& e) {
		std::string emsg;
		TVPUtf16ToUtf8(emsg, e.GetMessage().AsStdString());
		error_msg = "TJS error: " + emsg;
	} catch (...) {
		error_msg = "unknown error during evaluate";
	}
	if (ctx_dict) ctx_dict->Release();

	if (!ok) {
		SendResponse(req, false, {}, error_msg);
		return;
	}

	ttstr pretty = TVPPrettyPrint(result, 3, false);
	std::string pretty_utf8;
	TVPUtf16ToUtf8(pretty_utf8, pretty.AsStdString());

	// 結果が Object/Array の場合は variablesReference を発行して子展開可能にする
	int64_t result_ref = RegisterIfHasChildren(result);

	picojson::object body;
	body["result"]             = picojson::value(pretty_utf8);
	body["variablesReference"] = picojson::value(result_ref);
	SendResponse(req, true, body);
}

//---------------------------------------------------------------------------
void DebuggerCore::HandleContinue(const picojson::value& req)
{
	exec_mode_.store(EXEC_RUN);
	should_resume_.store(true);
	picojson::object body;
	body["allThreadsContinued"] = picojson::value(true);
	SendResponse(req, true, body);
	pause_cv_.notify_all();
}

void DebuggerCore::HandlePause(const picojson::value& req)
{
	exec_mode_.store(EXEC_PAUSE);
	SendResponse(req, true);
	// 実際の停止は次の VM hook で発生
}

//---------------------------------------------------------------------------
void DebuggerCore::HandleExceptionInfo(const picojson::value& req)
{
	picojson::object body;
	if (has_current_exception_) {
		// description / typeName を組み立てる
		ttstr type_name(TJS_W("Exception"));
		ttstr desc;

		if (current_exception_.Type() == tvtObject) {
			// 例外オブジェクトから "message" プロパティを引いてみる
			iTJSDispatch2* dsp = current_exception_.AsObjectNoAddRef();
			if (dsp) {
				tTJSVariant msg;
				if (TJS_SUCCEEDED(dsp->PropGet(0, TJS_W("message"), NULL, &msg, dsp))) {
					desc = ttstr(msg);
				}
			}
		}
		if (desc.IsEmpty()) {
			// それ以外は pretty-print 全体を description に
			desc = TVPPrettyPrint(current_exception_, 2, true);
		}

		std::string id_utf8, desc_utf8, type_utf8;
		TVPUtf16ToUtf8(id_utf8,   type_name.AsStdString());
		TVPUtf16ToUtf8(desc_utf8, desc.AsStdString());
		TVPUtf16ToUtf8(type_utf8, type_name.AsStdString());

		body["exceptionId"] = picojson::value(id_utf8);
		body["description"] = picojson::value(desc_utf8);
		body["breakMode"]   = picojson::value(std::string("always"));

		picojson::object details;
		details["message"]      = picojson::value(desc_utf8);
		details["typeName"]     = picojson::value(type_utf8);
		body["details"]         = picojson::value(details);
	} else {
		body["exceptionId"] = picojson::value(std::string("(none)"));
		body["description"] = picojson::value(std::string("no current exception"));
		body["breakMode"]   = picojson::value(std::string("never"));
	}
	SendResponse(req, true, body);
}

//---------------------------------------------------------------------------
// Step 系: 現 depth を baseline として保存し、対応モードで再開する
//---------------------------------------------------------------------------
void DebuggerCore::HandleNext(const picojson::value& req)
{
	step_baseline_depth_ = step_depth_;
	exec_mode_.store(EXEC_STEP_OVER);
	should_resume_.store(true);
	SendResponse(req, true);
	pause_cv_.notify_all();
}

void DebuggerCore::HandleStepIn(const picojson::value& req)
{
	step_baseline_depth_ = step_depth_;
	exec_mode_.store(EXEC_STEP_IN);
	should_resume_.store(true);
	SendResponse(req, true);
	pause_cv_.notify_all();
}

void DebuggerCore::HandleStepOut(const picojson::value& req)
{
	step_baseline_depth_ = step_depth_;
	exec_mode_.store(EXEC_STEP_OUT);
	should_resume_.store(true);
	SendResponse(req, true);
	pause_cv_.notify_all();
}

void DebuggerCore::HandleDisconnect(const picojson::value& req)
{
	// disconnect の terminateDebuggee 引数を読む。
	// VSCode の停止ボタン:
	//   launch で起動した場合 → terminateDebuggee=true (krkrz 自体を終了)
	//   attach の場合         → terminateDebuggee=false (デバッガだけ離す)
	bool terminate_debuggee = false;
	const auto& obj = req.get<picojson::object>();
	auto args_it = obj.find("arguments");
	if (args_it != obj.end() && args_it->second.is<picojson::object>()) {
		auto td_it = args_it->second.get<picojson::object>().find("terminateDebuggee");
		if (td_it != args_it->second.get<picojson::object>().end()
			&& td_it->second.is<bool>()) {
			terminate_debuggee = td_it->second.get<bool>();
		}
	}

	SendResponse(req, true);

	// クライアント切断後の状態をクリア
	TVPDebuggerAttachedFlag.store(false, std::memory_order_release);
	exec_mode_.store(EXEC_RUN);
	should_resume_.store(true);
	break_on_uncaught_exception_ = false;
	configuration_done_.store(false);
	breakpoints_.ClearAll();
	pause_cv_.notify_all();

	if (terminate_debuggee) {
		TVPTerminateAsync(0);
	}
}

//---------------------------------------------------------------------------
// 共通ヘルパ
//---------------------------------------------------------------------------
void DebuggerCore::SendResponse(const picojson::value& req, bool success,
								 const picojson::object& body,
								 const std::string& message)
{
	picojson::object resp;
	resp["seq"]  = picojson::value((int64_t)seq_counter_.fetch_add(1));
	resp["type"] = picojson::value(std::string("response"));

	int64_t request_seq = 0;
	std::string command;
	if (req.is<picojson::object>()) {
		const auto& obj = req.get<picojson::object>();
		auto seq_it = obj.find("seq");
		if (seq_it != obj.end() && seq_it->second.is<int64_t>())
			request_seq = seq_it->second.get<int64_t>();
		auto cmd_it = obj.find("command");
		if (cmd_it != obj.end() && cmd_it->second.is<std::string>())
			command = cmd_it->second.get<std::string>();
	}
	resp["request_seq"] = picojson::value(request_seq);
	resp["command"]     = picojson::value(command);
	resp["success"]     = picojson::value(success);
	if (!message.empty()) resp["message"] = picojson::value(message);
	if (!body.empty())    resp["body"]    = picojson::value(body);

	if (server_) server_->PostMessage(picojson::value(resp));
}

void DebuggerCore::SendEvent(const std::string& event, const picojson::object& body)
{
	picojson::object ev;
	ev["seq"]   = picojson::value((int64_t)seq_counter_.fetch_add(1));
	ev["type"]  = picojson::value(std::string("event"));
	ev["event"] = picojson::value(event);
	if (!body.empty()) ev["body"] = picojson::value(body);
	if (server_) server_->PostMessage(picojson::value(ev));
}

//---------------------------------------------------------------------------
// 停止/再開
//---------------------------------------------------------------------------
void DebuggerCore::BlockUntilResume(tTJSInterCodeContext* /*ctx*/, const std::string& reason)
{
	// stopped event を送る
	picojson::object body;
	body["reason"]            = picojson::value(reason);
	body["threadId"]          = picojson::value((int64_t)1);
	body["allThreadsStopped"] = picojson::value(true);
	SendEvent("stopped", body);

	// 再開要求 / 切断 / シャットダウンまで待つ
	while (!should_resume_.load() && !terminating_.load()) {
		if (!server_ || !server_->IsClientConnected()) break;

		// 停止中もリクエストは処理する (setBreakpoints / stackTrace / continue 等)
		picojson::value v;
		while (server_->TryPopMessage(v)) {
			ProcessRequest(v);
		}
		if (should_resume_.load() || terminating_.load()) break;
		if (!server_->IsClientConnected()) break;

		// 停止中も REPL を回す (icline からの入力 1 件処理)。in_repl_eval_ で
		// その間の VM hook を skip し、step tracking を乱さない。
#ifdef KRKRZ_USE_REPL
		in_repl_eval_ = true;
		TVPDrainREPL();
		in_repl_eval_ = false;
#endif

		std::unique_lock<std::mutex> lk(pause_mtx_);
		pause_cv_.wait_for(lk, std::chrono::milliseconds(50));
	}

	should_resume_.store(false);
	current_ctx_ = nullptr;
}

} // namespace TJS

//---------------------------------------------------------------------------
// public wrapper API
//---------------------------------------------------------------------------
void TVPCreateDAP()
{
	tTJSVariant val;
	if (!TVPGetCommandLine(TJS_W("-dap"), &val)) return;

	ttstr s(val);
	if (s == TJS_W("no") || s == TJS_W("off") || s == TJS_W("false") || s == TJS_W("0"))
		return;

	int port = 6635; // default
	if (!s.IsEmpty() && s != TJS_W("yes") && s != TJS_W("on") && s != TJS_W("true")) {
		// 数値として解釈
		int parsed = (int)s.AsInteger();
		if (parsed > 0 && parsed < 65536) port = parsed;
	}

	DebuggerCore::Init(port);
}

void TVPDestroyDAP()
{
	TJS::DebuggerCore::Shutdown();
}

void TVPDrainDAP()
{
	if (auto* core = TJS::DebuggerCore::Instance()) {
		core->DrainMain();
	}
}

#endif // KRKRZ_ENABLE_DAP
