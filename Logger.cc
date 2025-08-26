#include "Logger.h"
#include "spdlog/async.h"
#include <cstdarg>
#include <cstdio>
#include <sstream>

Logger::~Logger() {
    Shutdown();
}

Logger* Logger::Instance() {
    static Logger instance;
    return &instance;
}

void Logger::Shutdown() {
    spdlog::shutdown();
    _init = false;
}

void Logger::DropAll() {
    spdlog::drop_all();
    _init = false;
}

bool Logger::Init(const std::string& logName, const std::string& logType, bool async, std::string errMsg) {
    if(_init) {
        return true;
    }

    try{
        std::vector<spdlog::sink_ptr> sinkVec;
        if(logType == "console") {
            auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sinkVec.push_back(consoleSink);
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logName, FILE_SIZE, 3);
            sinkVec.push_back(fileSink);
            _logger = std::make_shared<spdlog::logger>("logger", sinkVec.begin(), sinkVec.end());
        } else if(!async) {
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logName, FILE_SIZE, 3);
            sinkVec.push_back(fileSink);
            _logger = std::make_shared<spdlog::logger>("logger", sinkVec.begin(), sinkVec.end());
        } else {
            spdlog::init_thread_pool(8192, 2);
            auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logName, FILE_SIZE, 3);
            sinkVec.push_back(fileSink);
            _logger = std::make_shared<spdlog::async_logger>("logger", sinkVec.begin(), sinkVec.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
        }
    } catch(const std::exception &e) {
        std::stringstream ss;
        ss << "init log failed, error: " << e.what();
        errMsg = ss.str();
        return false;
    }

    _logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [PID:%P] [%s:%#] %v");
    _logger->set_level(spdlog::level::info);
    _logger->flush_on(spdlog::level::debug);
    _init = true;

    return true;
}

void Logger::PrintLog(const char* file, int line, spdlog::level::level_enum level, const char* fmt, ...) {
    if(!_init) {
        printf("Logger not initialized!\n");
        return;
    }

    if(!_logger) {
        printf("Logger pointer is null!\n");
        return;
    }

    char buf[LOG_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    _logger->log(spdlog::source_loc{file, line, ""}, level, buf);
}