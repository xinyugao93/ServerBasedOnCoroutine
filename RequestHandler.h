#pragma once

#include "MsgType.h"
#include <string>
#include <atomic>

class RequestHandler {
public:
    RequestHandler() = default;

    ~RequestHandler() = default;

    void HandleRequest(MsgType type, std::string& reqMsg, std::string& respMsg);

private:
    void _HandleMsg(std::string& reqMsg, std::string& respMsg);

    void _HandleRequest(std::string& reqMsg, std::string& respMsg);

    void _MakeErrResponse(std::string& respMsg);

private:
    static std::atomic<uint32_t> _requestNum;
};