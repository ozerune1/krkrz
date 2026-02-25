#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "KeyRepeat.h"

#include "Application.h"
#include "WindowForm.h"
#include "LogIntf.h"

// ビット位置とキーマップの対応
static int TVPPadVirtualKeyMap[] = {
    VK_PAD1,        // A
    VK_PAD2,        // B
    VK_PAD3,        // X
    VK_PAD4,        // Y
    VK_PAD5,        // L
    VK_PAD6,        // R
    VK_PAD7,        // LT
    VK_PAD8,        // RT
    VK_PAD9,        // SELECT
    VK_PAD10,       // START
    VK_PAD11,       // L3
    VK_PAD12,       // R3
//12
	VK_PADLEFT,     // 左
    VK_PADUP,       // 上
    VK_PADRIGHT,    // 右
    VK_PADDOWN,     // 下
//16
	VK_PAD_L_LEFT,  // アナログ左上下左右
    VK_PAD_L_UP,    // アナログ左上下左右
    VK_PAD_L_RIGHT, // アナログ左上下左右
    VK_PAD_L_DOWN,  // アナログ左上下左右
//20
	VK_PAD_R_LEFT,  // アナログ右上下左右
    VK_PAD_R_UP,    // アナログ右上下左右
    VK_PAD_R_RIGHT, // アナログ右上下左右
    VK_PAD_R_DOWN,  // アナログ右上下左右
// 24
};

#define TVP_NUM_PAD_KEY 24

// 以前のパッド状態
static tjs_uint32 LastPadState = 0;
static tjs_uint32 LastPushedTrigger = 0;

// 直近で送付したイベント
static std::vector<int> UppedKeys;
static std::vector<int> DownedKeys;
static std::vector<int> RepeatKeys;

// キーリピート処理用
static tTVPKeyRepeatEmulator CrossKeysRepeater;
static tTVPKeyRepeatEmulator TriggerKeysRepeater;

// キー押し下げ状態取得（とりあえずパッドのみ）
bool 
tTVPApplication::GetAsyncKeyState(tjs_uint keycode, bool getcurrent)
{
	if (getcurrent) { // false ならトグル状態取得 XXX
		int code = -1;
		if (keycode >= VK_PAD1 && keycode <= VK_PAD12) { // 他のボタン
			code = keycode-VK_PAD1;
		} else if (keycode >= VK_PADLEFT && keycode <= VK_PADDOWN) { // カーソル
			code = keycode-VK_PADLEFT + 12;
		} else if (keycode >= VK_PAD_L_LEFT && keycode <= VK_PAD_L_DOWN) { // カーソル
			code = keycode-VK_PAD_L_LEFT + 16;
		} else if (keycode >= VK_PAD_R_LEFT && keycode <= VK_PAD_R_DOWN) { // カーソル
			code = keycode-VK_PAD_R_LEFT + 20;
		}
		if (code >= 0) {
			return (LastPadState & (1<<code)) != 0;
		}
	}
	return false;
}

void 
tTVPApplication::SendPadEvent()
{
    TTVPWindowForm *form = MainWindowForm();
	if (!form) return;

	tjs_uint32 newstate = GetPadState(0);

	// キーリピート処理
	// virtual/win32/DInputMgn.cpp 由来
	UppedKeys.clear();
	DownedKeys.clear();
	RepeatKeys.clear();

	tjs_uint32 downed = newstate & ~LastPadState; // newly pressed buttons
	tjs_uint32 upped  = ~newstate & LastPadState; // newly released buttons

	// emulate key repeats.
	// key repeats are calculated about two groups independently.
	// one is cross-keys(up, down, left, right) and the other is
	// trigger buttons(button0 .. button9).
	const tjs_uint32 cross_group_mask = 0xfff<<12;
	const tjs_uint32 trigger_group_mask = ~cross_group_mask;

	if(!(LastPadState & cross_group_mask) && (newstate & cross_group_mask))
		CrossKeysRepeater.Down(); // any pressed
	if(!(newstate     & cross_group_mask))
		CrossKeysRepeater.Up(); // all released

	if     (downed & trigger_group_mask) TriggerKeysRepeater.Down();
	else if(upped  & trigger_group_mask) TriggerKeysRepeater.Up();

	if(downed & trigger_group_mask) LastPushedTrigger = downed & trigger_group_mask;

	// scan downed buttons
	for(tjs_int i = 0; i < TVP_NUM_PAD_KEY; i++)
		if((1<<i) & downed) DownedKeys.push_back(TVPPadVirtualKeyMap[i]);
	// scan upped buttons
	for(tjs_int i = 0; i < TVP_NUM_PAD_KEY; i++)
		if((1<<i) & upped)  UppedKeys.push_back(TVPPadVirtualKeyMap[i]);
	// scan cross group repeats
	tjs_int cnt;
	cnt = CrossKeysRepeater.GetRepeatCount();
	if(cnt)
	{
		tjs_uint32 t = newstate & cross_group_mask;
		do
		{
			for(tjs_int i = 0; i < TVP_NUM_PAD_KEY; i++)
				if((1<<i) & t) RepeatKeys.push_back(TVPPadVirtualKeyMap[i]);
		} while(--cnt);
	}

	// scan trigger group repeats
	cnt = TriggerKeysRepeater.GetRepeatCount();
	if(cnt)
	{
		tjs_uint32 t = LastPushedTrigger;
		do
		{
			for(tjs_int i = 0; i < TVP_NUM_PAD_KEY; i++)
			{
				if((1<<i) & t)
				{
					RepeatKeys.push_back(TVPPadVirtualKeyMap[i]);
					break;
				}
			}
		} while(--cnt);
	}

	// update last state
	LastPadState = newstate;

	// イベントとして送信
	int shift = 0;
	for(auto it= UppedKeys.begin();it != UppedKeys.end(); it++) {
		form->SendMessage(AM_KEY_UP, *it, shift);
	}
	for(auto it= DownedKeys.begin();it != DownedKeys.end(); it++) {
		form->SendMessage(AM_KEY_DOWN, *it, shift);
	}
	for(auto it= RepeatKeys.begin(); it != RepeatKeys.end(); it++) {
		form->SendMessage(AM_KEY_DOWN, *it, shift|TVP_SS_REPEAT);
	}
}
