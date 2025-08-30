#include "CoroutineServer.h"
#include "RequestHandler.h"
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
    for(auto &clientSocket : mapFd2Task_) {
        close(clientSocket.first);
    }
    mapFd2Task_.clear();
    if(listenSocket_ != INVALID_SOCKET_VALUE) {
        close(listenSocket_);
        listenSocket_ = INVALID_SOCKET_VALUE;
    }
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

void AsyncServer::RunServer() {
    while(running_) {
        sel_.RunOnce(listenSocket_);
        for(auto itr = mapFd2Task_.begin(); itr != mapFd2Task_.end();) {
            if(itr->second.Done()) {
                INFO_LOG("Find task has been done, fd[%d]", itr->first);
                sel_.CancelFd(itr->first);
                close(itr->first);
                itr = mapFd2Task_.erase(itr);
            } else {
                ++itr;
            }
        }
    }
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
                mapFd2RecvBuf_.emplace(clientFd, new char[BUFFER_SIZE]);
                mapFd2Task_.emplace(clientFd, SessionEcho(clientFd));
                continue;
            }
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                INFO_LOG("Failed to accept errno: %d, errmsg: %s, ignore", errno, strerror(errno));
                break;
            }
            if(errno == EINTR || errno == ECONNABORTED) {
                INFO_LOG("Failed to accept errno: %d, errmsg: %s, restart", errno, strerror(errno));
                continue;
            }
            ERROR_LOG("Failed to accept errno: %d, errmsg: %s", errno, strerror(errno));
            break;
        }
    }

    INFO_LOG("finish accept looop coroutine");
    co_return;
}

Task<void> AsyncServer::SessionEcho(int cliendFd) {
    std::string readData;
    while(true) {
        readData.clear();
        INFO_LOG("client fd[%d] co_await ReadData", cliendFd)
        auto req = co_await ReadData(cliendFd, readData);
        INFO_LOG("co_await read data len[%lu]", req.reqDataLen);
        if(req.reqDataLen <= 0) {
            break;
        }

        RequestHandler handler;
        std::string respMsg;
        handler.HandleRequest(req.type, readData, respMsg);
        _MakeResponse(req.reqId, req.type, readData, respMsg);

        INFO_LOG("client fd[%d] co_await SendData, response len[%u]", cliendFd, (uint32_t)respMsg.length())
        co_await SendData(cliendFd, respMsg);
    }

    co_return;
}

Task<ReqData> AsyncServer::ReadData(int clientFd, std::string& readData) {
    auto itr = mapFd2RecvBuf_.find(clientFd);
    if(itr == mapFd2RecvBuf_.end()) {
        co_return ReqData{0, -1, MsgType::UNKNOWN};
    }

    int readLen = sizeof(MsgHead);
    INFO_LOG("request header len[%d]", readLen);
    while(readLen > 0) {
        int len = recv(clientFd, itr->second.recvBuf + itr->second.usedBuf, readLen, 0);
        if(len < 0) {
            if(EINTR == errno) {
                INFO_LOG("Failed to read data, errno: %d, errmsg: %s, ignore", errno, strerror(errno));
                continue;
            } else if(EAGAIN == errno || EWOULDBLOCK == errno) {
                INFO_LOG("Failed to read data, errno: %d, errmsg: %s, wait to read data", errno, strerror(errno));
                co_await OnReadable{&sel_, clientFd};
                continue;
            } 
            
            INFO_LOG("Failed to read data, errno: %d, errmsg: %s, connection closed", errno, strerror(errno));
            co_return ReqData{0, -1, MsgType::UNKNOWN};;
        } else if(0 == len) {
            INFO_LOG("Peer closed the connection");
            co_return ReqData{0, 0, MsgType::UNKNOWN};
        }

        itr->second.usedBuf += len;
        readLen             -= len;
    }
    INFO_LOG("request used buf len[%u]", itr->second.usedBuf);

    itr->second.pHead = reinterpret_cast<PMsgHead>(itr->second.recvBuf);
    readLen = itr->second.pHead->dataLen;
    INFO_LOG("request datalen[%d]", readLen);
    while(readLen > 0) {
        int len = recv(clientFd, itr->second.recvBuf + itr->second.usedBuf, readLen, 0);
        if(len < 0) {
            if(EINTR == errno) {
                INFO_LOG("Failed to read data, errno: %d, errmsg: %s, ignore", errno, strerror(errno));
                continue;
            } else if(EAGAIN == errno || EWOULDBLOCK == errno) {
                INFO_LOG("Failed to read data, errno: %d, errmsg: %s, wait to read data", errno, strerror(errno));
                co_await OnReadable{&sel_, clientFd};
                continue;
            } 
            
            INFO_LOG("Failed to read data, errno: %d, errmsg: %s, connection closed", errno, strerror(errno));
            co_return ReqData{0, -1, MsgType::UNKNOWN};;
        } else if(0 == len) {
            INFO_LOG("Peer closed the connection");
            co_return ReqData{0, 0, MsgType::UNKNOWN};;
        }

        itr->second.usedBuf += len;
        readLen             -= len;
    }

    readLen = itr->second.pHead->dataLen;
    uint32_t msgId = itr->second.pHead->msgId;
    MsgType type = itr->second.pHead->type;
    readData.resize(readLen);
    memcpy(readData.data(), itr->second.recvBuf + sizeof(MsgHead), readLen);
    itr->second.pHead = nullptr;
    itr->second.usedBuf = 0;
    co_return ReqData{msgId, readLen, type};
}

Task<void> AsyncServer::SendData(int clientFd, const std::string& data) {
    int writeLen = 0;
    int dataLen = data.length();
    while(writeLen < dataLen) {
        int len = send(clientFd, data.data() + writeLen, dataLen - writeLen, 0);
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

void AsyncServer::_MakeResponse(uint32_t msgId, MsgType type, const std::string& respMsg, std::string& response) {
    uint32_t totalLen = sizeof(MsgHead) + respMsg.length();
    response.resize(totalLen);

    auto head = (PMsgHead)response.data();
    head->msgId   = msgId;
    head->type    = type;
    head->dataLen = respMsg.length();

    memcpy(response.data() + sizeof(MsgHead), respMsg.data(), respMsg.length());
}