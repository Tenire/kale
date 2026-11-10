#ifndef KALE_CORE_LOGGER_H
#define KALE_CORE_LOGGER_H

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace kale {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5
};

constexpr std::string_view level_to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off:   return "OFF";
    }
    return "UNKNOWN";
}

struct LogContext {
    std::string_view component;
    std::string_view method;
    std::string_view path;
    int status_code{0};
};

class Logger {
public:
    using Sink = std::function<void(LogLevel level, const LogContext &ctx, std::string_view message)>;

    Logger() noexcept : min_level_(LogLevel::Warn), sink_(nullptr) {}
    explicit Logger(LogLevel min_level) noexcept : min_level_(min_level), sink_(nullptr) {}
    Logger(LogLevel min_level, Sink sink) noexcept : min_level_(min_level), sink_(std::move(sink)) {}

    static Logger off() noexcept { return Logger(LogLevel::Off); }

    void set_level(LogLevel level) noexcept { min_level_ = level; }
    LogLevel level() const noexcept { return min_level_; }
    bool enabled(LogLevel level) const noexcept { return min_level_ != LogLevel::Off && level != LogLevel::Off && level >= min_level_; }

    void set_sink(Sink sink) noexcept { sink_ = std::move(sink); }
    const Sink &sink() const noexcept { return sink_; }

    void log(LogLevel level, const LogContext &ctx, std::string_view message) const noexcept;
    void log(LogLevel level, std::string_view component, std::string_view message) const noexcept {
        log(level, LogContext{component, {}, {}, 0}, message);
    }

private:
    LogLevel min_level_{LogLevel::Warn};
    Sink sink_{nullptr};
};

} // namespace kale

#endif // KALE_CORE_LOGGER_H
