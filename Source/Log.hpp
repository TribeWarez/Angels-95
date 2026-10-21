#ifndef OMEGA_LOG_HPP
#define OMEGA_LOG_HPP

#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <mutex>

// Logging system for AngelTech.
// Provides macros compatible with OzWorld's oz_log.h interface.
// Also provides a log window that can be rendered with raygui.

// Windows headers define ERROR as a macro; undefine before our enum.
#ifdef ERROR
#undef ERROR
#endif

enum class LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

// Maximum log lines stored for the in-game log window
constexpr int LOG_WINDOW_MAX_LINES = 256;
constexpr int LOG_WINDOW_MAX_LINE_LENGTH = 512;

struct LogLine {
    LogLevel level;
    char text[LOG_WINDOW_MAX_LINE_LENGTH];
};

class Log {
public:
    // Push a formatted log message
    static void write(LogLevel level, const char* file, int line, const char* fmt, ...);

    // Write raw string (used by the system)
    static void raw(LogLevel level, const char* msg);

    // Get the log buffer for rendering
    static const std::vector<LogLine>& get_buffer();

    // Clear log
    static void clear();

private:
    static std::vector<LogLine> s_buffer;
    static std::mutex s_mutex;
};

// Macros matching OzWorld oz_log.h interface
#define OZ_TRACE(...)  Log::write(LogLevel::TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define OZ_DEBUG(...)  Log::write(LogLevel::DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define OZ_INFO(...)   Log::write(LogLevel::INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define OZ_WARN(...)   Log::write(LogLevel::WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define OZ_ERROR(...)  Log::write(LogLevel::ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define OZ_FATAL(...)  Log::write(LogLevel::FATAL, __FILE__, __LINE__, __VA_ARGS__)

#endif // OMEGA_LOG_HPP