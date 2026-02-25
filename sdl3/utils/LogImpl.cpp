#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"

#include <SDL3/SDL.h>
#include <string>
#include <vector>

// TVPLogLevelからSDL_LogPriorityへの変換テーブル
static SDL_LogPriority TVPLogLevelToSDLPriority(TVPLogLevel level)
{
    switch (level) {
        case TVPLOG_LEVEL_VERBOSE:
            return SDL_LOG_PRIORITY_VERBOSE;
        case TVPLOG_LEVEL_DEBUG:
            return SDL_LOG_PRIORITY_DEBUG;
        case TVPLOG_LEVEL_INFO:
            return SDL_LOG_PRIORITY_INFO;
        case TVPLOG_LEVEL_WARNING:
            return SDL_LOG_PRIORITY_WARN;
        case TVPLOG_LEVEL_ERROR:
            return SDL_LOG_PRIORITY_ERROR;
        case TVPLOG_LEVEL_CRITICAL:
            return SDL_LOG_PRIORITY_CRITICAL;
        case TVPLOG_LEVEL_OFF:
        default:
            return SDL_LOG_PRIORITY_CRITICAL;
    }
}

void TVPLogSetLevel(TVPLogLevel logLevel)
{
    // SDL3のログレベルも設定
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel);
    
    // すべてのカテゴリに対してログレベルを設定
    // SDL_LOG_CATEGORY_APPLICATIONはアプリケーション専用
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, priority);
    
    // システム全体のログレベルも設定（必要に応じて）
    SDL_SetLogPriority(SDL_LOG_CATEGORY_SYSTEM, priority);
}

void TVPLogInit(TVPLogLevel logLevel) 
{
    // SDL3のログシステムを初期化
    // SDL3ではSDL_Initの際に自動的にログシステムも初期化される
    
    // カスタムログ出力関数を設定（オプション）
    // デフォルトではSDL3が標準出力に出力する
    
    // ログレベルを設定
    TVPLogSetLevel(logLevel);
    
    // 初期化完了をログ出力
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "TVP Log system initialized with level: %d", logLevel);
}

void TVPLog(TVPLogLevel logLevel, const char *file, int line, const char *func, const char *format, fmt::format_args args)
{
    // SDL3のログレベルに変換
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel);

    std::string msg;
    try {
        msg = fmt::vformat(format, args);
    } catch (const fmt::format_error& e) {
        msg = "Log Format error: " + std::string(e.what());
    }
    // ファイル名、行番号、関数名を含む詳細なログメッセージを作成
    if (file && func) {
        // Extract just the filename from the full path
        const char* fileName = file;
        // Find last path separator (handles both / and \)
        const char* lastSlash = strrchr(file, '/');
        const char* lastBackslash = strrchr(file, '\\');
        // Use whichever separator was found last
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
   // SDL3のログレベルに変換
    SDL_LogPriority priority = TVPLogLevelToSDLPriority(logLevel); 
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%s", msg);
}
