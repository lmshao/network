//
// Copyright © 2024-2025 SHAO Liming <lmshao@163.com>. All rights reserved.
//

#ifndef NETWORK_EVENT_REACTOR_H
#define NETWORK_EVENT_REACTOR_H

#include <condition_variable>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "singleton.h"

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

private:
    int epollFd_ = -1;
    bool running_ = false;

    std::shared_mutex mutex_;
    std::mutex signalMutex_;
    std::condition_variable runningSignal_;

    std::unordered_map<int, std::shared_ptr<EventHandler>> handlers_;

    std::unique_ptr<std::thread> epollThread_;
};

#endif // NETWORK_EVENT_REACTOR_H