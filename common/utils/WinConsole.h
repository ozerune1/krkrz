//---------------------------------------------------------------------------
// Windows コンソール attach/detach 共通ヘルパ
//
// WINVER (GUI サブシステム) と SDL3 (Windows では GUI サブシステム) の両方で、
// 親プロセス (シェル) のコンソールに attach してログ出力を可視化するために使う。
// 非 Windows プラットフォームでは no-op。
//---------------------------------------------------------------------------
#ifndef WIN_CONSOLE_H
#define WIN_CONSOLE_H

// 親プロセスのコンソールに attach する。既に自プロセスがコンソールを持っている
// 場合は何もしない。attach に成功したら内部フラグを立てる。
// 戻り値: attach 済み (または最初からコンソールを持っていた) なら true。
bool TVPAttachWindowsConsole();

// TVPAttachWindowsConsole() で attach したコンソールを解放する。
// もともと attach していない場合は何もしない。
void TVPDetachWindowsConsole();

// 現在、本ヘルパ経由で親コンソールに attach 済みか。
bool TVPIsAttachedWindowsConsole();

#endif
