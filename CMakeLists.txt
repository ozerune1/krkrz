cmake_minimum_required(VERSION 3.20)

# UTF-8 source
add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C,CXX>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")
# ignore warnings
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C,CXX>>:/wd4819>")
add_compile_definitions("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C,CXX>>:_CRT_SECURE_NO_WARNINGS>")
# force output PDB
add_link_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C,CXX>>:/DEBUG>")


set(PROJECT_NAME krkrz)
set(PROJECT_VERSION 2.0.0)

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION})

option(MASTER "build master version" OFF)

option(KRKRZ_USE_SJIS "use sjis" OFF)
option(KRKRZ_USE_OPENGL "use opengl" ON)
set(KRKRZ_VARIANT "SDL" CACHE STRING "Build variant: SDL (default), WIN (Windows native), LIB (static library)")
option(KRKRZ_BUILD_TESTS "build SIMD parity tests" ON)
option(KRKRZ_SDL3_SPLASHWINDOW "use splash window" OFF)

if(WIN32 AND (CMAKE_SIZEOF_VOID_P EQUAL 8))
set(WIN64 1)
endif()

if(WIN32)
if(WIN64)
set(EXENAME ${PROJECT_NAME}64)
set(LIBNAME lib${PROJECT_NAME})
set(PLUGIN_DIR "plugin64")
else()
set(EXENAME ${PROJECT_NAME})
set(LIBNAME lib${PROJECT_NAME}32)
set(PLUGIN_DIR "plugin")
endif()
else()
set(EXENAME ${PROJECT_NAME})
set(LIBNAME lib${PROJECT_NAME})
set(PLUGIN_DIR "plugin")
endif()

if(APPLE)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
set(KRKRZ_APPLE_BUNDLE_RUNTIME_DIR "${EXENAME}.app")
else()
set(KRKRZ_APPLE_BUNDLE_RUNTIME_DIR "${EXENAME}.app/Contents/MacOS")
endif()
set(KRKRZ_PLUGIN_INSTALL_DIR "${KRKRZ_APPLE_BUNDLE_RUNTIME_DIR}")
else()
set(KRKRZ_PLUGIN_INSTALL_DIR "${PLUGIN_DIR}")
endif()

if (WIN32)
set(CMAKE_RC_FLAGS "")
endif()

set(KRKRZ_DEFINES
TJS_JP_LOCALIZED
TVP_ENABLE_EXECUTE_AT_EXCEPTION
TVP_LOG_TO_COMMANDLINE_CONSOLE
UNICODE
_UNICODE
# ファイル名をノーマライズしない
#TVP_NO_NORMALIZE_PATH
# ファイル検索時はケース無視
TVP_AUTOPATH_IGNORECASE
# ローカルパス参照のcaseを強制参照
#TVP_LOCALFILE_FORCE_CASESENSITIVE
__CODEGUARD__
)

if (MASTER)
list(APPEND KRKRZ_DEFINES
MASTER
)
endif()

if (KRKRZ_USE_SJIS)
list(APPEND KRKRZ_DEFINES
# 標準をSJISにするかどうか
TVP_TEXT_READ_ANSI_MBCS
)
endif()

if (WIN32)
list(APPEND KRKRZ_DEFINES
TJS_TEXT_OUT_CRLF
)
endif()

include(sources.cmake)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/win32/custom.cmake")
message(STATUS "add custom system")
include(${CMAKE_CURRENT_SOURCE_DIR}/win32/custom.cmake)
endif()

# ----------------------------------------------------
# libkrkrz plugin
# ----------------------------------------------------

if (NOT TARGET libkrkrzplugin)

