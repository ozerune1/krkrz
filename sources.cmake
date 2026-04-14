
# 共通の include ディレクトリ
set( KRKRZ_INC
common/environ
common/tjs2
common/base
common/extension
common/sound
common/msg
common/utils
common/visual
common/visual/gl
common/visual/IA32
common/visual/opengl
common/glad/include
external
)

set( KRKRZ_INC_WIN32_COMMON
win32/vcproj
win32/environ
common/visual/IA32
)

set( KRKRZ_INC_WIN32
win32/vcproj
win32/environ
win32/base
win32/sound
win32/msg
win32/utils
win32/visual
win32/movie
common/visual/IA32
)

set( KRKRZ_SRC 
common/tjs2/tjs.cpp
common/tjs2/tjs.tab.cpp
common/tjs2/tjsArray.cpp
common/tjs2/tjsBinarySerializer.cpp
common/tjs2/tjsByteCodeLoader.cpp
common/tjs2/tjsCompileControl.cpp
common/tjs2/tjsConfig.cpp
common/tjs2/tjsConstArrayData.cpp
common/tjs2/tjsDate.cpp
common/tjs2/tjsdate.tab.cpp
common/tjs2/tjsDateParser.cpp
common/tjs2/tjsDebug.cpp
common/tjs2/tjsDictionary.cpp
common/tjs2/tjsDisassemble.cpp
common/tjs2/tjsError.cpp
common/tjs2/tjsException.cpp
common/tjs2/tjsGlobalStringMap.cpp
common/tjs2/tjsInterCodeExec.cpp
common/tjs2/tjsInterCodeGen.cpp
common/tjs2/tjsInterface.cpp
common/tjs2/tjsLex.cpp
common/tjs2/tjsMath.cpp
common/tjs2/tjsMessage.cpp
common/tjs2/tjsMT19937ar-cok.cpp
common/tjs2/tjsNamespace.cpp
common/tjs2/tjsNative.cpp
common/tjs2/tjsObject.cpp
common/tjs2/tjsObjectExtendable.cpp
common/tjs2/tjsOctPack.cpp
common/tjs2/tjspp.tab.cpp
common/tjs2/tjsRandomGenerator.cpp
common/tjs2/tjsRegExp.cpp
common/tjs2/tjsScriptBlock.cpp
common/tjs2/tjsScriptCache.cpp
common/tjs2/tjsSnprintf.cpp
common/tjs2/tjsString.cpp
common/tjs2/tjsUtils.cpp
common/tjs2/tjsVariant.cpp
common/tjs2/tjsVariantString.cpp
common/base/BinaryStream.cpp
common/base/CharacterSet.cpp
common/base/EventIntf.cpp
common/base/PluginIntf.cpp
common/base/ScriptMgnIntf.cpp
common/base/StorageIntf.cpp
common/base/SysInitIntf.cpp
common/base/SystemIntf.cpp
common/base/TextStream.cpp
common/base/UtilStreams.cpp
common/base/XP3Archive.cpp
common/base/StorageCache.cpp
common/environ/TouchPoint.cpp
common/extension/Extension.cpp
common/msg/MsgIntf.cpp
common/sound/MathAlgorithms.cpp
common/sound/PhaseVocoderDSP.cpp
common/sound/PhaseVocoderFilter.cpp
common/sound/RealFFT.cpp
common/sound/SoundBufferBaseIntf.cpp
common/sound/SoundBufferBaseImpl.cpp
common/sound/WaveIntf.cpp
common/sound/WaveLoopManager.cpp
common/sound/WaveSegmentQueue.cpp
common/sound/OpusCodecDecoder.cpp
common/sound/VorbisCodecDecoder.cpp
common/utils/ClipboardIntf.cpp
common/utils/cp932_uni.cpp
common/utils/DebugIntf.cpp
common/utils/md5.c
common/utils/MiscUtility.cpp
common/utils/Random.cpp
common/utils/ThreadIntf.cpp
common/utils/TimerThread.cpp
common/utils/TimerIntf.cpp
common/utils/TVPTimer.cpp
common/utils/uni_cp932.cpp
common/utils/VelocityTracker.cpp
common/visual/BitmapIntf.cpp
common/visual/BitmapLayerTreeOwner.cpp
common/visual/BitmapInfomation.cpp
common/visual/CharacterData.cpp
common/visual/ComplexRect.cpp
common/visual/DrawDevice.cpp
common/visual/FontSystem.cpp
common/visual/FreeType.cpp
common/visual/FreeTypeFontRasterizer.cpp
common/visual/GraphicsLoaderIntf.cpp
common/visual/GraphicsLoadThread.cpp
common/visual/ImageFunction.cpp
common/visual/LayerBitmapImpl.cpp
common/visual/LayerBitmapIntf.cpp
common/visual/LayerIntf.cpp
common/visual/LayerManager.cpp
common/visual/LayerTreeOwnerImpl.cpp
common/visual/LoadJPEG.cpp
common/visual/LoadPNG.cpp
common/visual/LoadTLG.cpp
common/visual/NullDrawDevice.cpp
common/visual/BitmapDrawDevice.cpp
common/visual/PrerenderedFont.cpp
common/visual/RectItf.cpp
common/visual/SaveTLG5.cpp
common/visual/SaveTLG6.cpp
common/visual/TransIntf.cpp
common/visual/tvpgl.c
common/visual/cpu_detect.cpp
common/visual/VideoOvlIntf.cpp
common/visual/WindowIntf.cpp
common/visual/KeyRepeat.cpp
common/visual/gl/blend_function.cpp
common/visual/gl/ResampleImage.cpp
common/visual/gl/WeightFunctor.cpp
common/base/FuncStubs.cpp
)

