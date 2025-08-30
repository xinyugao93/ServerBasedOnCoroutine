#pragma once
#include <cstdint>

enum MsgType {
    MSG = 1,
    REQ = 2,
    UNKNOWN = 3
};

typedef struct MsgHead {
    uint32_t msgId;
    MsgType type;
    uint32_t dataLen;
}* PMsgHead;