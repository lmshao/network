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

#include "singleton.h"

namespace lmshao::network {
enum class EventType {
    READ = 0x01,
    WRITE = 0x02,
    ERROR = 0x04,
    CLOSE = 0x08
};

class EventHandler {
public:
    virtual ~EventHandler() = default;

    virtual void HandleRead(int fd) = 0;
    virtual void HandleWrite(int fd) = 0;
    virtual void HandleError(int fd) = 0;
    virtual void HandleClose(int fd) = 0;

    virtual int GetHandle() const = 0;
    virtual int GetEvents() const { return static_cast<int>(EventType::READ); }
};

class EventReactor : public Singleton<EventReactor> {
public:
    friend class Singleton<EventReactor>;

    EventReactor();
    ~EventReactor();

    bool RegisterHandler(std::shared_ptr<EventHandler> handler);
    bool RemoveHandler(int fd);
    bool ModifyHandler(int fd, int events);

    void SetThreadName(const std::string &name);

private:
    void Run();
    void DispatchEvent(int fd, int events);
    void WakeupEventLoop();

#ifdef _WIN32
    HANDLE iocpHandle_ = INVALID_HANDLE_VALUE;
    HANDLE shutdownEvent_ = INVALID_HANDLE_VALUE;
    std::unordered_map<SOCKET, std::shared_ptr<EventHandler>> handlers_;
#else
    int epollFd_ = -1;
    int wakeupFd_ = -1;
    std::unordered_map<int, std::shared_ptr<EventHandler>> handlers_;
#endif

    bool running_ = false;

    std::shared_mutex mutex_;
    std::mutex signalMutex_;
    std::condition_variable runningSignal_;

    std::unique_ptr<std::thread> eventThread_;
};

} // namespace lmshao::network

#endif // NETWORK_EVENT_REACTOR_H