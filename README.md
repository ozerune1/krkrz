# 吉里吉里Z multi platform

## 概要

マルチプラットフォーム展開を想定した吉里吉里Zです

・システム基本制御は SDL3 を使います
・OpenGLベース描画機構を持ちます Canvas/Screen/Texture/Shader
・極力外部ライブラリを参照する形で構築されています。

外部ライブラリの参照には vcpkg を利用しています。
SDL3 は最新版を利用する関係で FettchContents で処理されます。

## 開発環境準備

### Windows

Windows用に Visual Studio をインストールして
C++ コンパイラ を使える状態にしておきます。

あわせて Visual Studio 付属の Cmake / Ninja を利用します。

※Visual Studio 2022 以降は vcpkg があわせて導入されますが、
自前環境を使う場合は競合してまうので入れない様にして下さい。

make を使いたい場合は、msys2 をインストールして基礎開発ツールを導入しておきます。

```bash
pacman -S base-devel
```

### Linux

整備中

### OSX

整備中

### vcpkg 環境準備

各環境に vcpkg を導入します。

https://learn.microsoft.com/ja-jp/vcpkg/get_started/overview

vcpkg のフォルダを環境変数 VCPKG_ROOT に設定しておきます。

```bash
# dos
set VCPKG_ROOT="c:\work\vcpkg"

# msys/cygwin
export VCPKG_ROOT='c:\work\vcpkg'
```

## ビルド

### ソースのチェックアウト

git clone 後 submodule 更新しておいてください

```
git submodule update --init
```

### ビルド

CMakePresets.json 中のプリセット定義をつかってビルドします。
必要なライブラリは vcpkg.json によってセットアップされます。

ビルドフォルダはデフォルトでは build/プリセット名 になっています。
また Generator は Ninja Multi Config での生成になります。

vpkg.json で外部ライブラリを扱うため、
CMAKE_TOOLCHAIN_FILE は vcpkg のものが指定されています。

```bash
cmake --preset x86-windows --config Release
cmake --build build/x86-windows
```

ビルドに必要な定義が行われた Makefile が準備されていいます。
make が使える環境ではこちらが利用可能です


```bash
# 構築対象 preset設定（未定義時はOSで自動判定）
export PRESET=x86-windows

# ビルドタイプ指定（未定義時は Release）
export BUILD_TYPE=Release
#export BUILD_TYPE=Debug

# cmake オプション指定
# KRKRZ_USE_SJIS  デフォルトをSJIS(MBSC) にする
export CMAKEOPT="-DKRKRZ_USE_SJIS=ON"

# cmake プロジェクト生成
# この段階で vcpkg が処理されてライブラリが準備されます
make prebuild

# cmake でビルド
make build 

# サンプル実行
make run

# インストール処理
INSTALL_PREFIX=install make install

```	

### ビルド設定

処理内容詳細は Makefile と CMakeList.txt を参照して下さい。

ビルド用の以下の特殊な CMake変数があります

KRKRZ_VARIANT=WIN    旧来のWindows版準拠で構築します
KRKRZ_VARIANT=SDL    SDLバージョンで作成します（デフォルト）
KRKRZ_VARIANT=LIB    ライブラリ版KRKRZを作成します

KRKRZ_VARIANT=SDL / LIB では、旧来の Windows版固有の機能が排除
された GENERICバージョンの吉里吉里になります。

GENERICバージョンあわせのプラグインをビルドする場合は、tp_stub.h を
読み込む前に __GENERIC__ を定義しておく必要があるので注意してください。

### そのほか特殊変数

MASTER  
    ビルド時に定義されているとログレベルが WARNING で固定になります（INFOログがコンソール表示されなくなります）

    未定義時は、起動時ログレベルが Release 版は INFO、Debug版は DEBUG になります。
    起動時オプション -loglevel=ERROR,WARNING,INFO,DEBUG,VERBOSE で変更可能になります

※吉里吉里旧来の TVPAddLog() は INFO相当、TVPAddImportantlLog() は WARNING 相当になります

### テスト実行

Makefile にそのままトップフォルダで実行可能なルールが定義されています。

```bash
# cmake 経由で実行
make run
```

OpenGL 機能動作時は以下のファイル構成が必要になります

    plugin/ プラグインフォルダ
      libEGL.dll        OpenGL の egl用DLL
      libGLESv2.dll     OpenGL の GLES2用DLL
    plugin64/ プラグインフォルダ 64bit
      libEGL.dll        OpenGL の egl用DLL
      libGLESv2.dll     OpenGL の GLES2用DLL

