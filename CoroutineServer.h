#include "Logger.h"
#include "MsgType.h"
#include <coroutine>
#include <functional>
#include <exception>
#include <utility>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/select.h>

const int INVALID_SOCKET_VALUE = -1;
template <class T>
struct promise_result {
    T value_;
    std::exception_ptr eptr_;

    void return_value(T value) noexcept {
        INFO_LOG("promise non-void return value.");
        value_ = std::move(value);
    }
};

template <>
struct promise_result<void> {
    std::exception_ptr eptr_;

    void return_void() noexcept {
        INFO_LOG("promise void return void.");
    }
};

template <typename T>
class Task {
public:
    struct promise_type : promise_result<T> {
        std::coroutine_handle<> continuation_{};

        Task get_return_object() {
            INFO_LOG("Task get_return_object.");
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_never initial_suspend() {
            INFO_LOG("Task initial_suspend.");
            return {};
        }

        struct FinalAwaiter {
            bool await_ready() noexcept { 
                INFO_LOG("Task FinalAwaiter await_ready.");
                return false; 
            }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                INFO_LOG("Task FinalAwaiter await_suspend.");
                if(auto cont = h.promise().continuation_) {
                    cont.resume();
                }
            }
            void await_resume() noexcept {
                INFO_LOG("Task FinalAwaiter await_resume.");
            }
        };

        FinalAwaiter final_suspend() noexcept {
            INFO_LOG("Task final_suspend.");
            return {};
        }

        void unhandled_exception() {
            INFO_LOG("Task unhandled_exception.");
            this->eptr_ = std::current_exception();
        }
    };

    Task() noexcept : _handle{} {}

    explicit Task(std::coroutine_handle<promise_type> handle) : _handle(handle) {
        INFO_LOG("Task contruction function.");
    }

    ~Task() {
        INFO_LOG("Task deconstruction function.");
        if(_handle) {
            _handle.destroy();
            _handle = {};
        }
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    
    Task(Task&& other) noexcept : _handle(std::exchange(other._handle, {})) {
        INFO_LOG("Task move construction function.");
    }

    Task& operator=(Task&& other) noexcept {
        INFO_LOG("Task move equality construction function.");
        if(this != &other) {
            if(_handle) {
                _handle.destroy();
            }
            _handle = std::exchange(other._handle, {});
        }
        return *this;
    }

    bool await_ready() const noexcept {
        INFO_LOG("Task Awaiter await_ready.");
        return !_handle || _handle.done();
    }

    void await_suspend(std::coroutine_handle<> awaiting) noexcept {
        INFO_LOG("Task Awaiter await_suspend.");
        _handle.promise().continuation_ = awaiting;
    }

    auto await_resume() {
        INFO_LOG("Task Awaiter await_resume.");
        if(_handle.promise().eptr_) {
            auto e = _handle.promise().eptr_;
            _handle = {};
            std::rethrow_exception(e);
        }
        if constexpr (std::is_void_v<T>) {
            _handle.destroy();
            _handle = {};
            return;
        } else {
            T result = std::move(_handle.promise().value_);
            _handle.destroy();
            _handle = {};
            return result;
        }
    }

    bool Done() {
        return !_handle || _handle.done();
    }

    T get() {
        INFO_LOG("Task get value function.");
        while(!_handle.done()) {
            _handle.resume();
        }
        if(_handle.promise().eptr_) {
            std::rethrow_exception(_handle.promise().eptr_);
        }
        return _handle.promise().value_;
    }

private:
    std::coroutine_handle<promise_type> _handle;
};

struct Selector {
    std::unordered_map<int, std::vector<std::coroutine_handle<>>> mapFd2ReadHandle;
    std::unordered_map<int, std::vector<std::coroutine_handle<>>> mapFd2WriteHandle;

    void WaitRead(int fd, std::coroutine_handle<> h) {
        mapFd2ReadHandle[fd].push_back(h);
    }

    void WaitWrite(int fd, std::coroutine_handle<> h) {
        mapFd2WriteHandle[fd].push_back(h);
    }

    void CancelFd(int fd) {
        mapFd2ReadHandle.erase(fd);
        mapFd2WriteHandle.erase(fd);
    }

    void ShutDown() {
        std::vector<std::coroutine_handle<>> vecResumes;
        for(auto &pr : mapFd2ReadHandle) {
            auto vec = std::move(pr.second);
            vecResumes.insert(vecResumes.end(), vec.begin(), vec.end());
        }
        for(auto &pr : mapFd2WriteHandle) {
            auto vec = std::move(pr.second);
            vecResumes.insert(vecResumes.end(), vec.begin(), vec.end());
        }
        mapFd2ReadHandle.clear();
        mapFd2WriteHandle.clear();

        for(auto h : vecResumes) {
            if(h && !h.done()) {
                h.resume();
            }
        }
    }

