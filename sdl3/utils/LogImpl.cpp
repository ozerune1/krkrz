#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"

#include <SDL3/SDL.h>
#include <string>

// TVPLogLevelからSDL_LogPriorityへの変換テーブル
static SDL_LogPriority TVPLogLevelToSDLPriority(TVPLogLevel level)
{
    switch (level) {
        case TVPLOG_LEVEL_VERBOSE:  return SDL_LOG_PRIORITY_VERBOSE;
        case TVPLOG_LEVEL_DEBUG:    return SDL_LOG_PRIORITY_DEBUG;
        case TVPLOG_LEVEL_INFO:     return SDL_LOG_PRIORITY_INFO;
        case TVPLOG_LEVEL_WARNING:  return SDL_LOG_PRIORITY_WARN;
        case TVPLOG_LEVEL_ERROR:    return SDL_LOG_PRIORITY_ERROR;
        case TVPLOG_LEVEL_CRITICAL: return SDL_LOG_PRIORITY_CRITICAL;
        case TVPLOG_LEVEL_OFF:
        default:                    return SDL_LOG_PRIORITY_CRITICAL;
    }
}

static TVPLogLevel TVPSDLPriorityToLogLevel(SDL_LogPriority pri)
{
    switch (pri) {
        case SDL_LOG_PRIORITY_VERBOSE:  return TVPLOG_LEVEL_VERBOSE;
        case SDL_LOG_PRIORITY_DEBUG:    return TVPLOG_LEVEL_DEBUG;
        case SDL_LOG_PRIORITY_INFO:     return TVPLOG_LEVEL_INFO;
        case SDL_LOG_PRIORITY_WARN:     return TVPLOG_LEVEL_WARNING;
        case SDL_LOG_PRIORITY_ERROR:    return TVPLOG_LEVEL_ERROR;
        case SDL_LOG_PRIORITY_CRITICAL: return TVPLOG_LEVEL_CRITICAL;
        default:                        return TVPLOG_LEVEL_INFO;
    }
}

void TVPLogSetLevel(TVPLogLevel logLevel)
{
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, priority);
    SDL_SetLogPriority(SDL_LOG_CATEGORY_SYSTEM, priority);
}

//---------------------------------------------------------------------------
// SDL のログ出力関数を差し替え、受けとった UTF-8 本文 (タイムスタンプ無し)
// をそのまま LogCore の TVPLogDispatchLine に渡す。コンソール/ファイル/
// キャッシュ/REPL sink は LogCore が処理する。
//---------------------------------------------------------------------------
static void SDLCALL TVPSDLLogOutput(void * /*userdata*/, int /*category*/, SDL_LogPriority priority, const char *message)
{
    if (!message) return;
    TVPLogDispatchLine(TVPSDLPriorityToLogLevel(priority), message);
}

void TVPLogInit(TVPLogLevel logLevel)
{
    SDL_SetLogOutputFunction(TVPSDLLogOutput, nullptr);
    TVPLogSetLevel(logLevel);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "TVP Log system initialized with level: %d", logLevel);
}

void TVPLog(TVPLogLevel logLevel, const char *file, int line, const char *func, const char *format, tvpfmt::format_args args)
{
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel);

    std::string msg;
    try {
        msg = tvpfmt::vformat(format, args);
    } catch (const tvpfmt::format_error& e) {
        msg = "Log Format error: " + std::string(e.what());
    }
    if (file && func) {
        const char* fileName = file;
        const char* lastSlash = strrchr(file, '/');
        const char* lastBackslash = strrchr(file, '\\');
        if (lastSlash != nullptr || lastBackslash != nullptr) {
            if (lastSlash < lastBackslash || lastSlash == nullptr) {
                fileName = lastBackslash + 1;
            } else {
                fileName = lastSlash + 1;
            }
        }
        SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "[%s:%s:%d] %s", fileName, func, line, msg.c_str());
    } else {
        SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%s", msg.c_str());
    }
}

void TVPLogMsg(TVPLogLevel logLevel, const char *msg)
{
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel);
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%s", msg);
}
