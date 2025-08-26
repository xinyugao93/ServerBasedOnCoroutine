#include "CoroutineServer.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h> 
#include <arpa/inet.h>

AsyncServer::~AsyncServer() {
    StopServer();
}

void AsyncServer::StopServer() {
    running_ = false;
    sel_.ShutDown();
    for(auto clientSocket : vecClientSockets_) {
        close(clientSocket);
    }
    vecClientSockets_.clear();
    if(listenSocket_ != INVALID_SOCKET_VALUE) {
        close(listenSocket_);
        listenSocket_ = INVALID_SOCKET_VALUE;
    }
    mapFd2Task_.clear();
}

Task<bool> AsyncServer::StartServer(uint16_t prrt) {
    listenSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if(-1 == listenSocket_) {
        ERROR_LOG("create socket failed, errno: %d, error: %s", errno, strerror(errno));
        co_return false;
    }

    int32_t flags = fcntl(listenSocket_, F_GETFL, 0);
    if(-1 == flags) {
        ERROR_LOG("fcntl get flags failed, errno: %d, error: %s", errno, strerror(errno));
        co_return false;
    }

    flags |= O_NONBLOCK;
    if(-1 == fcntl(listenSocket_, F_SETFL, flags)) {
        ERROR_LOG("fcntl set nonblock failed, errno: %d, error: %s", errno, strerror(errno));
        co_return false;
    }

    int32_t opt = 1;
    if(-1 == setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        ERROR_LOG("setsockopt SO_REUSEADDR failed, errno: %d, error: %s", errno, strerror(errno));
        co_return false;
    }
    
    if(-1 == setsockopt(listenSocket_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
        ERROR_LOG("setsockopt TCP_NODELAY failed, errno: %d, error: %s", errno, strerror(errno));
        co_return false;
    }

    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(prrt);

    if(-1 == bind(listenSocket_, (struct sockaddr *)&serverAddress, sizeof(serverAddress))) {
        ERROR_LOG("bind failed, errno: %d, error: %s", errno, strerror(errno));
        co_return false;
    }

    if(-1 == listen(listenSocket_, 10)) {   
        ERROR_LOG("listen failed, errno: %d, error: %s", errno, strerror(errno));
        co_return false;
    }

    running_ = true;
    acceptTask_ = std::move(AcceptLoop());
    INFO_LOG("Server started on port %d", prrt);
    co_return true;
}

Task<void> AsyncServer::AcceptLoop() {
    INFO_LOG("start accept loop coroutine");
    while(running_) {
        co_await OnReadable{&this->sel_, listenSocket_};
        if(!running_) {
            INFO_LOG("finish accept looop coroutine");
            co_return;
        }
        while(true) {
            struct sockaddr_in clientAddress{};
            socklen_t cliAddrLen = sizeof(clientAddress);
            int clientFd = accept4(listenSocket_, (sockaddr*)&clientAddress, &cliAddrLen, SOCK_NONBLOCK);
            if(clientFd >= 0) {
                std::string clientIp(INET_ADDRSTRLEN, '0');
                inet_ntop(AF_INET, &clientAddress.sin_addr, clientIp.data(), clientIp.length());
                INFO_LOG("accept client ip[%s:%hu] connect.", clientIp.c_str(), ntohs(clientAddress.sin_port));
                mapFd2Task_.emplace(clientFd, SessionEcho(clientFd));
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                INFO_LOG("Failed to accept errno: %d, errmsg: %s, ignore", errno, strerror(errno));
                break;
            }
            if(errno == EINTR || errno == ECONNABORTED) {
                INFO_LOG("Failed to accept errno: %d, errmsg: %s, restart", errno, strerror(errno))
                continue;
            }
            ERROR_LOG("Failed to accept errno: %d, errmsg: %s", errno, strerror(errno));
            break;
        }
    }

    INFO_LOG("finish accept looop coroutine");
    co_return;
}

void AsyncServer::RunServer() {
    while(running_) {
        sel_.RunOnce(listenSocket_);
    }
}

Task<void> AsyncServer::SessionEcho(int cliendFd) {
    std::string readData;
    while(true) {
        readData.clear();
        ssize_t n = co_await ReadData(cliendFd, readData);
        if(n <= 0) {
            break;
        }
        co_await SendData(cliendFd, readData);
    }

    close(cliendFd);
    sel_.CancelFd(cliendFd);
    co_return;
}

Task<ssize_t> AsyncServer::ReadData(int clientFd, std::string& readData) {
    while(true) {
        readData.resize(4096);
        ssize_t readLen = recv(clientFd, readData.data(), readData.length(), 0);
        if(readLen > 0) {
            INFO_LOG("Succeed to read data len[%d]", readLen);
            co_return readLen;
        } else if(0 == readLen) {
            INFO_LOG("Peer close the connection");
            co_return 0;
        }

        if(errno == EINTR) {
            INFO_LOG("Failed to read data errno: %d, errmsg: %s, ignore", errno, strerror(errno));
            continue;
        } else if(errno == EAGAIN || errno == EWOULDBLOCK) {
            co_await OnReadable{&sel_, clientFd};
            continue;
        }
        break;
    }
    co_return -1;
}

Task<void> AsyncServer::SendData(int clientFd, const std::string& data) {
    int writeLen = 0;
    int dataLen = data.length();
    while(writeLen < dataLen) {
        int len = send(clientFd, data.data(), dataLen - writeLen, 0);
        if(len > 0) {
            INFO_LOG("Succeed to write data len[%d]", len);
            writeLen += len;
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            INFO_LOG("Failed to write data errno: %d, errmsg: %s, ignore", errno, strerror(errno));
            co_await OnWritable{&sel_, clientFd};
            continue;
        }
        break;
    }
    co_return;
}