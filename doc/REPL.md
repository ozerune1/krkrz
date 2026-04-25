# REPL (Read-Eval-Print Loop)

`KRKRZ_REPL=ON` でビルドされたバイナリに、コマンドライン引数 `-repl`
(または `-repl=yes`) を付けて起動すると、対話型 TJS シェルが有効になります。
Win / Mac / Linux 対応。行編集は [icline](https://github.com/deths74r/icline)
(isocline フォーク) を利用し、FetchContent で取得されます。

## 起動

```bash
# SDL 版 (console subsystem なのでそのまま動きます)
krkrz -repl data/

# Win32 版 (windowed subsystem なので親コンソールに接続します)
krkrz64 -repl data/
```

`-repl` が無ければ REPL は起動しません (TTY 自動判定は無し)。
`-repl=no` / `-repl=off` / `-repl=false` / `-repl=0` で明示的に無効化も可能。

WIN (windowed subsystem) 版では REPL 起動時に `AttachConsole` で親プロセス
のコンソールを捕まえ、それも無ければ `AllocConsole` で新規確保します。

## コマンド・操作

プロンプトは `krkrz>`、継続行は `...`。TJS 式や文を入力して改行で評価
されます (`;` 無しの式も可)。括弧・クォートが閉じていない間は継続入力に
なります。履歴はカレントディレクトリの `.krkrz_history` に保存。

REPL 特殊コマンド:

| コマンド | 説明 |
|---|---|
| `exit` / `quit` / `Ctrl+D` | REPL を抜けてアプリを `TVPTerminateAsync(0)` で終了 |
| `.help` | ヘルプ表示 |
| `.clear` | 継続入力のバッファをクリア |
| `.depth [N]` | 結果表示の展開深さを表示または設定 |
| `.compact [on\|off]` | 結果表示のコンパクトモード切替 |

## 結果表示

評価結果は `TVPPrettyPrint(variant, depth, compact)` (`DebugIntf.h` 公開、
TJS では `Debug.prettyPrint(v, depth=2, compact=false)`) で整形されます:

- `void` → `(void)`、null object → `(null)`
- 数値・文字列・octet → `TJSVariantToExpressionString` 相当
- Array → `[ e1, e2, ... ]` (compact は `[e1, e2, ...]`)
- Dictionary → `%[ "k" => v, ... ]`
- Function / Class / Property → `(function)` / `(class)` / `(property)`
- その他 object → `(object: 0x...)`
- 深さ到達 → `[...]` / `%[...]`
- 循環参照 → `(recursion)`

ログ出力は REPL 実行中、icline の bbcode 機能でレベル別に色付けされて
プロンプト行の上に割り込み表示されます (VERBOSE=gray, DEBUG=cyan,
INFO=デフォルト, WARNING=yellow, ERROR=red, CRITICAL=bold red)。

## スレッド構造

REPL ワーカースレッドが `ic_readline` で入力をブロッキング取得し、
完成した式を専用の CV 付き request スロットに積み、メインスレッドを
起床させます。メインスレッドは毎 frame `TVPDrainREPL()` を呼び出して
リクエストを 1 件ずつ取り出し `TVPExecuteExpression` を実行、結果を
response スロットに詰めて CV で worker を起こします。

この構造により:

- TJS エンジンのスレッドアフィニティ (メインスレッドのみ) を守りつつ
  ワーカー側で行編集が動く
- 初期起動スクリプト (AM_STARTUP_SCRIPT) が長時間走っていても、完了次第
  REPL リクエストが確実にピックアップされる (`NativeEventQueue` の共有
  `command_que_` を介さないため、他のイベントと競合しない)
- Win32 では worker が `PostThreadMessage(WM_NULL)` でメインスレッドの
  `WaitMessage` を起こす。SDL3 では `SDL_AppIterate` が連続呼び出しされる
  ので起床機構は不要
