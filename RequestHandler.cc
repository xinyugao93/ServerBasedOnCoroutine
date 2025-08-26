#include "RequestHandler.h"
#include "Logger.h"
#include <sstream>
#include <cstring>

std::atomic<uint32_t> RequestHandler::_requestNum{0};

void RequestHandler::HandleRequest(MsgType type, std::string& reqMsg, std::string& respMsg) {
    ++_requestNum;
    switch (type) {
        case MsgType::MSG:
            _HandleMsg(reqMsg, respMsg);
            break;
        case MsgType::REQ:
            _HandleRequest(reqMsg, respMsg);
            break;
        default:
            _MakeErrResponse(respMsg);
            break;
    }
}

void RequestHandler::_HandleMsg(std::string& reqMsg, std::string& respMsg) {
    std::stringstream ss;
    ss << "receive msg, msg: " << reqMsg << ", and its confirm response";
    auto response = ss.str();

    uint32_t respLen = sizeof(bool) + response.length();
    respMsg.resize(respLen);
    *(bool *)respMsg.data() = true;
    memcpy(respMsg.data() + sizeof(bool), response.data(), response.length());
    INFO_LOG("%s\n", response.c_str());
}

void RequestHandler::_HandleRequest(std::string& reqMsg, std::string& respMsg) {
    std::stringstream ss;
    ss << "receive request, reqMsg: " << reqMsg << ", the request num is: " << _requestNum.load();
    auto response = ss.str();

    uint32_t respLen = sizeof(bool) + response.length();
    respMsg.resize(respLen);
    *(bool *)respMsg.data() = true;
    memcpy(respMsg.data() + sizeof(bool), response.data(), response.length());
    INFO_LOG("%s\n", response.c_str());
}


void RequestHandler::_MakeErrResponse(std::string& respMsg) {
    std::stringstream ss;
    ss << "receive unknown request, failed handle request";
    auto response = ss.str();

    uint32_t respLen = sizeof(bool) + response.length();
    respMsg.resize(respLen);
    *(bool *)respMsg.data() = false;
    memcpy(respMsg.data() + sizeof(bool), response.data(), response.length());
    INFO_LOG("%s\n", response.c_str());
}
