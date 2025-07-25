//
// Copyright © 2024-2025 SHAO Liming <lmshao@163.com>. All rights reserved.
//

#include "event_reactor.h"

#include <errno.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cstring>

#include "log.h"

constexpr int EPOLL_WAIT_EVENT_NUMS_MAX = 1024;
EventReactor::EventReactor()
{
    epollThread_ = std::make_unique<std::thread>([&]() { this->Run(); });
    SetThreadName("EventReactor");
    std::unique_lock<std::mutex> taskLock(signalMutex_);
    runningSignal_.wait_for(taskLock, std::chrono::milliseconds(5), [this] { return this->running_ == true; });
}

EventReactor::~EventReactor()
{
    NETWORK_LOGD("enter");
    running_ = false;
    if (epollThread_ && epollThread_->joinable()) {
        epollThread_->join();
    }

    if (epollFd_ != -1) {
        close(epollFd_);
        epollFd_ = -1;
    }
}

void EventReactor::Run()
{
    NETWORK_LOGD("enter");
    epollFd_ = epoll_create1(0);
    if (epollFd_ == -1) {
        perror("epoll_create");
        NETWORK_LOGE("epoll_create %s", strerror(errno));
        return;
    }

    int nfds = 0;
    running_ = true;
    runningSignal_.notify_all();
    struct epoll_event readyEvents[EPOLL_WAIT_EVENT_NUMS_MAX] = {};

    while (running_) {
        nfds = epoll_wait(epollFd_, readyEvents, EPOLL_WAIT_EVENT_NUMS_MAX, 100);
        if (nfds == -1) {
            if (errno == EINTR) {
                NETWORK_LOGD("ignore signal EINTR");
                continue;
            }

            NETWORK_LOGE("epoll_wait error: %s(%d)", strerror(errno), errno);
            return;
        } else if (nfds == 0) {
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            int readyFd = readyEvents[i].data.fd;
            int events = readyEvents[i].events;

            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto handlerIt = handlers_.find(readyFd);
            if (handlerIt != handlers_.end()) {
                auto handler = handlerIt->second;
                lock.unlock();
                DispatchEvent(readyFd, events);
                continue;
            }
        }
    }
}

void EventReactor::SetThreadName(const std::string &name)
{
    if (epollThread_ && !name.empty()) {
        std::string threadName = name;
        if (threadName.size() > 15) {
            threadName = threadName.substr(0, 15);
        }
        pthread_setname_np(epollThread_->native_handle(), threadName.c_str());
    }
}

bool EventReactor::RegisterHandler(std::shared_ptr<EventHandler> handler)
{
    if (!handler) {
        NETWORK_LOGE("Handler is nullptr");
        return false;
    }

    int fd = handler->GetHandle();
    int events = handler->GetEvents();
    NETWORK_LOGD("[%p] Register handler for fd:%d, events:0x%x", this, fd, events);

    if (!running_) {
        NETWORK_LOGE("Reactor has exited");
        return false;
    }

    uint32_t epollEvents = 0;
    if (events & static_cast<int>(EventType::READ)) {
        epollEvents |= EPOLLIN;
    }
    if (events & static_cast<int>(EventType::WRITE)) {
        epollEvents |= EPOLLOUT;
    }
    if (events & static_cast<int>(EventType::ERROR)) {
        epollEvents |= EPOLLERR;
    }
    if (events & static_cast<int>(EventType::CLOSE)) {
        epollEvents |= EPOLLHUP | EPOLLRDHUP;
    }

    epollEvents |= EPOLLET;

    struct epoll_event ev;
    ev.events = epollEvents;
    ev.data.fd = fd;

    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
        NETWORK_LOGE("epoll_ctl ADD error for fd %d: %s", fd, strerror(errno));
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    handlers_.emplace(fd, handler);

    NETWORK_LOGD("Handler registered successfully for fd:%d", fd);
    return true;
}

bool EventReactor::RemoveHandler(int fd)
{
    NETWORK_LOGD("Remove handler for fd(%d)", fd);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) {
        NETWORK_LOGW("Handler not found for fd:%d", fd);
        return false;
    }
    handlers_.erase(it);
    lock.unlock();

    if (!running_) {
        NETWORK_LOGE("Reactor has exited");
        return false;
    }

    struct epoll_event ev;
    if (epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, &ev) == -1) {
        NETWORK_LOGE("epoll_ctl DEL error for fd %d: %s", fd, strerror(errno));
        return false;
    }

    NETWORK_LOGD("Handler removed successfully for fd:%d", fd);
    return true;
}

bool EventReactor::ModifyHandler(int fd, int events)
{
    NETWORK_LOGD("Modify handler for fd(%d), events:0x%x", fd, events);

    if (!running_) {
        NETWORK_LOGE("Reactor has exited");
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) {
        NETWORK_LOGW("Handler not found for fd:%d during modify", fd);
        return false;
    }

    uint32_t epollEvents = 0;
    if (events & static_cast<int>(EventType::READ)) {
        epollEvents |= EPOLLIN;
    }
    if (events & static_cast<int>(EventType::WRITE)) {
        epollEvents |= EPOLLOUT;
    }
    if (events & static_cast<int>(EventType::ERROR)) {
        epollEvents |= EPOLLERR;
    }
    if (events & static_cast<int>(EventType::CLOSE)) {
        epollEvents |= EPOLLHUP | EPOLLRDHUP;
    }

    epollEvents |= EPOLLET;

    struct epoll_event ev;
    ev.events = epollEvents;
    ev.data.fd = fd;

    if (epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
        NETWORK_LOGE("epoll_ctl MOD error for fd %d: %s", fd, strerror(errno));
        return false;
    }

    NETWORK_LOGD("Handler modified successfully for fd:%d", fd);
    return true;
}

void EventReactor::DispatchEvent(int fd, int events)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) {
        NETWORK_LOGW("Handler not found for fd:%d", fd);
        return;
    }

    auto handler = it->second;
    lock.unlock();

    try {
        if (events & EPOLLIN) {
            handler->HandleRead(fd);
        }

        if (events & EPOLLOUT) {
            handler->HandleWrite(fd);
        }

        if (events & EPOLLERR) {
            handler->HandleError(fd);
        }

        if (events & (EPOLLHUP | EPOLLRDHUP)) {
            handler->HandleClose(fd);
        }
    } catch (const std::exception &e) {
        NETWORK_LOGE("Exception in event handler for fd %d: %s", fd, e.what());
    } catch (...) {
        NETWORK_LOGE("Unknown exception in event handler for fd %d", fd);
    }
}