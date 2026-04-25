# ログ系

## 構造

ログ出力は `common/utils/LogCore.cpp` を中心に一本化されています。
全てのログは最終的に `TVPLogDispatchLine(level, utf8_line)` に集約され、
次の順番で処理されます:

1. タイムスタンプ (`HH:MM:SS`) を付与、WARNING 以上には `!` マーカー追加
2. リングバッファ (`TVPLogDeque`, 既定 2048 行) に追加
3. important log キャッシュ (WARNING 以上) に追加
4. TJS logging handler (`Debug.addLoggingHandler`) 発火
5. ファイル出力 (`krkr.console.log`, UTF-16 LE + BOM + CRLF) — 有効時
6. コンソール出力 — sink が登録されていればそちらへ、無ければ既定書き出し

LogImpl (`generic/utils/LogImpl.cpp` = plog 版、`sdl3/utils/LogImpl.cpp` =
SDL3 版) はレベル整形だけを担当し、整形済みの本文を LogCore に渡します。
タイムスタンプは LogCore 側で一元付与され、LogImpl は含めません。

## API

### 出力

- `TVPLOG_VERBOSE / DEBUG / INFO / WARNING / ERROR / CRITICAL(fmt, ...)`:
  `tvpfmt` 書式 (後述) でのレベル付きログ。file/func/line が自動付与されます。
- `TVPLogMsg(level, utf8)`: シンプルな UTF-8 文字列を直接流す経路。
- `TVPAddLog(ttstr)` / `TVPAddImportantLog(ttstr)`: 旧来 API (互換目的)。
  それぞれ INFO / WARNING 相当で LogCore 経由に転送されます。

## 書式整形層: tvpfmt

ログ専用のミニ書式整形器です。実体は `common/utils/LogIntf.h` と
`common/utils/LogCore.cpp` (`tvpfmt::vformat`) に閉じており、外部依存は
ありません。

### 経緯

もともとは fmtlib / C++20 `<format>` を切り替えて使う薄いラッパでしたが、
C++20 対応が中途半端な環境で fmtlib 9/10 も `<format>` もビルドできない
ケースが出たため、**ログ用途に必要な機能だけを自前実装** する方針に切り
替えました。fmtlib / `<format>` への依存は完全に削除されています。

UTF-16 の `tjs_char*` / `ttstr` / `tjs_string` は引数を積む時点で UTF-8
へ変換されるため、低層の `TVPLog()` が UTF-8 を受ける前提とそのまま噛み
合います。SDL3 版は整形後の UTF-8 を `SDL_LogMessage` に、plog 版は
`plog::Record` に流します。

### 使える書式

| 書式 | 意味 | 例 |
|---|---|---|
| `{}` | 既定。型から自動選択 | `TVPLOG_INFO("path: {}", path)` |
| `{:d}` `{:i}` | 10 進整数 | `TVPLOG_DEBUG("n={:d}", n)` |
| `{:x}` `{:X}` | 16 進整数 (小/大文字) | `TVPLOG_ERROR("err {:x}", code)` |
| `{:o}` | 8 進整数 | — |
| `{:u}` | 符号なし 10 進 | — |
| `{:s}` | 文字列 (既定と同じ) | — |
| `{:0Nd}` `{:0Nx}` | ゼロ埋め + 幅指定 | `"{:08x}"` で `0000beef` |
| `{:Nd}` `{:Nx}` | 幅指定のみ | — |
| `{:f}` `{:g}` `{:e}` `{:G}` `{:E}` `{:F}` | 浮動小数点 (既定は `%g`) | `"{}"` でも float/double を受け付けます |
| `{{` `}}` | 波括弧リテラル | — |

対応している引数型:

- 真偽値、整数 (`char` / `short` / `int` / `long` / `long long` と各符号なし)
- 浮動小数点 (`float` / `double` / `long double` — 内部は `double`)
- C 文字列 (`char*` / `const char*`)
- `std::string`
- `const tjs_char*` / `ttstr` / `tjs_string` (UTF-8 へ自動変換)
- 任意のポインタ型 (`%p` 相当)

### 使用例

```cpp
TVPLOG_INFO("Loaded GLES {}.{}", major, minor);
TVPLOG_ERROR("OpenGL error occurred: {:08x} {}", error_code, msg);
TVPLOG_DEBUG("rect: {},{},{},{}", left, top, right, bottom);   // float でも OK
TVPLOG_DEBUG("Opening {} (access={})", path_utf8, access);
TVPLOG_INFO("project: {}", projectPath);                        // ttstr も OK
```

### あえて対応していないもの

ログ用途では使われていないため意図的に実装していません。必要になったら
`LogCore.cpp` の `vformat` / `format_one` を拡張する形で追加してください。

- 位置指定 (`{0}` `{1}`)
- アライン / 埋め文字 (`{:<10}` `{:*>8}`)
- 精度 (`{:.3f}`)
- `{:b}` (2 進)
- ユーザ型向け `formatter<T>` 特殊化 — 呼び出し側で `ttstr` / `std::string`
  へ明示変換してから渡してください。

書式エラー (閉じていない `{` など) は `tvpfmt::format_error` を投げますが、
`TVPLog()` は内部で捕捉して "Log Format error: ..." をメッセージ化するの
でクラッシュしません。

### 将来の再検討

C++20 `<format>` が主要ターゲット (Windows / Linux / macOS / Android /
iOS) の標準ツールチェーンで安定して使えるようになったら、`tvpfmt` を
`<format>` の薄いラッパに差し戻すことを検討してください。その際は:

- `vformat` の仕様差 (例: `{}` に `ttstr` を渡すには `formatter` 特殊化が
  必要) を吸収するアダプタを `LogIntf.h` 側に用意する
- 呼び出し側 (`TVPLOG_*` マクロおよび `tvpfmt::make_format_args` を直接
  使っている箇所 — 現状は `OpenGLError.cpp` のみ) を破壊しないこと
- fmtlib との二股復活は避ける (今回撤去した理由そのものなので)

逆にログ以外の用途で書式整形が必要になった場合は、`tvpfmt` を広げるより
呼び出し側で個別に `snprintf` / `std::to_string` を使うか、C++20
`<format>` が使える前提にビルド要件を引き上げるほうが健全です。

### レベル設定

起動時オプション `-loglevel=ERROR,WARNING,INFO,DEBUG,VERBOSE` で変更可能。
`MASTER` 定義でビルドされたバイナリでは WARNING 固定。

### Sink

`TVPLogSetConsoleSink(hook)` でコンソール出力を乗っ取れます。REPL 側が
起動時にこれを登録し、icline の bbcode でレベル別に色付けしたうえで
プロンプト行に割り込み表示します。

### ファイル出力

- `TVPStartLogToFile(bool clear)` — ログファイル出力を開始
- 出力先: `[TVPLogLocation]/krkr.console.log` (UTF-16 LE + BOM)
- `-forcelog=yes|clear` — 起動時にファイル出力を強制開始
- `-logerror=no|clear` — エラー時の自動フラッシュ挙動を制御

### TJS 面

- `Debug.message(...)` / `Debug.notice(...)` — INFO / WARNING 出力
- `Debug.getLastLog(n)` — リングバッファから最新 n 行を取得
- `Debug.addLoggingHandler(func)` / `Debug.removeLoggingHandler(func)`
- `Debug.logLocation` — ログ出力先ディレクトリ (プロパティ)
- `Debug.startLogToFile(clear)` — ファイル出力開始
- `Debug.logToFileOnError` / `Debug.clearLogFileOnError` — エラー時挙動