    void RunOnce(int fd = -1) {
        fd_set readFd, writeFd;
        FD_ZERO(&readFd);
        FD_ZERO(&writeFd);

        int maxFd = -1;
        if(fd >= 0) {
            FD_SET(fd, &readFd);
            maxFd = std::max(maxFd, fd);
        }

        for(auto& pr : mapFd2ReadHandle) {
            FD_SET(pr.first, &readFd);
            maxFd = std::max(maxFd, pr.first);
        }
        for(auto& pr : mapFd2WriteHandle) {
            FD_SET(pr.first, &writeFd);
            maxFd = std::max(maxFd, pr.first);
        }

        if(maxFd < 0) {
            WARN_LOG("no fd needs to select!");
            return;
        }

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;
        int ret = select(maxFd + 1, &readFd, &writeFd, nullptr, &timeout);
        if(ret < 0) {
            WARN_LOG("selct return error, errno: %d, errmsg: %s", errno, strerror(errno));
            return;
        }

        std::vector<std::coroutine_handle<>> vecResumes;
        if(fd >= 0 && FD_ISSET(fd, &readFd)) {
            auto itr = mapFd2ReadHandle.find(fd);
            if(itr != mapFd2ReadHandle.end()) {
                auto vec = std::move(itr->second);
                mapFd2ReadHandle.erase(itr);
                vecResumes.insert(vecResumes.end(), vec.begin(), vec.end());
            }
        }

        for(auto itr = mapFd2ReadHandle.begin(); itr != mapFd2ReadHandle.end();) {
            if(FD_ISSET(itr->first, &readFd)) {
                auto vec = std::move(itr->second);
                itr = mapFd2ReadHandle.erase(itr);
                vecResumes.insert(vecResumes.end(), vec.begin(), vec.end());
            } else {
                ++itr;
            }
        }
        for(auto itr = mapFd2WriteHandle.begin(); itr != mapFd2WriteHandle.end();) {
            if(FD_ISSET(itr->first, &writeFd)) {
                auto vec = std::move(itr->second);
                itr = mapFd2WriteHandle.erase(itr);
                vecResumes.insert(vecResumes.end(), vec.begin(), vec.end());
            } else {
                ++itr;
            }
        }

        for(auto h : vecResumes) {
            if(h && !h.done()) {
                h.resume();
            }
        }
    }
};

struct OnReadable {
    Selector* sel;
    int fd;
    bool await_ready() const noexcept { 
        INFO_LOG("OnReadable await_ready.");
        return false; 
    }
    void await_suspend(std::coroutine_handle<> h) {
        INFO_LOG("OnReadable await_suspend.");
        sel->WaitRead(fd, h);
    }
    void await_resume() const noexcept {
        INFO_LOG("OnReadable await_resume.");
    }
};

struct OnWritable {
    Selector* sel;
    int fd;
    bool await_ready() const noexcept { 
        INFO_LOG("OnWritable await_ready.");
        return false; 
    }
    void await_suspend(std::coroutine_handle<> h) {
        INFO_LOG("OnWritable await_suspend.");
        sel->WaitWrite(fd, h);
    }
    void await_resume() const noexcept {
        INFO_LOG("OnWriteable await_resume.");
    }
};

struct RecvBuf {
    char* recvBuf{nullptr};
    PMsgHead pHead{nullptr};
    uint32_t usedBuf{0};

    RecvBuf() = default;
    explicit RecvBuf(char* buf) : recvBuf(buf) {}
};

struct ReqData {
    uint32_t reqId{0};
    int32_t  reqDataLen{0};
    MsgType  type;
};
class AsyncServer {
    static constexpr uint32_t BUFFER_SIZE = 10 << 20;
public:
    AsyncServer() = default;
    ~AsyncServer();

    Task<bool> StartServer(uint16_t port);

    void RunServer();

private:
    void StopServer();

    Task<void> AcceptLoop();

    Task<void> SessionEcho(int clientFd);

    Task<ReqData> ReadData(int clientFd, std::string& readData);

    Task<void> SendData(int clientFd, const std::string& data);

    void _MakeResponse(uint32_t msgId, MsgType type, const std::string& respMsg, std::string& response);

private:
    int listenSocket_{INVALID_SOCKET_VALUE};

    bool running_{false};

    Selector sel_;

    uint16_t port_{0};

    Task<void> acceptTask_;

    std::unordered_map<int, Task<void>> mapFd2Task_;
    
    std::unordered_map<int, RecvBuf> mapFd2RecvBuf_;
};