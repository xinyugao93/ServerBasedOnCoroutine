#include "CoroutineServer.h"
#include "Logger.h"
#include <cstdio>
#include <numeric>

spdlog::level::level_enum log_level = spdlog::level::info;

int main() {
    std::string errMsg;
    if(!Logger::Instance()->Init("server.log", "", false,  errMsg)) {
        printf("Init logger failed, %s", errMsg.c_str());
        return -1;
    }
    
    AsyncServer server;
    auto start = server.StartServer(9999);
    if(!start.get()) {
        ERROR_LOG("start server failed");
        return -1;
    }

    server.RunServer();

    return 0;
}