#ifndef LOG_H
#define LOG_H

#include <cstdarg>
#include <string>
#include <cassert>

namespace Log {
enum Level {
    Verbose,
    Debug,
    Warn,
    Error,
    Fatal,
    Silent
};

Level logLevel();
void setLogLevel(Level level);

template <size_t StaticBufSize = 4096>
inline static std::string vformat(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    char buffer[StaticBufSize];
    const size_t size = ::vsnprintf(buffer, StaticBufSize, format, args);
    assert(size >= 0);
    std::string ret;
    if (size < StaticBufSize) {
        ret.assign(buffer, size);
    } else {
        ret.resize(size);
        ::vsnprintf(&ret[0], size+1, format, copy);
    }
    va_end(copy);
    return ret;
}

void log(Level level, const std::string &string);
void log(Level level, const char *fmt, va_list args);
void verbose(const char *fmt, ...) __attribute__ ((__format__ (__printf__, 1, 2)));
void debug(const char *fmt, ...) __attribute__ ((__format__ (__printf__, 1, 2)));
void warn(const char *fmt, ...) __attribute__ ((__format__ (__printf__, 1, 2)));
void error(const char *fmt, ...) __attribute__ ((__format__ (__printf__, 1, 2)));
void fatal(const char *fmt, ...) __attribute__ ((__format__ (__printf__, 1, 2)));

#define VERBOSE(...)                            \
    if (Log::logLevel() <= Log::Verbose)        \
        Log::verbose(__VA_ARGS__)
#define DEBUG(...)                              \
    if (Log::logLevel() <= Log::Debug)          \
        Log::debug(__VA_ARGS__)
#define WARN(...)                               \
    if (Log::logLevel() <= Log::Warn)           \
        Log::warn(__VA_ARGS__)
#define ERROR(...)                              \
    if (Log::logLevel() <= Log::Error)          \
        Log::error(__VA_ARGS__)
#define FATAL(...)                              \
    if (Log::logLevel() <= Log::Fatal)          \
        Log::fatal(__VA_ARGS__)

}

#endif /* LOG_H */
