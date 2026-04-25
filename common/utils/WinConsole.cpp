//---------------------------------------------------------------------------
// Windows コンソール attach/detach 共通ヘルパ実装
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "WinConsole.h"

#ifdef _WIN32
#include <windows.h>

static bool g_tvp_attached_console = false;

bool TVPAttachWindowsConsole()
{
	// 自プロセスが既にコンソールを持っている (CONSOLE サブシステム起動 /
	// 事前 AllocConsole 済み等) かを判定
	DWORD curProcId = ::GetCurrentProcessId();
	DWORD processList[256];
	DWORD count = ::GetConsoleProcessList(processList, 256);
	for (DWORD i = 0; i < count; i++) {
		if (processList[i] == curProcId) {
			return true;
		}
	}

	// 既存 std ハンドルのスナップショット (非 NULL なら親から継承された
	// 有効なリダイレクト先なので残す)
	HANDLE hin  = ::GetStdHandle(STD_INPUT_HANDLE);
	HANDLE hout = ::GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE herr = ::GetStdHandle(STD_ERROR_HANDLE);

	if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
		return false;
	}

	// AttachConsole は NULL だった std ハンドルを新 console に張り替える。
	// 既存のリダイレクト (>file 等) は優先して戻す。
	if (hin)  ::SetStdHandle(STD_INPUT_HANDLE,  hin);
	if (hout) ::SetStdHandle(STD_OUTPUT_HANDLE, hout);
	if (herr) ::SetStdHandle(STD_ERROR_HANDLE,  herr);

	g_tvp_attached_console = true;
	return true;
}

void TVPDetachWindowsConsole()
{
	if (g_tvp_attached_console) {
		::FreeConsole();
		g_tvp_attached_console = false;
	}
}

bool TVPIsAttachedWindowsConsole()
{
	return g_tvp_attached_console;
}

#else // !_WIN32

bool TVPAttachWindowsConsole() { return false; }
void TVPDetachWindowsConsole() {}
bool TVPIsAttachedWindowsConsole() { return false; }

#endif