### SIMDパリティテスト

`tests/simd_parity_test.cpp` に画像処理SIMD（SSE2 / AVX2 / NEON）と
C リファレンス実装の出力を byte 単位で比較する CTest テスト
（`krkrz_simd_parity_test` / テスト名 `simd_parity`）が用意されています。

このテストは `tvpgl.c` / `blend_function.cpp` / 各 `*_sse2.cpp` /
`*_avx2.cpp` / `*_neon.cpp` / `detect_cpu.cpp` 等 SIMD コアのみを直接
リンクするスタンドアロンターゲットで、SDL3 / OpenGL / vcpkg のランタイム
依存はありません。`KRKRZ_BUILD_TESTS=ON`（デフォルト）かつターゲットアーキ
テクチャが x86 系または ARM 系のときに有効化されます。

```bash
# Makefile 経由 (prebuild 済みであること)
make test

# cmake / ctest 直接実行
cmake --build $(BUILD_PATH) --config Release --target krkrz_simd_parity_test
ctest --test-dir $(BUILD_PATH) -C Release -R simd_parity --output-on-failure
```

期待される出力:

- x86 (Windows / Linux / macOS): `[SSE2 vs C reference]` と
  `[AVX2 vs C reference]` の 2 セクションが走り、それぞれ全項目 pass。
- ARM / ARM64 (Linux / Android): `[NEON vs C reference]` セクションが走る。

PsBlend ファミリは SSE2 側が 7bit 量子化のため harness 側で
`tol_alpha=-1, tol_rgb=2`（ColorDodge5 のみ `tol_rgb=8`）の tolerance
policy が適用されます。それ以外は byte-exact 比較です。

## デバッグ実行

### VisualStudio でのデバッグ

以下の手順でソースデバッグできます

- Visual Studio を起動して、プロジェクトなしの状態のウインドウに実行ファイルをドロップする
- デバッグのプロパティの作業フォルダにプロジェクトフォルダを指定（プラグインフォルダの参照先になるため）
- デバッグのプロパティの引数に data フォルダの場所をフルパスで指定（現行仕様がexe相対もしくは絶対パス）

### VSCode でのデバッグ

次のような launch.jsonを準備して下さい。
program 部分に生成される実行ファイルのパス名を直接記載します。
args で処理対象フォルダを指定できます（フルパスになるように記載して下さい）

launch.json

```json
{
    // IntelliSense を使用して利用可能な属性を学べます。
    // 既存の属性の説明をホバーして表示します。
    // 詳細情報は次を確認してください: https://go.microsoft.com/fwlink/?linkid=830387
    "version": "0.2.0",
    "configurations": [
        {
            "name": "WINデバッグ起動",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "build/x86-windows/Debug/krkrz.exe",
            "args": ["${workspaceFolder}/data"],
            "stopAtEntry": false,
            "console":"externalTerminal",
            "cwd": "${workspaceFolder}",
            "environment": []
        }
    ]
}
```

# その他情報

自動生成ファイル
吉里吉里Z本体にはいくつかの自動生成ファイルが存在します。
自動生成ファイルは直接編集せず、生成元のファイルを編集します。
生成には主にbatファイルとperlが使用されているので、perlのインストールが必要です。
各生成ファイルを左に ':' 以降に生成元ファイルを列挙します。

tjs2/syntax/compile.bat で以下のファイルが生成されます。

tjs.tab.cpp/tjs.tab.hpp : tjs.y
tjsdate.tab.cpp/tjsdate.tab.hpp : tjsdate.y
tjspp.tab.cpp/tjspp.tab.hpp : tjspp.y
tjsDateWordMap.cc : gen_wordtable.bat

これらのファイルの生成には bison が必要です。
bison には libiconv2.dll libintl3.dll regex2.dll が必要なので一緒にインストールする必要があります。
http://gnuwin32.sourceforge.net/packages/bison.htm
http://gnuwin32.sourceforge.net/packages/libintl.htm
http://gnuwin32.sourceforge.net/packages/libiconv.htm
http://gnuwin32.sourceforge.net/packages/regex.htm

visual/glgen/gengl.bat で以下のファイルが生成されます。
tvpgl.c/tvpgl.h : maketab.c/tvpps.c

base/win32/makestub.bat で以下のファイルが生成されます。
FuncStubs.cpp/FuncStubs.h : makestub.pl内で指定されたヘッダーファイル内のTJS_EXP_FUNC_DEF/TVP_GL_FUNC_PTR_EXTERN_DECLマクロで記述された関数
tp_stub.cpp/tp_stub.h : 同上

