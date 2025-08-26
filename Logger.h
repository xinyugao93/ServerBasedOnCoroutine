#pragma once

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include <string>

extern spdlog::level::level_enum log_level;

class Logger {
    static constexpr uint32_t FILE_SIZE = 100 << 20;
    static constexpr uint32_t LOG_LEN   = 1024;
public:
    ~Logger();

    void Shutdown();

    void DropAll();

    static Logger* Instance();

    bool Init(const std::string& logName, const std::string& logType, bool async, std::string errMsg);

    void PrintLog(const char* file, int line, spdlog::level::level_enum level, const char* fmt, ...);

private:
    Logger() = default;

    Logger(const Logger&) = delete;

    Logger(Logger&&) = delete;

    Logger& operator=(const Logger&) = delete;

    Logger& operator=(Logger&&) = delete;

private:    
    std::shared_ptr<spdlog::logger> _logger{nullptr};

    bool _init{false};
};

#define DEBUG_LOG(fmt, ...)                                                     \
    if(log_level >= spdlog::level::debug) {                                     \
        Logger::Instance()->PrintLog(__FILE__, __LINE__, spdlog::level::debug, fmt, ##__VA_ARGS__); \
    }                                                                           

#define INFO_LOG(fmt, ...)                                                      \
    if(log_level >= spdlog::level::info) {                                      \
        Logger::Instance()->PrintLog(__FILE__, __LINE__, spdlog::level::info, fmt, ##__VA_ARGS__);  \
    }                                                                           

#define WARN_LOG(fmt, ...)                                                      \
    if(log_level >= spdlog::level::warn) {                                      \
        Logger::Instance()->PrintLog(__FILE__, __LINE__, spdlog::level::warn, fmt, ##__VA_ARGS__);  \
    }                                                                           

#define ERROR_LOG(fmt, ...)                                                     \
    if(log_level >= spdlog::level::err) {                                       \
        Logger::Instance()->PrintLog(__FILE__, __LINE__, spdlog::level::err, fmt, ##__VA_ARGS__);   \
    }                                                                                 
