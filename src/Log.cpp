#include "Log.h"

static Log::Level sLevel = Log::Error;

Log::Level Log::logLevel()
{
    return sLevel;
}

void Log::setLogLevel(Log::Level level)
{
    sLevel = level;
}

void Log::log(Level level, const std::string &string)
{
    if (level < sLevel)
        return;

    fwrite(string.c_str(), 1, string.size(), stdout);
    if (string.size() > 0 && string[string.size() - 1] != '\n')
        fwrite("\n", 1, 1, stdout);
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
