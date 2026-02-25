//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Utilities for Logging
//---------------------------------------------------------------------------
#ifndef LogIntfH
#define LogIntfH

#include "tjsNative.h"
#include "CharacterSet.h"

//---------------------------------------------------------------------------
// log functions
//---------------------------------------------------------------------------

/*[*/

#include <fmt/core.h>


template <>
struct fmt::formatter<ttstr> {
    constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename FormatContext>
    auto format(const ttstr &msg, FormatContext& ctx) const{
        std::string utf8Str;
        TVPUtf16ToUtf8(utf8Str, msg.c_str());
        return fmt::format_to(ctx.out(), "{}", utf8Str);
    }
};

template <>
struct fmt::formatter<tjs_string> {
    constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    template <typename FormatContext>
    auto format(const tjs_string &msg, FormatContext& ctx) const{
        std::string utf8Str;
        TVPUtf16ToUtf8(utf8Str, msg.c_str());
        return fmt::format_to(ctx.out(), "{}", utf8Str);
    }
};

enum TVPLogLevel {
    TVPLOG_LEVEL_VERBOSE = 0,
    TVPLOG_LEVEL_DEBUG = 1,
    TVPLOG_LEVEL_INFO = 2,
    TVPLOG_LEVEL_WARNING = 3,
    TVPLOG_LEVEL_ERROR = 4,
    TVPLOG_LEVEL_CRITICAL = 5,
    TVPLOG_LEVEL_OFF = 6
};

// コンパイル時に定義されるログレベル
#ifndef TVPLOG_LEVEL
#ifdef MASTER
#define TVPLOG_LEVEL TVPLOG_LEVEL_WARNING
#else
#ifdef NDEBUG
#define TVPLOG_LEVEL TVPLOG_LEVEL_INFO
#else
#define TVPLOG_LEVEL TVPLOG_LEVEL_DEBUG
#endif
#endif
#endif

/*]*/

void TVPLogInit(TVPLogLevel logLevel);
TJS_EXP_FUNC_DEF(void, TVPLogSetLevel, (TVPLogLevel logLevel));
TJS_EXP_FUNC_DEF(void, TVPLog, (TVPLogLevel logLevel, const char *file, int line, const char *func, const char *fmt, fmt::format_args args));
TJS_EXP_FUNC_DEF(void, TVPLogMsg, (TVPLogLevel logLevel, const char *msg));

/*[*/

// ログマクロ実装のヘルパー
#define TVPLOG_IMPL(level, format, ...) do{ TVPLog(level, __FILE__, __LINE__, __FUNCTION__, format, fmt::make_format_args(__VA_ARGS__)); } while(0)

// ログレベルに応じたマクロ定義
#if TVPLOG_LEVEL <= TVPLOG_LEVEL_VERBOSE
#define TVPLOG_VERBOSE(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_VERBOSE, format, __VA_ARGS__)
#else
#define TVPLOG_VERBOSE(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_DEBUG
#define TVPLOG_DEBUG(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_DEBUG, format, __VA_ARGS__)
#else
#define TVPLOG_DEBUG(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_INFO
#define TVPLOG_INFO(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_INFO, format, __VA_ARGS__)
#else
#define TVPLOG_INFO(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_WARNING
#define TVPLOG_WARNING(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_WARNING, format, __VA_ARGS__)
#else
#define TVPLOG_WARNING(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_ERROR
#define TVPLOG_ERROR(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_ERROR, format, __VA_ARGS__)
#else
#define TVPLOG_ERROR(...) do {} while(0)
#endif

#if TVPLOG_LEVEL <= TVPLOG_LEVEL_CRITICAL
#define TVPLOG_CRITICAL(format, ...) TVPLOG_IMPL(TVPLOG_LEVEL_CRITICAL, format, __VA_ARGS__)
#else
#define TVPLOG_CRITICAL(...) do {} while(0)
#endif

// 汎用ログマクロ（ログレベルを指定可能）
#define TJSLOG(level, format, ...) do { \
    if (level >= TVPLOG_LEVEL) { \
        TVPLOG_IMPL(level, format, __VA_ARGS__); \
    } \
} while(0)

/*]*/

#endif
//---------------------------------------------------------------------------
