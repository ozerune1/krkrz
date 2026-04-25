#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "LogIntf.h"

#include <plog/Log.h>
#include <plog/Init.h>
#include <plog/Formatters/MessageOnlyFormatter.h>
#include <plog/Appenders/IAppender.h>
#include <plog/Util.h>

//---------------------------------------------------------------------------
// TJSログレベルから plog のログレベルに変換する関数
//---------------------------------------------------------------------------
static plog::Severity TVPLogLevelToPlogSeverity(TVPLogLevel logLevel)
{
    switch (logLevel) {
        case TVPLOG_LEVEL_VERBOSE:  return plog::verbose;
        case TVPLOG_LEVEL_DEBUG:    return plog::debug;
        case TVPLOG_LEVEL_INFO:     return plog::info;
        case TVPLOG_LEVEL_WARNING:  return plog::warning;
        case TVPLOG_LEVEL_ERROR:    return plog::error;
        case TVPLOG_LEVEL_CRITICAL: return plog::fatal;
        default:                    return plog::none;
    }
}

static TVPLogLevel TVPPlogSeverityToLogLevel(plog::Severity s)
{
    switch (s) {
        case plog::verbose: return TVPLOG_LEVEL_VERBOSE;
        case plog::debug:   return TVPLOG_LEVEL_DEBUG;
        case plog::info:    return TVPLOG_LEVEL_INFO;
        case plog::warning: return TVPLOG_LEVEL_WARNING;
        case plog::error:   return TVPLOG_LEVEL_ERROR;
        case plog::fatal:   return TVPLOG_LEVEL_CRITICAL;
        default:            return TVPLOG_LEVEL_OFF;
    }
}

void TVPLogSetLevel(TVPLogLevel logLevel)
{
    auto logger = plog::get();
    if (logger) {
        logger->setMaxSeverity(TVPLogLevelToPlogSeverity(logLevel));
    }
}

void TVPLog(TVPLogLevel logLevel, const char *file, int line, const char *func, const char *format, tvpfmt::format_args args)
{
    auto logger = plog::get();
    if (logger) {
        plog::Record record(TVPLogLevelToPlogSeverity(logLevel), func, line, file, 0, 0);
        std::string msg;
        try {
            msg = tvpfmt::vformat(format, args);
        } catch (const tvpfmt::format_error& e) {
            msg = "Log Format error: " + std::string(e.what());
        }
        record << msg;
        logger->write(record.ref());
    }
}

void TVPLogMsg(TVPLogLevel logLevel, const char *msg)
{
    auto logger = plog::get();
    if (logger) {
        plog::Record record(TVPLogLevelToPlogSeverity(logLevel), "", 0, "", 0, 0);
        record << msg;
        logger->write(record.ref());
    }
}

//---------------------------------------------------------------------------
// TVPDispatchAppender
//
// plog で整形された本文 (タイムスタンプ無し、MessageOnlyFormatter) を
// UTF-8 化して LogCore の TVPLogDispatchLine に引き渡す。以降の
// コンソール/ファイル/キャッシュ/sink は LogCore が面倒を見る。
//---------------------------------------------------------------------------
class TVPDispatchAppender : public plog::IAppender
{
public:
    virtual void write(const plog::Record& record) override
    {
        plog::util::nstring str = plog::MessageOnlyFormatter::format(record);
        plog::util::MutexLock lock(m_mutex);

#ifdef _WIN32
        const std::wstring& wstr = plog::util::toWide(str);
        std::string utf8;
        TVPUtf16ToUtf8(utf8, (const tjs_char*)wstr.c_str());
#else
        std::string utf8 = str;
#endif
        // 末尾の改行は LogCore 側で処理されるので剥がしてもしなくても良い
        while (!utf8.empty() && (utf8.back() == '\n' || utf8.back() == '\r'))
            utf8.pop_back();
        TVPLogDispatchLine(TVPPlogSeverityToLogLevel(record.getSeverity()), utf8.c_str());
    }
protected:
    plog::util::Mutex m_mutex;
};

void TVPLogInit(TVPLogLevel logLevel)
{
    static TVPDispatchAppender dispatchAppender;
    plog::init(TVPLogLevelToPlogSeverity(logLevel), &dispatchAppender);
}
