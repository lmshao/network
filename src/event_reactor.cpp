/**
 * @file event_reactor.cpp
 * @brief Event Reactor Implementation for Network I/O
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#include "event_reactor.h"

#ifdef _WIN32
#include <errno.h>
#else
#include <errno.h>
#include <sys/prctl.h>

#include <cstring>
#endif

#include "log.h"

namespace {
#ifdef _WIN32
const HANDLE INVALID_IOCP_HANDLE = INVALID_HANDLE_VALUE;
const HANDLE INVALID_SHUTDOWN_EVENT = INVALID_HANDLE_VALUE;
constexpr DWORD IOCP_WAIT_EVENT_NUMS_MAX = 1024;
#else
constexpr int INVALID_WAKEUP_FD = -1;
constexpr int EPOLL_WAIT_EVENT_NUMS_MAX = 1024;
#endif
} // namespace

namespace lmshao::network {

EventReactor::EventReactor()
#ifdef _WIN32
    : iocpHandle_(INVALID_IOCP_HANDLE), shutdownEvent_(INVALID_SHUTDOWN_EVENT)
#else
    : wakeupFd_(INVALID_WAKEUP_FD)
#endif
{
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        NETWORK_LOGE("WSAStartup failed");
        return;
    }

    // Create shutdown event
    shutdownEvent_ = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (shutdownEvent_ == INVALID_HANDLE_VALUE) {
        NETWORK_LOGE("CreateEvent failed: %lu", GetLastError());
        return;
    }
#else
    wakeupFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif

    eventThread_ = std::make_unique<std::thread>([this]() { this->Run(); });
    SetThreadName("EventReactor");
    std::unique_lock<std::mutex> taskLock(signalMutex_);
    runningSignal_.wait_for(taskLock, std::chrono::milliseconds(5), [this] { return this->running_ == true; });
}

void EventReactor::WakeupEventLoop()
{
    // Unified wakeup mechanism for event loop
#ifdef _WIN32
    if (shutdownEvent_ != INVALID_SHUTDOWN_EVENT) {
        SetEvent(shutdownEvent_);
    }
#else
    if (wakeupFd_ != INVALID_WAKEUP_FD) {
        uint64_t one = 1;
        write(wakeupFd_, &one, sizeof(one));
    }
#endif
}

EventReactor::~EventReactor()
{
    NETWORK_LOGD("enter");
    running_ = false;

    // Use unified wakeup mechanism
    WakeupEventLoop();

    if (eventThread_ && eventThread_->joinable()) {
        eventThread_->join();
    }

    // Cleanup resources
#ifdef _WIN32
    if (shutdownEvent_ != INVALID_SHUTDOWN_EVENT) {
        CloseHandle(shutdownEvent_);
        shutdownEvent_ = INVALID_SHUTDOWN_EVENT;
    }

    if (iocpHandle_ != INVALID_IOCP_HANDLE) {
        CloseHandle(iocpHandle_);
        iocpHandle_ = INVALID_IOCP_HANDLE;
    }

    WSACleanup();
#else
    if (wakeupFd_ != INVALID_WAKEUP_FD) {
        close(wakeupFd_);
        wakeupFd_ = INVALID_WAKEUP_FD;
    }

    if (epollFd_ != -1) {
        close(epollFd_);
        epollFd_ = -1;
    }
#endif
}

void EventReactor::Run()
{
    NETWORK_LOGD("enter");

#ifdef _WIN32
    // Windows IOCP initialization
    iocpHandle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (iocpHandle_ == INVALID_HANDLE_VALUE) {
        NETWORK_LOGE("CreateIoCompletionPort failed: %lu", GetLastError());
        return;
    }

    running_ = true;
    runningSignal_.notify_all();

    DWORD bytesTransferred;
    ULONG_PTR completionKey;
    LPOVERLAPPED overlapped;
#else
    // Linux epoll initialization
    epollFd_ = epoll_create1(0);
    if (epollFd_ == -1) {
        NETWORK_LOGE("epoll_create %s", strerror(errno));
        return;
    }

    if (wakeupFd_ != INVALID_WAKEUP_FD) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = wakeupFd_;
        epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeupFd_, &ev);
    }

    int nfds = 0;
    running_ = true;
    runningSignal_.notify_all();
    struct epoll_event readyEvents[EPOLL_WAIT_EVENT_NUMS_MAX] = {};
#endif

    // Common event loop for both Windows and Linux
    while (running_) {
#ifdef _WIN32
        // Windows: Mixed model - poll listening sockets with select, data sockets with IOCP

        // First check for accept events on listening sockets using select
        fd_set readfds;
        FD_ZERO(&readfds);
        socket_t maxfd = 0;
        bool hasListeningSockets = false;

        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            for (const auto &pair : handlers_) {
                auto handler = pair.second;
                int events = handler->GetEvents();
                // Check if this is a listening socket (only has READ events and not doing data I/O)
                if ((events & static_cast<int>(EventType::EVT_READ)) &&
                    !(events & static_cast<int>(EventType::EVT_WRITE))) {
                    socket_t fd = pair.first;
                    FD_SET(fd, &readfds);
                    if (fd > maxfd)
                        maxfd = fd;
                    hasListeningSockets = true;
                }
            }
        }

        if (hasListeningSockets) {
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 10000; // 10ms

            int result = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
            if (result > 0) {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                for (const auto &pair : handlers_) {
                    socket_t fd = pair.first;
                    if (FD_ISSET(fd, &readfds)) {
                        NETWORK_LOGD("Select: accept event on listening socket: " SOCKET_FMT, fd);
                        lock.unlock();
                        DispatchEvent(fd, static_cast<int>(EventType::EVT_READ));
                        lock.lock();
                    }
                }
            }
        }

        // Then handle IOCP events for data sockets
        BOOL result = GetQueuedCompletionStatus(iocpHandle_, &bytesTransferred, &completionKey, &overlapped, 10);
        if (!result) {
            DWORD error = GetLastError();
            if (error == WAIT_TIMEOUT) {
                continue;
            }
            if (error == ERROR_ABANDONED_WAIT_0) {
                // Shutdown event signaled
                break;
            }
            NETWORK_LOGE("GetQueuedCompletionStatus failed: %lu", error);
            continue;
        }

        if (overlapped == NULL) {
            // Shutdown signal
            break;
        }

        // For our implementation, treat all IOCP completions as read events
        // The completionKey contains the socket descriptor
        socket_t socket = static_cast<socket_t>(completionKey);

        NETWORK_LOGD("IOCP completion: socket=" SOCKET_FMT ", bytes=%lu", socket, bytesTransferred);

        // Dispatch as read event
        DispatchEvent(socket, static_cast<int>(EventType::EVT_READ));
#else
        // Linux epoll event processing
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

            if (wakeupFd_ != INVALID_WAKEUP_FD && readyFd == wakeupFd_) {
                uint64_t tmp;
                read(wakeupFd_, &tmp, sizeof(tmp));
                continue;
            }

            // Reuse DispatchEvent for event handling
            DispatchEvent(readyFd, events);
        }
#endif
    }
}

void EventReactor::SetThreadName(const std::string &name)
{
    if (eventThread_ && !name.empty()) {
#ifndef _WIN32
        std::string threadName = name;
        if (threadName.size() > 15) {
            threadName = threadName.substr(0, 15);
        }
        pthread_setname_np(eventThread_->native_handle(), threadName.c_str());
#endif
        NETWORK_LOGD("Thread name set to: %s", name.c_str());
    }
}

bool EventReactor::RegisterHandler(std::shared_ptr<EventHandler> handler)
{
    if (!handler) {
        NETWORK_LOGE("Handler is nullptr");
        return false;
    }

#ifdef _WIN32
    socket_t socket = handler->GetHandle();
    int events = handler->GetEvents();
    NETWORK_LOGD("[%p] Register handler for socket:" SOCKET_FMT ", events:0x%x", this, socket, events);

    if (!running_) {
        NETWORK_LOGE("Reactor has exited");
        return false;
    }

    // Associate socket with IOCP
    HANDLE result =
        CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket), iocpHandle_, static_cast<ULONG_PTR>(socket), 0);

    if (result == NULL) {
        NETWORK_LOGE("CreateIoCompletionPort failed for socket " SOCKET_FMT ": %lu", socket, GetLastError());
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    handlers_.emplace(socket, handler);

    NETWORK_LOGD("Handler registered successfully for socket:" SOCKET_FMT, socket);
    return true;
#else
    socket_t fd = handler->GetHandle();
    int events = handler->GetEvents();
    NETWORK_LOGD("[%p] Register handler for fd:" SOCKET_FMT ", events:0x%x", this, fd, events);

    if (!running_) {
        NETWORK_LOGE("Reactor has exited");
        return false;
    }

    uint32_t epollEvents = 0;
    if (events & static_cast<int>(EventType::EVT_READ)) {
        epollEvents |= EPOLLIN;
    }
    if (events & static_cast<int>(EventType::EVT_WRITE)) {
        epollEvents |= EPOLLOUT;
    }
    if (events & static_cast<int>(EventType::EVT_ERROR)) {
        epollEvents |= EPOLLERR;
    }
    if (events & static_cast<int>(EventType::EVT_CLOSE)) {
        epollEvents |= EPOLLHUP | EPOLLRDHUP;
    }

    epollEvents |= EPOLLET;

    struct epoll_event ev;
    ev.events = epollEvents;
    ev.data.fd = fd;

    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
        NETWORK_LOGE("epoll_ctl ADD error for fd " SOCKET_FMT ": %s", fd, strerror(errno));
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    handlers_.emplace(fd, handler);

    NETWORK_LOGD("Handler registered successfully for fd:" SOCKET_FMT, fd);
    return true;
#endif
}

#ifdef _WIN32
bool EventReactor::RegisterHandlerWithoutIOCP(std::shared_ptr<EventHandler> handler)
{
    if (!handler) {
        NETWORK_LOGE("Handler is nullptr");
        return false;
    }

    socket_t socket = handler->GetHandle();
    NETWORK_LOGD("[%p] Register handler without IOCP for socket:" SOCKET_FMT, this, socket);

    if (!running_) {
        NETWORK_LOGE("Reactor has exited");
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    handlers_.emplace(socket, handler);

    NETWORK_LOGD("Handler registered successfully without IOCP for socket:" SOCKET_FMT, socket);
    return true;
}
#endif

bool EventReactor::RemoveHandler(socket_t fd)
{
    NETWORK_LOGD("Remove handler for fd(" SOCKET_FMT ")", fd);
#ifndef _WIN32
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) {
        NETWORK_LOGW("Handler not found for fd:" SOCKET_FMT, fd);
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
        NETWORK_LOGE("epoll_ctl DEL error for fd " SOCKET_FMT ": %s", fd, strerror(errno));
        return false;
    }

    NETWORK_LOGD("Handler removed successfully for fd:" SOCKET_FMT, fd);
#endif
    return true;
}

bool EventReactor::ModifyHandler(socket_t fd, int events)
{
#ifndef _WIN32
    NETWORK_LOGD("Modify handler for fd(" SOCKET_FMT "), events:0x%x", fd, events);

    if (!running_) {
        NETWORK_LOGE("Reactor has exited");
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) {
        NETWORK_LOGW("Handler not found for fd:" SOCKET_FMT " during modify", fd);
        return false;
    }

    uint32_t epollEvents = 0;
    if (events & static_cast<int>(EventType::EVT_READ)) {
        epollEvents |= EPOLLIN;
    }
    if (events & static_cast<int>(EventType::EVT_WRITE)) {
        epollEvents |= EPOLLOUT;
    }
    if (events & static_cast<int>(EventType::EVT_ERROR)) {
        epollEvents |= EPOLLERR;
    }
    if (events & static_cast<int>(EventType::EVT_CLOSE)) {
        epollEvents |= EPOLLHUP | EPOLLRDHUP;
    }

    epollEvents |= EPOLLET;

    struct epoll_event ev;
    ev.events = epollEvents;
    ev.data.fd = fd;

    if (epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
        NETWORK_LOGE("epoll_ctl MOD error for fd " SOCKET_FMT ": %s", fd, strerror(errno));
        return false;
    }

    NETWORK_LOGD("Handler modified successfully for fd:" SOCKET_FMT, fd);

#endif
    return true;
}

void EventReactor::DispatchEvent(socket_t fd, int events)
{
    NETWORK_LOGD("Dispatching event for fd:" SOCKET_FMT ", events:0x%x", fd, events);

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) {
        NETWORK_LOGW("Handler not found for fd:" SOCKET_FMT, fd);
        return;
    }

    auto handler = it->second;
    lock.unlock();

    try {
#ifdef _WIN32
        // Windows: events are EventType enum values
        if (events & static_cast<int>(EventType::EVT_READ)) {
            NETWORK_LOGD("Calling HandleRead for fd:" SOCKET_FMT, fd);
            handler->HandleRead(fd);
        }

        if (events & static_cast<int>(EventType::EVT_WRITE)) {
            NETWORK_LOGD("Calling HandleWrite for fd:" SOCKET_FMT, fd);
            handler->HandleWrite(fd);
        }

        if (events & static_cast<int>(EventType::EVT_ERROR)) {
            NETWORK_LOGD("Calling HandleError for fd:" SOCKET_FMT, fd);
            handler->HandleError(fd);
        }

        if (events & static_cast<int>(EventType::EVT_CLOSE)) {
            NETWORK_LOGD("Calling HandleClose for fd:" SOCKET_FMT, fd);
            handler->HandleClose(fd);
        }
#else
        // Linux: events are epoll event flags
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
#endif
    } catch (const std::exception &e) {
        NETWORK_LOGE("Exception in event handler for fd " SOCKET_FMT ": %s", fd, e.what());
    } catch (...) {
        NETWORK_LOGE("Unknown exception in event handler for fd " SOCKET_FMT, fd);
    }
}

} // namespace lmshao::network