if (KRKRZ_LIBPLUGIN_FOLDER AND EXISTS "${KRKRZ_LIBPLUGIN_FOLDER}/CMakeLists.txt")
set(KRKRZ_SRC_FOLDER ${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(${KRKRZ_LIBPLUGIN_FOLDER} ${CMAKE_CURRENT_BINARY_DIR}/krkrz_libplugin)
elseif (EXISTS "${CMAKE_SOURCE_DIR}/krkrz_libplugin/CMakeLists.txt")
set(KRKRZ_SRC_FOLDER ${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(${CMAKE_SOURCE_DIR}/krkrz_libplugin ${CMAKE_CURRENT_BINARY_DIR}/krkrz_libplugin)
endif()

endif()

if (TARGET libkrkrzplugin)

message("krkrz with libkrkrplugin")

list(APPEND KRKRZ_DEFINES
	USE_LIBPLUGIN
)
list(APPEND KRKRZ_LIB_ADD
	libkrkrzplugin
)
endif()

# ----------------------------------------------------
# plugins
#
# TVP_PLUGIN_FOLDERS  プラグイン参照先フォルダのリスト
# TVP_PLUGINS         同時作成するプラグインのリスト
# TVP_PLUGINS_STATIC  同時作成するプラグインのうち吉里吉里本体に含めるプラグインのリスト
#
# STATIC 対象プラグインは TVP_STATIC_PLUGIN 定義時に krkr_plugin_XXX 関数をエクスポートする必要あり
# ----------------------------------------------------

# StaticPlugins.cpp 生成用の変数を初期化
set(STATIC_PLUGIN_EXTERNS "")
set(STATIC_PLUGIN_ATSTARTS "")
set(KRKRZ_DYNAMIC_PLUGINS "")

foreach(PLUGIN ${TVP_PLUGINS})
    if(${PLUGIN} IN_LIST TVP_PLUGINS_STATIC)
        set(TVP_STATIC_PLUGIN ON)
    else()
        set(TVP_STATIC_PLUGIN OFF)
    endif()

    # TVP_PLUGIN_FOLDERS から該当プラグインのフォルダを検索
    set(PLUGIN_SOURCE_DIR "")
    foreach(FOLDER ${TVP_PLUGIN_FOLDERS})
        if(EXISTS "${FOLDER}/${PLUGIN}")
            set(PLUGIN_SOURCE_DIR "${FOLDER}/${PLUGIN}")
            break()
        endif()
    endforeach()

    if(NOT PLUGIN_SOURCE_DIR)
        message(FATAL_ERROR "Plugin '${PLUGIN}' not found in any of: ${PLUGIN_FOLDERS}")
    endif()

    add_subdirectory(${PLUGIN_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/plugins/${PLUGIN})

    if(TVP_STATIC_PLUGIN)
        message(STATUS "static  plugin: ${PLUGIN}")
        list(APPEND KRKRZ_LIB_ADD ${PLUGIN})
        # StaticPlugins.cpp 用のデータを生成
        string(APPEND STATIC_PLUGIN_EXTERNS "extern \"C\" void krkrz_plugin_${PLUGIN}();\n")
        string(APPEND STATIC_PLUGIN_ATSTARTS "static tTVPAtStart AtStart_${PLUGIN}(TVP_ATSTART_PRI_PREPARE, [](){ krkrz_plugin_${PLUGIN}(); });\n")
    else()
        message(STATUS "dynamic plugin: ${PLUGIN}")
		list(APPEND KRKRZ_DYNAMIC_PLUGINS ${PLUGIN})
		if(NOT APPLE)
        install(TARGETS ${PLUGIN} 
			LIBRARY DESTINATION ${KRKRZ_PLUGIN_INSTALL_DIR}
			RUNTIME DESTINATION ${KRKRZ_PLUGIN_INSTALL_DIR}
        )
		endif()
    endif()
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND KRKRZ_DYNAMIC_PLUGINS)
message(WARNING "iOS restricts runtime loading of bundled dynamic libraries; prefer TVP_PLUGINS_STATIC for app plugins")
endif()

# Static Pluginが存在する場合のみ StaticPlugins.cpp を生成してソースに追加
if(STATIC_PLUGIN_EXTERNS)
	set(STATIC_PLUGINS_CPP_CONTENT "#include \"tjsCommHead.h\"\n#include \"SysInitIntf.h\"\n${STATIC_PLUGIN_EXTERNS}${STATIC_PLUGIN_ATSTARTS}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/StaticPlugins.cpp" "${STATIC_PLUGINS_CPP_CONTENT}")
    list(APPEND KRKRZ_SRC_ADD "${CMAKE_CURRENT_BINARY_DIR}/StaticPlugins.cpp")
	message(STATUS "Generated StaticPlugins.cpp with static plugins")
	# 共通バインド部
	list(APPEND KRKRZ_INC ${TPSTUB_DIR})
	list(APPEND KRKRZ_SRC_ADD ${TPSTUB_DIR}/tp_stub.cpp)
endif()

# ----------------------------------------------------
# libraries
# ----------------------------------------------------

if (NOT TARGET ZLIB::ZLIB)
	find_package(ZLIB REQUIRED)
endif()
if (NOT TARGET freetype)
	find_package(freetype CONFIG REQUIRED)
endif()
if (NOT TARGET Ogg::ogg)
	find_package(Ogg CONFIG REQUIRED)
endif()
if (NOT TARGET Vorbis::vorbis)
find_package(Vorbis CONFIG REQUIRED)
endif()
find_package(PNG MODULE)
find_package(Opus)
find_package(oniguruma CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG)

# libjpeg-turbo::turbojpeg が見つからない場合
if (NOT libjpeg-turbo_FOUND)
	message("libjpeg-turbo not found, trying to find libjpeg")
	find_package(JPEG QUIET)
else()
	message("libjpeg-turbo found")
	set(JPEG_INCLUDE_DIR ${libjpeg-turbo_INCLUDE_DIRS})
	set(JPEG_LIBRARIES $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>)
	set(JPEG_TURBO_FOUND TRUE)
	list(APPEND KRKRZ_DEFINES TVP_USE_TURBO_JPEG_API)
endif()

find_package(OpusFile CONFIG REQUIRED)

if (0)
find_library(OPUSFILE NAMES opusfile)
if(OPUSFILE_FOUND AND NOT TARGET opusfile)
  find_path(OPUSFILE_INCLUDE_DIRS "opus/opusfile.h")
  add_library(opusfile UNKNOWN IMPORTED)
  set_target_properties(opusfile PROPERTIES
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${OPUSFILE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPUSFILE_INCLUDE_DIR}"
    )
endif()
endif()

find_path(PICOJSON_INCLUDE_DIRS "picojson/picojson.h")
find_package(fmt CONFIG REQUIRED)

if (KRKRZ_VARIANT STREQUAL "LIB")

# ----------------------------------------------------
# krkrz generic library
# ----------------------------------------------------

message("krkrz generic library")

add_library( ${LIBNAME} EXCLUDE_FROM_ALL STATIC
	${KRKRZ_SRC} 
	${KRKRZ_SRC_OPENGL} 
	${KRKRZ_SRC_GENERIC} 
	${KRKRZ_SRC_PLATFORM}
	${KRKRZ_SRC_ADD}
)

set_target_properties(${LIBNAME} PROPERTIES 
    PUBLIC_HEADER "${KRKRZ_PUBLIC_HEADER}"
#   DEBUG_POSTFIX "_d"
)

target_include_directories( ${LIBNAME} PUBLIC
	${KRKRZ_INC}
	${KRKRZ_INC_GENERIC}
	${KRKRZ_INC_PLATFORM}
)

target_include_directories( ${LIBNAME} PRIVATE 
	${JPEG_INCLUDE_DIR}
	${PICOJSON_INCLUDE_DIRS}
)

target_compile_definitions( ${LIBNAME} PUBLIC
	${KRKRZ_DEFINES}
	__GENERIC__
	TVP_DONT_AUTOLOAD_PROVINCE
	TVP_DONT_AUTOLOAD_MASK
	${KRKRZ_DEFINES_PLATFORM}  
	${KRKRZ_DEFINES_OPENGL}
)

target_link_libraries( ${LIBNAME} PUBLIC
	freetype
	${JPEG_LIBRARIES}
	Ogg::ogg
	Vorbis::vorbis
	Vorbis::vorbisenc 
	Vorbis::vorbisfile
	ZLIB::ZLIB
	oniguruma::onig
	fmt::fmt
	PNG::PNG
	glm::glm
	$<IF:$<TARGET_EXISTS:Opus::opus>,Opus::opus,>
	$<IF:$<TARGET_EXISTS:OpusFile::opusfile>,OpusFile::opusfile,>
	$<IF:$<TARGET_EXISTS:opusfile>,opusfile,>
	${KRKRZ_LIB_ADD}
)

elseif(KRKRZ_VARIANT STREQUAL "SDL") # !LIB

# ----------------------------------------------------
# krkrz SDL version
# ----------------------------------------------------

list(APPEND KRKRZ_DEFINES
# ファイル名をノーマライズしない
#TVP_NO_NORMALIZE_PATH
)

# ----------------------------------------------------
# SDL3

if (NOT TARGET SDL3::SDL3)
	include(FetchContent)
	FetchContent_Declare(
		SDL3
		GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
		GIT_TAG main
	)
	FetchContent_Declare(
		SDL3_image
		GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
		GIT_TAG main
	)

	set(SDL_SHARED ON CACHE BOOL "Build SDL shared library")
	set(SDL_STATIC OFF CACHE BOOL "Build SDL static library")
	
	FetchContent_MakeAvailable(SDL3)

	if (KRKRZ_SDL3_SPLASHWINDOW)
	FetchContent_MakeAvailable(SDL3_image)
	endif()

endif()

if (KRKRZ_SDL3_SPLASHWINDOW)
if (NOT TARGET SDL3_image::SDL3_image)
	find_package(SDL3_image REQUIRED CONFIG)
endif()
endif()

# ----------------------------------------------------
# movieplayer

if(NOT TARGET movieplayer)
option(MOVIEPLAYER_EXTERNAL_MINIAUDIO "use external miniaudio" ON)
add_subdirectory(external/movie-player)
endif()

# ----------------------------------------------------
# resource

# リソースデータ一式
set(RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resource)
file(GLOB RESOURCE_FILES "${RESOURCE_DIR}/*")

if (WIN32)

# `.rc` ファイルを生成
set(RC_FILE ${CMAKE_BINARY_DIR}/resources.rc)
file(WRITE ${RC_FILE} "")

foreach(RESOURCE ${RESOURCE_FILES})
    get_filename_component(NAME ${RESOURCE} NAME)
    file(APPEND ${RC_FILE} "${NAME} BINARY \"${RESOURCE}\"\n")
endforeach()

elseif(APPLE)

# iOS または macOS の場合、リソースをアプリバンドルに追加
foreach(RESOURCE ${RESOURCE_FILES})
	set_source_files_properties(${RESOURCE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endforeach()

elseif(ANDROID)

# install でファイルをアプリに一式コピーする
# LINUX誤認防止で先に定義

elseif(UNIX)

# objcopy でバイナリを埋め込む
# objcopy のコマンドを探す
find_program(OBJCOPY objcopy)

# リソースオブジェクト出力ディレクトリ
set(RESOURCE_OBJ_DIR ${CMAKE_BINARY_DIR}/resource_obj)
file(MAKE_DIRECTORY ${RESOURCE_OBJ_DIR})

# リソースレジストリソースファイルの内容を生成
set(REGISTRY_SOURCE_CONTENT "/* Auto-generated resource registry source */
#include <stddef.h>
#include <string.h>

/* External symbols generated by objcopy */
")
set(REGISTRY_ENTRIES "")
set(REGISTRY_COUNT 0)
# objcopy の BFD target を CPU アーキに合わせて選択する
# (-I binary で取り込んだ生データを ELF オブジェクトに変換する際の出力フォーマット)
# CACHE にしてしまうと configure をまたいでアーキの異なる値が焼き付いたまま残り
# (-O elf64-x86-64 なのに -B aarch64 のような食い違いが出る) ため、毎 configure で
# KRKRZ_TARGET_ARM / CMAKE_SIZEOF_VOID_P を見て再計算する。明示的に上書きしたい場合は
# -DOBJFORMAT=... / -DOBJARCH=... を指定する。
if (KRKRZ_TARGET_ARM)
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OBJFORMAT_DEFAULT elf64-littleaarch64)
        set(OBJARCH_DEFAULT aarch64)
    else()
        set(OBJFORMAT_DEFAULT elf32-littlearm)
        set(OBJARCH_DEFAULT arm)
    endif()
else()
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OBJFORMAT_DEFAULT elf64-x86-64)
        set(OBJARCH_DEFAULT i386:x86-64)
    else()
        set(OBJFORMAT_DEFAULT elf32-i386)
        set(OBJARCH_DEFAULT i386)
    endif()
endif()
if (NOT DEFINED OBJFORMAT)
    set(OBJFORMAT ${OBJFORMAT_DEFAULT})
endif()
if (NOT DEFINED OBJARCH)
    set(OBJARCH ${OBJARCH_DEFAULT})
endif()
message(STATUS "Resource objcopy: -O ${OBJFORMAT} -B ${OBJARCH}")

# 各バイナリをオブジェクトファイルに変換 & レジストリエントリ生成
foreach(RESOURCE ${RESOURCE_FILES})
    get_filename_component(NAME ${RESOURCE} NAME)
    set(OBJ_FILE ${RESOURCE_OBJ_DIR}/${NAME}.o)
    
    # ファイル名をシンボル名に変換（特殊文字を_に置換）
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" SYMBOL_NAME "${NAME}")
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" ORIG_NAME "${RESOURCE}")

    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${OBJCOPY} -I binary -O ${OBJFORMAT} -B ${OBJARCH}
            --redefine-sym _binary_${ORIG_NAME}_start=_binary_${SYMBOL_NAME}_start
            --redefine-sym _binary_${ORIG_NAME}_end=_binary_${SYMBOL_NAME}_end
            --redefine-sym _binary_${ORIG_NAME}_size=_binary_${SYMBOL_NAME}_size
            ${RESOURCE} ${OBJ_FILE}
        DEPENDS ${RESOURCE}
        WORKING_DIRECTORY ${RESOURCE_DIR}
        COMMENT "Converting ${NAME} to object file"
    )

    list(APPEND RESOURCE_OBJECTS ${OBJ_FILE})

    # レジストリエントリを追加
    set(REGISTRY_SOURCE_CONTENT "${REGISTRY_SOURCE_CONTENT}
extern const char _binary_${SYMBOL_NAME}_start[];
extern const char _binary_${SYMBOL_NAME}_end[];
")
    set(REGISTRY_ENTRIES "${REGISTRY_ENTRIES}    { \"${NAME}\", _binary_${SYMBOL_NAME}_start, _binary_${SYMBOL_NAME}_end },\n")
    math(EXPR REGISTRY_COUNT "${REGISTRY_COUNT} + 1")
endforeach()

# リソースレジストリソースファイルを出力
set(RESOURCE_REGISTRY_C ${RESOURCE_OBJ_DIR}/resource_registry.c)
set(REGISTRY_SOURCE_CONTENT "${REGISTRY_SOURCE_CONTENT}
/* Resource entry structure */
typedef struct {
    const char* name;
    const char* start;
    const char* end;
} ResourceEntry;

/* Resource table */
const ResourceEntry resource_table[] = {
${REGISTRY_ENTRIES}};

const size_t resource_count = ${REGISTRY_COUNT};

")
file(WRITE "${RESOURCE_REGISTRY_C}" "${REGISTRY_SOURCE_CONTENT}")
message(STATUS "Generated resource registry: ${RESOURCE_REGISTRY_C}")

set(RESOURCE_LIB resources)

# リソースを含むライブラリを作成
add_library(${RESOURCE_LIB} STATIC ${RESOURCE_OBJECTS} ${RESOURCE_REGISTRY_C})
set_property(TARGET ${RESOURCE_LIB} PROPERTY LINKER_LANGUAGE C)

endif()

# ----------------------------------------------------
# executable

if(ANDROID)

# SDL applications need to be built as a shared library
function(add_executable TARGET)
	add_library(${TARGET} SHARED ${ARGN})
endfunction()

endif()

if(APPLE)

add_executable(${EXENAME} MACOSX_BUNDLE
	${KRKRZ_SRC} 
	${KRKRZ_SRC_OPENGL} 
	${KRKRZ_SRC_GENERIC} 
	${KRKRZ_SRC_SDL3}
	${KRKRZ_SRC_PLATFORM}
	${KRKRZ_SRC_ADD}
	${RESOURCE_FILES}
)

else()

add_executable(${EXENAME}
	${KRKRZ_SRC} 
	${KRKRZ_SRC_OPENGL} 
	${KRKRZ_SRC_GENERIC} 
	${KRKRZ_SRC_SDL3}
	${KRKRZ_SRC_PLATFORM}
	${KRKRZ_SRC_ADD}
	${RC_FILE}
)

endif()

target_include_directories(${EXENAME} PRIVATE
	${KRKRZ_INC}
	${KRKRZ_INC_GENERIC}
	${KRKRZ_INC_SDL3}
	${KRKRZ_INC_PLATFORM}
	${JPEG_INCLUDE_DIR}
	${PICOJSON_INCLUDE_DIRS}
)

target_compile_definitions(${EXENAME} PRIVATE
	${KRKRZ_DEFINES}
	__GENERIC__
	TVP_DONT_AUTOLOAD_PROVINCE
	TVP_DONT_AUTOLOAD_MASK
	${KRKRZ_DEFINES_PLATFORM}  
	${KRKRZ_DEFINES_OPENGL}
)

target_link_libraries( ${EXENAME} PRIVATE
	freetype
	${JPEG_LIBRARIES}
	Ogg::ogg
	Vorbis::vorbis
	Vorbis::vorbisenc 
	Vorbis::vorbisfile
	ZLIB::ZLIB
	oniguruma::onig
	fmt::fmt
	PNG::PNG
	glm::glm
	$<IF:$<TARGET_EXISTS:Opus::opus>,Opus::opus,>
	$<IF:$<TARGET_EXISTS:OpusFile::opusfile>,OpusFile::opusfile,>
	$<IF:$<TARGET_EXISTS:opusfile>,opusfile,>
	$<IF:$<TARGET_EXISTS:movieplayer>,movieplayer,>
	${KRKRZ_LIB_SDL3}
	${KRKRZ_LIB_ADD}
	${KRKRZ_LIB_PLATFORM}
	${RESOURCE_LIB}
)

if(APPLE)
foreach(PLUGIN ${KRKRZ_DYNAMIC_PLUGINS})
	add_dependencies(${EXENAME} ${PLUGIN})
	add_custom_command(TARGET ${EXENAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:${PLUGIN}>
			"$<TARGET_FILE_DIR:${EXENAME}>/"
	)
endforeach()
endif()

# ----------------------------------------------------
# install

if (ANDROID)

elseif(SDL_SHARED)

	# 実行ディレクトリの場所にコピー
	foreach(LIB ${KRKRZ_LIB_SDL3})
		add_custom_command(TARGET ${EXENAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				$<TARGET_FILE:${LIB}>
				$<TARGET_FILE_DIR:${EXENAME}>
		)		
	endforeach()

	install(TARGETS ${EXENAME}
		RUNTIME DESTINATION .
		LIBRARY DESTINATION .
		BUNDLE DESTINATION .
	)

	if (!APPLE)
		# SDL3 の DLL も一緒にインストール
		foreach(LIB ${KRKRZ_LIB_SDL3})
			install(FILES $<TARGET_FILE:${LIB}> DESTINATION .)
		endforeach()
	endif()

else()

	install(TARGETS ${EXENAME}
			RUNTIME DESTINATION .
			BUNDLE DESTINATION .
	)

endif()

elseif(KRKRZ_VARIANT STREQUAL "WIN") # !SDL

# ----------------------------------------------------
# krkrz original win32 version
# ----------------------------------------------------

if (WIN32)
message("krkrz original based win32 executable")
else()
message(FATAL_ERROR "Unsupported platform")
endif()

# krmovie
add_subdirectory( win32/movie )
set_target_properties(krmovie PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# krkrz executable

# for LogImp.cpp
find_package(plog CONFIG)

add_executable( ${EXENAME} WIN32
	${KRKRZ_SRC} 
	${KRKRZ_SRC_OPENGL} 
	${KRKRZ_SRC_WIN32} 
	${KRKRZ_SRC_WIN32_SSE} 
	${KRKRZ_SRC_WIN32_OPENGL} 
	${KRKRZ_SRC_ADD}
)

target_include_directories( ${EXENAME} PRIVATE 
	${KRKRZ_INC}
	${KRKRZ_INC_WIN32} 
	${JPEG_INCLUDE_DIR}
	${PICOJSON_INCLUDE_DIRS}
)

target_compile_definitions( ${EXENAME} PRIVATE
	${KRKRZ_DEFINES}
	NO_STRICT
	__WINVER__
	TJS_DEBUG_DUMP_STRING
	TVP_REPORT_HW_EXCEPTION
	${KRKRZ_DEFINES_PLATFORM}  
	${KRKRZ_DEFINES_OPENGL}
)

target_link_libraries( ${EXENAME} PRIVATE
	freetype
	${JPEG_LIBRARIES}
	OpusFile::opusfile
	Ogg::ogg
	Opus::opus
	Vorbis::vorbis
	Vorbis::vorbisenc 
	Vorbis::vorbisfile
	ZLIB::ZLIB
	oniguruma::onig
	fmt::fmt
	PNG::PNG
	glm::glm
 	plog::plog
	${KRKRZ_LIB_ADD}		
)

install(TARGETS ${EXENAME}
	DESTINATION .
)

install(TARGETS krmovie
	LIBRARY DESTINATION ${PLUGIN_DIR}
	RUNTIME DESTINATION ${PLUGIN_DIR}
)

endif() # KRKRZ_VARIANT

# ----------------------------------------------------
# tests (Phase 3: SIMD パリティテスト)
# ----------------------------------------------------
if (KRKRZ_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
