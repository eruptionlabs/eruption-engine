#include "core/Logger.hpp"

#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace eruption {

LogLevel Logger::s_level = LogLevel::Warning;
bool Logger::s_initialized = false;
Logger::Sink Logger::s_sink = nullptr;
void* Logger::s_sinkUser = nullptr;

void Logger::message(LogLevel level, const char* text) {
    std::fprintf(stderr, "%s%s\033[0m\n", levelToColor(level), text);
    if (Sink sink = s_sink) sink(level, text, s_sinkUser);
}

void Logger::setSink(Sink sink, void* user) {
    s_sinkUser = user;
    s_sink = sink;
}

void Logger::init() {
    s_initialized = true;
}

void Logger::shutdown() {
    s_initialized = false;
}

void Logger::setLevel(LogLevel level) {
    s_level = level;
}

const char* Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::None:    return "NONE";
        case LogLevel::Trace:   return "TRACE";
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

const char* Logger::levelToColor(LogLevel level) {
    switch (level) {
        case LogLevel::None:    return "\033[0m";
        case LogLevel::Trace:   return "\033[90m";  // bright black
        case LogLevel::Debug:   return "\033[36m";  // cyan
        case LogLevel::Info:    return "\033[32m";  // green
        case LogLevel::Warning: return "\033[33m";  // yellow
        case LogLevel::Error:   return "\033[31m";  // red
        case LogLevel::Fatal:   return "\033[35m";  // magenta
    }
    return "\033[0m";
}

void Logger::log(LogLevel level, const char* fmt, std::va_list args) {
    if (!s_initialized || s_level == LogLevel::None || static_cast<int>(level) < static_cast<int>(s_level)) {
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm* local = std::localtime(&now);
    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", local);

    const char* color = levelToColor(level);
    const char* reset = "\033[0m";

    std::va_list sinkArgs;
    va_copy(sinkArgs, args);
    std::fprintf(stderr, "%s[%s] [%s] ", color, timeBuf, levelToString(level));
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "%s\n", reset);
    if (Sink sink = s_sink) {
        char msg[1024];
        std::vsnprintf(msg, sizeof(msg), fmt, sinkArgs);
        sink(level, msg, s_sinkUser);
    }
    va_end(sinkArgs);
}

void Logger::trace(const char* fmt, ...) { std::va_list args; va_start(args, fmt); log(LogLevel::Trace, fmt, args); va_end(args); }
void Logger::debug(const char* fmt, ...) { std::va_list args; va_start(args, fmt); log(LogLevel::Debug, fmt, args); va_end(args); }
void Logger::info(const char* fmt, ...)  { std::va_list args; va_start(args, fmt); log(LogLevel::Info, fmt, args); va_end(args); }
void Logger::warning(const char* fmt, ...) { std::va_list args; va_start(args, fmt); log(LogLevel::Warning, fmt, args); va_end(args); }
void Logger::error(const char* fmt, ...) { std::va_list args; va_start(args, fmt); log(LogLevel::Error, fmt, args); va_end(args); }
void Logger::fatal(const char* fmt, ...) { std::va_list args; va_start(args, fmt); log(LogLevel::Fatal, fmt, args); va_end(args); }

} // namespace eruption