if (KRKRZ_USE_OPENGL)

set( KRKRZ_SRC_OPENGL
common/visual/opengl/OGLDrawDevice.cpp
common/visual/opengl/CanvasIntf.cpp
common/visual/opengl/GLTexture.cpp
common/visual/opengl/GLFrameBufferObject.cpp
common/visual/opengl/GLShaderUtil.cpp
common/visual/opengl/Matrix32Intf.cpp
common/visual/opengl/Matrix44Intf.cpp
common/visual/opengl/OffscreenIntf.cpp
common/visual/opengl/OpenGLError.cpp
common/visual/opengl/ShaderProgramIntf.cpp
common/visual/opengl/TextureIntf.cpp
common/visual/opengl/TextureLayerTreeOwner.cpp
common/visual/opengl/VertexBinderIntf.cpp
common/visual/opengl/VertexBufferIntf.cpp 
common/glad/src/gles2.c
common/glad/src/egl.c
)

list(APPEND KRKRZ_DEFINES
TVP_USE_OPENGL
)

set( KRKRZ_SRC_WIN32_OPENGL
common/glad/src/egl.c
common/visual/opengl/EGLContext.cpp
win32/visual/OpenGLPlatform.cpp
)

endif()

set( KRKRZ_SRC_WIN32 
win32/environ/CompatibleNativeFuncs.cpp
win32/environ/ConfigFormUnit.cpp
win32/environ/EmergencyExit.cpp
win32/environ/MouseCursor.cpp
win32/environ/SystemControl.cpp
win32/environ/TVPWindow.cpp
win32/environ/VersionFormUnit.cpp
win32/environ/WindowFormUnit.cpp
win32/environ/WindowsUtil.cpp
win32/base/EventImpl.cpp
win32/base/FileSelector.cpp
win32/base/NativeEventQueue.cpp
win32/base/PluginImpl.cpp
win32/base/SusieArchive.cpp
win32/base/ScriptMgnImpl.cpp
win32/base/StorageImpl.cpp
win32/base/SysInitImpl.cpp
win32/base/SystemImpl.cpp
win32/msg/MsgImpl.cpp
win32/msg/MsgLoad.cpp
win32/msg/ReadOptionDesc.cpp
win32/sound/tvpsnd.c
win32/sound/WaveImpl.cpp
win32/utils/ClipboardImpl.cpp
win32/utils/ThreadImpl.cpp
win32/visual/BasicDrawDevice.cpp
win32/visual/BitmapBitsAlloc.cpp
win32/visual/DInputMgn.cpp
win32/visual/DrawDeviceImpl.cpp
win32/visual/GDIFontRasterizer.cpp
win32/visual/GraphicsLoaderImpl.cpp
win32/visual/LoadJXR.cpp
win32/visual/LayerImpl.cpp
win32/visual/NativeFreeTypeFace.cpp
win32/visual/TVPScreen.cpp
win32/visual/TVPSysFont.cpp
win32/visual/VideoOvlImpl.cpp
win32/visual/VSyncTimingThread.cpp
win32/visual/WindowImpl.cpp
win32/environ/Application.cpp
win32/movie/TVPVideoOverlay.cpp

common/utils/TickCount.cpp
win32/utils/TickCountImpl.cpp

common/base/FileAllocator.cpp
generic/utils/LogImpl.cpp

win32/vcproj/tvpwin32.rc
win32/vcproj/dpi.manifest
)

