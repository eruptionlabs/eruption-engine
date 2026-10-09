#pragma once

#include <string>
#include <cstdio>
#include <cstdarg>

namespace eruption {

enum class LogLevel {
    Trace = 0,   // Per-frame details (Highest volume)
    Debug = 1,   // General debug info
    Info = 2,    // Normal info
    Warning = 3, // Warnings
    Error = 4,   // Errors
    Fatal = 5,   // Fatal errors
    None = 6     // Disabled
};

class Logger {
public:
    static void init();
    static void shutdown();

    static void setLevel(LogLevel level);
    static LogLevel getLevel() { return s_level; }

    static void trace(const char* fmt, ...);
    static void debug(const char* fmt, ...);
    static void info(const char* fmt, ...);
    static void warning(const char* fmt, ...);
    static void error(const char* fmt, ...);
    static void fatal(const char* fmt, ...);

    // Destino extra para cada mensagem ja' formatada (o console do editor).
    // Chamado na thread que gerou o log; o destino cuida da propria trava.
    using Sink = void (*)(LogLevel level, const char* message, void* user);
    // Mensagem pronta que ignora o filtro de nível (saída dos scripts do jogo).
    static void message(LogLevel level, const char* text);
    static void setSink(Sink sink, void* user);

private:
    static void log(LogLevel level, const char* fmt, std::va_list args);
    static const char* levelToString(LogLevel level);
    static const char* levelToColor(LogLevel level);

    static LogLevel s_level;
    static bool s_initialized;
    static Sink s_sink;
    static void* s_sinkUser;
};

} // namespace eruption

// Minimum log level for compilation. 0 = Trace (All), 1 = Debug, 2 = Info ...
#ifndef ERUPTION_LOG_MIN_LEVEL
#define ERUPTION_LOG_MIN_LEVEL 4 // ERROR level by default (extremely quiet)
#endif

#if ERUPTION_LOG_MIN_LEVEL <= 0
#define ERUPTION_LOG_TRACE(...)   ::eruption::Logger::trace(__VA_ARGS__)
#else
#define ERUPTION_LOG_TRACE(...)
#endif

#if ERUPTION_LOG_MIN_LEVEL <= 1
#define ERUPTION_LOG_DEBUG(...)   ::eruption::Logger::debug(__VA_ARGS__)
#else
#define ERUPTION_LOG_DEBUG(...)
#endif

#if ERUPTION_LOG_MIN_LEVEL <= 2
#define ERUPTION_LOG_INFO(...)    ::eruption::Logger::info(__VA_ARGS__)
#else
#define ERUPTION_LOG_INFO(...)
#endif

#define ERUPTION_LOG_WARN(...)    ::eruption::Logger::warning(__VA_ARGS__)
#define ERUPTION_LOG_ERROR(...)   ::eruption::Logger::error(__VA_ARGS__)
#define ERUPTION_LOG_FATAL(...)   ::eruption::Logger::fatal(__VA_ARGS__)
