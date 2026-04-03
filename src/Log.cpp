#include "Log.h"
#include <spdlog/spdlog.h>

static Log::Level sLevel = Log::Error;

Log::Level Log::logLevel()
{
    return sLevel;
}

void Log::setLogLevel(Log::Level level)
{
    sLevel = level;

    // Keep spdlog's level in sync so spdlog::debug() etc. are gated the same way.
    switch (level) {
    case Verbose: spdlog::set_level(spdlog::level::trace);    break;
    case Debug:   spdlog::set_level(spdlog::level::debug);    break;
    case Warn:    spdlog::set_level(spdlog::level::warn);     break;
    case Error:   spdlog::set_level(spdlog::level::err);      break;
    case Fatal:   spdlog::set_level(spdlog::level::critical); break;
    default:      spdlog::set_level(spdlog::level::info);     break;
    }
}

void Log::log(Level level, const std::string &string)
{
    if (level < sLevel)
        return;

    switch (level) {
    case Verbose: spdlog::trace("{}", string); break;
    case Debug:   spdlog::debug("{}", string); break;
    case Warn:    spdlog::warn("{}", string); break;
    case Error:   spdlog::error("{}", string); break;
    case Fatal:   spdlog::critical("{}", string); break;
    default:      spdlog::info("{}", string); break;
    }
}

void Log::log(Level level, const char *fmt, va_list args)
{
    log(level, Log::vformat(fmt, args));
}

void Log::verbose(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Verbose, fmt, args);
    va_end(args);
}

void Log::debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Debug, fmt, args);
    va_end(args);
}

void Log::warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Warn, fmt, args);
    va_end(args);
}

void Log::error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Error, fmt, args);
    va_end(args);
}

void Log::fatal(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log(Fatal, fmt, args);
    va_end(args);
}