# ----------------------------------------------------------------------------
# 画像処理 SIMD ソース群
#
# 以下は CPU アーキテクチャごとに自動取り込みされる:
#  - x86 / x86_64 系       → KRKRZ_SRC_X86_GRAPHICS_SIMD
#  - arm / arm64 系        → KRKRZ_SRC_ARM_GRAPHICS_SIMD
#
# WIN32 専用の Win32 API 依存ファイル(CPU検出 / サウンド SSE)は
# KRKRZ_SRC_WIN32_SSE に残し、従来通り if(WIN32) でのみ取り込む。
#
# 補足: AVX2 / SSSE3 を使うソースには非 MSVC 向けの per-file フラグが必要。
# その設定は本ファイル末尾で set_source_files_properties() している。
# ----------------------------------------------------------------------------

# x86/x86_64 共通の画像処理 SIMD (cross-platform)
set( KRKRZ_SRC_X86_GRAPHICS_SIMD
common/visual/gl/ResampleImageAVX2.cpp
common/visual/gl/ResampleImageSSE2.cpp
common/visual/gl/x86simdutil.cpp
common/visual/gl/x86simdutilAVX2.cpp
common/visual/gl/blend_function_sse2.cpp
common/visual/gl/blend_function_avx2.cpp
common/visual/gl/adjust_color_sse2.cpp
common/visual/gl/boxblur_sse2.cpp
common/visual/gl/colormap_avx2.cpp
common/visual/gl/colorfill_avx2.cpp
common/visual/gl/colorfill_sse2.cpp
common/visual/gl/colormap_sse2.cpp
common/visual/gl/pixelformat_sse2.cpp
common/visual/gl/tlg_sse2.cpp
common/visual/gl/univtrans_sse2.cpp
common/visual/IA32/detect_cpu.cpp
common/visual/IA32/tvpgl_ia32_intf.c
)

# ARM/ARM64 用の画像処理 SIMD
set( KRKRZ_SRC_ARM_GRAPHICS_SIMD
common/visual/gl/blend_function_neon.cpp
common/visual/gl/adjust_color_neon.cpp
common/visual/gl/colormap_neon.cpp
common/visual/gl/colorfill_neon.cpp
common/visual/gl/pixelformat_neon.cpp
)

# Win32 API 固有の補助ファイル。
# per-CPU 親和性スレッドで全コアの features をマスクする Win32 専用の
# 追加検出 + ロギング (Win32 API 依存、本質的に Windows 専用)。
# sound 系の x86 SSE は Phase SB0 で portable 化して KRKRZ_SRC_X86_SOUND_SIMD
# に移動済み。
set( KRKRZ_SRC_WIN32_SSE
win32/environ/DetectCPU.cpp
)

# ----------------------------------------------------------------------------
# sound 系 SIMD (cross-platform)。
# 画像処理 SIMD の KRKRZ_SRC_X86_GRAPHICS_SIMD / KRKRZ_SRC_ARM_GRAPHICS_SIMD
# と同じ構造。Linux/macOS x86_64 でも KRKRZ_TARGET_X86 経由で自動 wired。
# sound NEON は Phase SB2 で実装予定 (現状空 list)。
# ----------------------------------------------------------------------------
set( KRKRZ_SRC_X86_SOUND_SIMD
common/sound/MathAlgorithms_SSE.cpp
common/sound/RealFFT_SSE.cpp
common/sound/xmmlib.cpp
)

