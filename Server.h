#pragma once

#include "MsgType.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>

class Server {
    static constexpr int32_t PORT = 9999;
    static constexpr uint32_t MAX_EVENTS = 64;
    static constexpr uint32_t BUFFER_SIZE = 10 << 20;
public:
    ~Server();
    Server(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(const Server&) = delete;
    Server& operator=(Server&&) = delete;

    static Server* GetInstance(); 

    bool Start();
    void Run();

private:
    Server() = default;

    void _HandleRequest();

    bool _InitChildProcess();

    void _MakeResponse(uint32_t msgId, MsgType type, const std::string& respMsg, std::string& response);

    void _SendResponse(const std::string& response);

private:
    int32_t _serverFd{-1};
    
    int32_t _clientFd{-1};

    int32_t _epollFd{-1};

    char* _buffer;

    uint32_t _usedBuf{0};

    PMsgHead _pHead{nullptr};
};