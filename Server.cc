#include "Server.h"
#include "Logger.h"
#include "RequestHandler.h"
#include <cerrno>
#include <cstdio>
#include <netinet/tcp.h> 
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cstdlib>

Server::~Server() {
    if(-1 != _serverFd) {
        close(_serverFd);
    }

    if(-1 != _clientFd) {
        close(_clientFd);
    }

    if(-1 != _epollFd) {
        close(_epollFd);
    }

    if(_buffer) {
        delete[] _buffer;
        _buffer = nullptr;
    }
}

Server* Server::GetInstance() {
    static Server instance;
    return &instance;
}

bool Server::Start() {
    _serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if(-1 == _serverFd) {
        ERROR_LOG("create socket failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    int32_t flags = fcntl(_serverFd, F_GETFL, 0);
    if(-1 == flags) {
        ERROR_LOG("fcntl get flags failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    flags |= O_NONBLOCK;
    if(-1 == fcntl(_serverFd, F_SETFL, flags)) {
        ERROR_LOG("fcntl set nonblock failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    int32_t opt = 1;
    if(-1 == setsockopt(_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        ERROR_LOG("setsockopt SO_REUSEADDR failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }
    
    if(-1 == setsockopt(_serverFd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt))) {
        ERROR_LOG("setsockopt TCP_NODELAY failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(PORT);

    if(-1 == bind(_serverFd, (struct sockaddr *)&serverAddress, sizeof(serverAddress))) {
        ERROR_LOG("bind failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    if(-1 == listen(_serverFd, 10)) {   
        ERROR_LOG("listen failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    _epollFd = epoll_create1(0);
    if(-1 == _epollFd) {
        ERROR_LOG("epoll_create1 failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    // 添加服务器套接字到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = _serverFd;
    if(-1 == epoll_ctl(_epollFd, EPOLL_CTL_ADD, _serverFd, &ev)) {
        ERROR_LOG("epoll_ctl add serverfd failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    INFO_LOG("Server started on port %d\n", PORT);
    return true;
}

void Server::Run() {
    while(true) {
        int32_t status = 0;
        auto pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if(pid > 0) {
            if(WIFEXITED(status)) {
                INFO_LOG("child process %d exited with status %d\n", pid, WEXITSTATUS(status));
            } else if(WIFSIGNALED(status)) {
                INFO_LOG("child process %d terminated by signal %d\n", pid, WTERMSIG(status));
            } else if(WIFSTOPPED(status)) {
                INFO_LOG("child process %d stopped by signal %d\n", pid, WSTOPSIG(status));
            } else if(WIFCONTINUED(status)) {
                INFO_LOG("child process %d continued\n", pid);
            }
        }
        
        struct epoll_event events[MAX_EVENTS];
        int32_t timeout = 5000;
        int32_t nfds = epoll_wait(_epollFd, events, MAX_EVENTS, timeout);
        if(-1 == nfds) {
            if(errno == EINTR) {
                continue;
            } else {
                ERROR_LOG("epoll_wait failed, errno: %d, error: %s\n", errno, strerror(errno));
                return;
            }
        } else if(0 == nfds) {\
            DEBUG_LOG("epoll wait timeout, ignore\n");
            continue;
        }

        INFO_LOG("epoll_wait nfds: %d\n", nfds);
        for(int32_t i = 0; i < nfds; ++i) {
            if(events[i].data.fd == _serverFd) {
                // parent process
                struct sockaddr_in clientAddress;
                socklen_t clientAddressLen = sizeof(clientAddress);
                _clientFd = accept(_serverFd, (struct sockaddr *)&clientAddress, &clientAddressLen);
                if(-1 == _clientFd) {
                    if(errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    } else {
                        ERROR_LOG("accept failed, errno: %d, error: %s\n", errno, strerror(errno));
                        break;
                    }
                }

                // fork child process
                pid_t childPid = fork();
                if(childPid < 0) {
                    ERROR_LOG("fork failed, errno: %d, error: %s\n", errno, strerror(errno));
                    close(_clientFd);
                    continue;
                } else if(childPid == 0) {
                    // child process
                    if(!_InitChildProcess()) {
                        close(_clientFd);
                        _clientFd = -1;
                        close(_epollFd);
                        _epollFd = -1;
                        return;
                    }
                    _HandleRequest();
                    close(_clientFd);
                    _clientFd = -1;
                    close(_epollFd);
                    _epollFd = -1;
                    INFO_LOG("child process exited\n");
                    return; 
                } else {
                    // parent process
                    char clientIp[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &clientAddress.sin_addr, clientIp, sizeof(clientIp));
                    INFO_LOG("new client connected: %s:%d, child process %d created, clientfd: %d\n", 
                            clientIp, ntohs(clientAddress.sin_port), childPid, _clientFd);
                    
                    close(_clientFd);
                    _clientFd = -1;
                }
            }
        }
    }
}

bool Server::_InitChildProcess() {
    // 关闭不需要的文件描述符
    close(_epollFd);
    _epollFd = -1;

    close(_serverFd);
    _serverFd = -1;

    Logger::Instance()->Shutdown();

    std::string errMsg;
    if(!Logger::Instance()->Init("server_worker.log", "async", true, errMsg)) {
        return false;
    }
    
    // 创建子进程自己的epoll实例
    _epollFd = epoll_create1(0);
    if(-1 == _epollFd) {
        ERROR_LOG("child process epoll_create1 failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    // 设置客户端套接字为非阻塞
    int32_t flags = fcntl(_clientFd, F_GETFL, 0);
    if(-1 == flags) {
        ERROR_LOG("fcntl get flags failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    flags |= O_NONBLOCK;
    if(-1 == fcntl(_clientFd, F_SETFL, flags)) {
        ERROR_LOG("fcntl set nonblock failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    // 将客户端套接字添加到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.fd = _clientFd;
    if(-1 == epoll_ctl(_epollFd, EPOLL_CTL_ADD, _clientFd, &ev)) {
        ERROR_LOG("child process epoll_ctl add clientfd failed, errno: %d, error: %s\n", errno, strerror(errno));
        return false;
    }

    _buffer = new char[BUFFER_SIZE];
    if(nullptr == _buffer) {
        ERROR_LOG("new buffer failed\n");
        return false;
    }

    return true;
}

void Server::_HandleRequest() {
    while(true) {
        struct epoll_event events[MAX_EVENTS];
        int32_t nfds = epoll_wait(_epollFd, events, MAX_EVENTS, -1);
        if(-1 == nfds) {
            if(errno == EINTR) {
                continue;
            } else {
                DEBUG_LOG("epoll_wait failed, errno: %d, error: %s\n", errno, strerror(errno));
                return;
            }
        }

        for(int i = 0; i < nfds; ++i) {
            if(events[i].data.fd == _clientFd && (events[i].events & EPOLLIN)) {
                uint32_t readLen = _pHead != nullptr ? _pHead->dataLen - _usedBuf + sizeof(MsgHead) : sizeof(MsgHead) - _usedBuf;
                while(readLen > 0) {
                    auto len = read(_clientFd, _buffer + _usedBuf, readLen);
                    if(len < 0) {
                        if(EINTR == errno) {
                            DEBUG_LOG("read failed caused by interrupt errno: %d, error: %s, read again\n", errno, strerror(errno));
                            continue;
                        } else if(EAGAIN == errno || EWOULDBLOCK == errno) {
                            DEBUG_LOG("read failed no data read, errno: %d, errno: %s, wait again\n", errno, strerror(errno));
                            break;
                        } else {
                            ERROR_LOG("read failed errno: %d, error: %s\n", errno, strerror(errno));
                            break;
                        }
                    } else if(0 == len) {
                        INFO_LOG("peer closed the connection\n");
                        return;
                    }

                    _usedBuf += len;
                    readLen  -= len;
                    if(_usedBuf < sizeof(MsgHead)) {
                        continue;
                    } else if(sizeof(MsgHead) == _usedBuf) {
                        _pHead = reinterpret_cast<PMsgHead>(_buffer);
                        if(_pHead->dataLen + sizeof(MsgHead) > BUFFER_SIZE) {
                            ERROR_LOG("request data len: %u larger than max buffer size: %u\n", 
                                    _pHead->dataLen + (uint32_t)sizeof(MsgHead), BUFFER_SIZE);
                            return;
                        }
                    } else if(_usedBuf - sizeof(MsgHead) < _pHead->dataLen) {
                        continue;
                    } else {
                        INFO_LOG("receive all request data, dataLen: %u, recevie len: %u\n", 
                                _pHead->dataLen, _usedBuf - (uint32_t)sizeof(MsgHead));

                        RequestHandler handler;
                        std::string reqMsg(_pHead->dataLen, '\0');
                        memcpy(reqMsg.data(), _buffer + sizeof(MsgHead), _pHead->dataLen);
                        std::string respMsg;
                        handler.HandleRequest(_pHead->type, reqMsg, respMsg);

                        std::string response;
                        _MakeResponse(_pHead->msgId, _pHead->type, respMsg, response);
                        _SendResponse(response);
                        
                        _usedBuf = 0;
                        _pHead = nullptr;
                    } 
                }
            }
        }
    }
}

void Server::_MakeResponse(uint32_t msgId, MsgType type, const std::string& respMsg, std::string& response) {
    uint32_t totalLen = sizeof(MsgHead) + respMsg.length();
    response.resize(totalLen);

    auto head = (PMsgHead)response.data();
    head->msgId   = msgId;
    head->type    = type;
    head->dataLen = respMsg.length();

    memcpy(response.data() + sizeof(MsgHead), respMsg.data(), respMsg.length());
}

void Server::_SendResponse(const std::string& response) {
    uint32_t totalLen = response.length();
    uint32_t writeLen = 0;

    struct epoll_event events[10];
    while(writeLen < totalLen) {
        auto nfds = epoll_wait(_epollFd, events, 10, 1000);
        if(nfds < 0) {
            if(EINTR == errno) {
                continue;
            }
            ERROR_LOG("epoll wait failed, errno: %d, error: %s\n", errno, strerror(errno));
            return;
        } else if(0 == nfds) {
            continue;
        }

        for(auto i = 0; i < nfds; ++i) {
            if(events[i].data.fd == _clientFd && (events[i].events & EPOLLOUT)) {
                while(true) {
                    auto len = write(_clientFd, response.data() + writeLen, totalLen - writeLen);
                    if(len < 0) {
                        if(EINTR == errno) {
                            continue;
                        } else if(EAGAIN == errno || EWOULDBLOCK == errno) {
                            break;
                        }

                        ERROR_LOG("write socket failed, errno: %d, error: %s\n", errno, strerror(errno));
                        return;
                    }

                    INFO_LOG("write data len: %u, data len: %u, total len: %u\n", (uint32_t)len, (uint32_t)(totalLen - sizeof(MsgHead)), totalLen);
                    writeLen += len;
                    break;
                }
            }
        }
    }
}