set( KRKRZ_SRC_ARM_SOUND_SIMD
common/sound/MathAlgorithms_NEON.cpp
common/sound/RealFFT_NEON.cpp
)

# ----------------------------------------------------------------------------
# アーキテクチャ自動判定
# ----------------------------------------------------------------------------
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _krkrz_arch_lc)
if (_krkrz_arch_lc MATCHES "^(x86_64|amd64|x64|i[3-6]86|x86)$")
    set(KRKRZ_TARGET_X86 TRUE)
endif()
if (_krkrz_arch_lc MATCHES "^(aarch64|arm64|armv[0-9]|arm)")
    set(KRKRZ_TARGET_ARM TRUE)
endif()
unset(_krkrz_arch_lc)

# 画像処理 + sound SIMD を共通ソースリストに追加 (OS 非依存、アーキテクチャに応じて)
if (KRKRZ_TARGET_X86)
    list(APPEND KRKRZ_SRC ${KRKRZ_SRC_X86_GRAPHICS_SIMD})
    list(APPEND KRKRZ_SRC ${KRKRZ_SRC_X86_SOUND_SIMD})
endif()
if (KRKRZ_TARGET_ARM)
    list(APPEND KRKRZ_SRC ${KRKRZ_SRC_ARM_GRAPHICS_SIMD})
    list(APPEND KRKRZ_SRC ${KRKRZ_SRC_ARM_SOUND_SIMD})
endif()

# ----------------------------------------------------------------------------
# 拡張命令 per-file コンパイルフラグ
#  - MSVC は SSE2/SSSE3 の intrinsics を /arch なしでも受け付けるが、AVX2 は
#    /arch:AVX2 が必要。GCC/Clang は SSSE3 / AVX2 ともに per-file の -m フラグ
#    が必要。runtime dispatch で実際に呼ばれる CPU は事前にチェックされるので、
#    ファイル単位で命令を有効化しても安全。
# ----------------------------------------------------------------------------
set(_krkrz_avx2_files
    common/visual/gl/blend_function_avx2.cpp
    common/visual/gl/ResampleImageAVX2.cpp
    common/visual/gl/x86simdutilAVX2.cpp
    common/visual/gl/colormap_avx2.cpp
    common/visual/gl/colorfill_avx2.cpp
)
if (MSVC)
    set(_krkrz_avx2_flags "/arch:AVX2")
    set(_krkrz_ssse3_flags "")
else()
    set(_krkrz_avx2_flags "-mavx2;-mfma")
    set(_krkrz_ssse3_flags "-mssse3")
endif()

if (KRKRZ_TARGET_X86)
    foreach(_f ${_krkrz_avx2_files})
        set_source_files_properties(${_f} PROPERTIES COMPILE_OPTIONS "${_krkrz_avx2_flags}")
    endforeach()
    if (_krkrz_ssse3_flags)
        # blend_function_sse2.cpp / pixelformat_sse2.cpp には SSSE3 runtime-dispatch 経路が含まれている
        set_source_files_properties(
            common/visual/gl/blend_function_sse2.cpp
            common/visual/gl/pixelformat_sse2.cpp
            PROPERTIES COMPILE_OPTIONS "${_krkrz_ssse3_flags}")
    endif()
endif()

unset(_krkrz_avx2_files)
unset(_krkrz_avx2_flags)
unset(_krkrz_ssse3_flags)


set( KRKRZ_INC_GENERIC
generic/base
generic/environ
generic/msg
generic/utils
generic/visual
)

