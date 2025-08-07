/**
 * @file event_reactor.h
 * @brief Event Reactor Header for Network I/O
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETWORK_EVENT_REACTOR_H
#define NETWORK_EVENT_REACTOR_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
// Windows IOCP specific structures - simplified for event-driven mode
#else
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#include <condition_variable>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "common.h"
#include "singleton.h"

namespace lmshao::network {
enum class EventType {
    READ = 0x01,
    WRITE = 0x02,
    ERROR = 0x04,
    CLOSE = 0x08
};

#ifdef _WIN32
struct IocpEventInfo {
    SOCKET socket;
    EventType eventType;

    IocpEventInfo(SOCKET s, EventType et) : socket(s), eventType(et) {}

    ULONG_PTR Encode() const { return static_cast<ULONG_PTR>(socket) | (static_cast<ULONG_PTR>(eventType) << 56); }

    static IocpEventInfo Decode(ULONG_PTR completionKey)
    {
        SOCKET s = static_cast<SOCKET>(completionKey & 0xFFFFFFFFFFFFFFFFULL);
        EventType et = static_cast<EventType>((completionKey >> 56) & 0xFF);
        return IocpEventInfo(s, et);
    }
};
#endif

class EventHandler {
public:
    virtual ~EventHandler() = default;

    virtual void HandleRead(socket_t fd) = 0;
    virtual void HandleWrite(socket_t fd) = 0;
    virtual void HandleError(socket_t fd) = 0;
    virtual void HandleClose(socket_t fd) = 0;

    virtual socket_t GetHandle() const = 0;
    virtual int GetEvents() const { return static_cast<int>(EventType::READ); }
};

class EventReactor : public Singleton<EventReactor> {
public:
    friend class Singleton<EventReactor>;

    EventReactor();
    ~EventReactor();

    bool RegisterHandler(std::shared_ptr<EventHandler> handler);
    bool RemoveHandler(socket_t fd);
    bool ModifyHandler(socket_t fd, int events);

    void SetThreadName(const std::string &name);

private:
    void Run();
    void DispatchEvent(socket_t fd, int events);
    void WakeupEventLoop();

#ifdef _WIN32
    HANDLE iocpHandle_ = INVALID_HANDLE_VALUE;
    HANDLE shutdownEvent_ = INVALID_HANDLE_VALUE;
#else
    int epollFd_ = -1;
    int wakeupFd_ = -1;
#endif

    std::unordered_map<socket_t, std::shared_ptr<EventHandler>> handlers_;
    bool running_ = false;

    std::shared_mutex mutex_;
    std::mutex signalMutex_;
    std::condition_variable runningSignal_;

    std::unique_ptr<std::thread> eventThread_;
};

} // namespace lmshao::network

#endif // NETWORK_EVENT_REACTOR_H