set( KRKRZ_SRC_GENERIC
common/sound/SoundDecodeThread.cpp
common/sound/SoundEventThread.cpp
common/sound/QueueSoundBufferImpl.cpp
generic/base/DrawDeviceImpl.cpp
generic/base/EventImpl.cpp
generic/base/NativeEventQueue.cpp
generic/base/PluginImpl.cpp
generic/base/ScriptMgnImpl.cpp
generic/base/StorageImpl.cpp
generic/base/SysInitImpl.cpp
generic/base/SystemImpl.cpp
generic/environ/Application.cpp
generic/environ/FontSystemBase.cpp
generic/environ/WindowForm.cpp
generic/environ/JoyPad.cpp
generic/msg/MsgImpl.cpp
generic/msg/MsgLoad.cpp
generic/utils/ClipboardImpl.cpp
generic/visual/BitmapBitsAlloc.cpp
generic/visual/LayerImpl.cpp
generic/visual/VideoOvlImpl.cpp
generic/visual/WindowImpl.cpp
)

set(KRKRZ_PUBLIC_HEADER 
common/visual/tvpinputdefs.h
common/visual/MoviePlayer.h
generic/krkrz.h
generic/base/LocalFileSystem.h
generic/environ/WindowFormEvent.h
generic/environ/VirtualKey.h
)
list(TRANSFORM KRKRZ_PUBLIC_HEADER PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)

if (WIN32)
	list(APPEND KRKRZ_INC_GENERIC ${KRKRZ_INC_WIN32_COMMON})
	list(APPEND KRKRZ_SRC_GENERIC ${KRKRZ_SRC_WIN32_SSE})
endif()

set(KRKRZ_SRC_SDL3
	sdl3/base/FileImpl.cpp
	sdl3/base/SDL3KirikiriIOStream.cpp
	sdl3/base/SDL3KirikiriIOStream.h
	sdl3/base/SDL3KirikiriStorage.cpp
	sdl3/base/SDL3KirikiriStorage.h
	sdl3/base/storage.cpp
	sdl3/environ/app.cpp
	sdl3/environ/app.h
	sdl3/environ/form.cpp
#	sdl3/environ/joystick.cpp
	sdl3/environ/key.cpp
	sdl3/environ/main.cpp
	sdl3/environ/pad.cpp
	sdl3/utils/LogImpl.cpp
	sdl3/utils/TickCount.cpp
	sdl3/visual/SDLDrawDevice.cpp
	sdl3/visual/SDLDrawDevice.h
	sdl3/visual/SDLTextureUpdateRect.h
)

set(KRKRZ_INC_SDL3
	sdl3/base
	sdl3/environ
	sdl3/utils
	sdl3/visual
)

set(KRKRZ_LIB_SDL3
	SDL3::SDL3
)

list(APPEND KRKRZ_SRC_WIN32
	common/sound/AudioStream.cpp
)
list(APPEND KRKRZ_SRC_SDL3
	common/sound/AudioStream.cpp
	sdl3/sound/audio.cpp
)
if (KRKRZ_VARIANT STREQUAL "SDL")
	list(APPEND KRKRZ_DEFINES
		# miniaudio dont use device io
		MA_NO_DEVICE_IO
	)
endif()


if (KRKRZ_SDL3_SPLASHWINDOW)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/app_splash.cpp
	)
	list(APPEND KRKRZ_DEFINES
		USE_SPLASHWINDOW
	)
	list(APPEND KRKRZ_LIB_SDL3
		SDL3_image::SDL3_image
	)
endif()

if(WIN32)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		common/base/FileAllocator.cpp
		generic/app/movie.cpp
		generic/app/winres.cpp
		win32/utils/ThreadImpl.cpp
	)
elseif(APPLE)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		common/base/FileAllocator.cpp
		generic/app/movie.cpp
		sdl3/base/resource.cpp
		sdl3/utils/ThreadImpl.cpp
	)
elseif(ANDROID)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		common/base/FileAllocator.cpp
		generic/app/movie.cpp
		generic/app/andres.cpp
		sdl3/utils/ThreadImpl.cpp
	)
elseif(UNIX)
	list(APPEND KRKRZ_SRC_SDL3
		sdl3/environ/stdapp.cpp
		common/base/FileAllocator.cpp
		generic/app/movie.cpp
		generic/app/objres.cpp
		sdl3/utils/ThreadImpl.cpp
	)
endif()
