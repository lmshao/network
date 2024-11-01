//
// Copyright © 2024 SHAO Liming <lmshao@163.com>. All rights reserved.
//

#include "tcp_server.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "event_reactor.h"
#include "log.h"
#include "thread_pool.h"

const int RECV_BUFFER_MAX_SIZE = 4096;
const int TCP_BACKLOG = 10;

TcpServer::~TcpServer()
{
    Stop();
}

bool TcpServer::Init()
{
    socket_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket error: %s", strerror(errno));
        return false;
    }
    NETWORK_LOGD("... fd:%d", socket_);

    int optval = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        NETWORK_LOGE("setsockopt error: %s", strerror(errno));
        return false;
    }

    memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_port = htons(localPort_);
    inet_aton(localIp_.c_str(), &serverAddr_.sin_addr);

    if (bind(socket_, (struct sockaddr *)&serverAddr_, sizeof(serverAddr_)) < 0) {
        NETWORK_LOGE("bind error: %s", strerror(errno));
        return false;
    }

    if (listen(socket_, TCP_BACKLOG) < 0) {
        NETWORK_LOGE("listen error: %s", strerror(errno));
        return false;
    }

    return true;
}

bool TcpServer::Start()
{
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket not initialized");
        return false;
    }

    if (!listener_.expired()) {
        callbackThreads_ = std::make_unique<ThreadPool>(1, 1, "TcpServer-cb");
    }

    EventReactor::GetInstance()->AddDescriptor(socket_, [&](int fd) { this->HandleAccept(fd); });

    return true;
}

bool TcpServer::Stop()
{
    if (socket_ != INVALID_SOCKET) {
        callbackThreads_.reset();
        EventReactor::GetInstance()->RemoveDescriptor(socket_);
        close(socket_);
        socket_ = INVALID_SOCKET;
    }

    return true;
}

bool TcpServer::Send(int fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer)
{
    if (sessions_.find(fd) == sessions_.end()) {
        NETWORK_LOGE("invalid session fd");
        return false;
    }

    ssize_t bytes = send(fd, buffer->Data(), buffer->Size(), 0);
    if (bytes != buffer->Size()) {
        printf("Send scuccess, length:%zd\n\n", bytes);
        NETWORK_LOGE("send failed with error: %s, %zd/%zu", strerror(errno), bytes, buffer->Size());
        return false;
    }

    return true;
}

void TcpServer::HandleAccept(int fd)
{
    NETWORK_LOGD("enter");
    struct sockaddr_in clientAddr = {};
    socklen_t addrLen = sizeof(struct sockaddr_in);
    int clientSocket = accept(fd, (struct sockaddr *)&clientAddr, &addrLen);
    if (clientSocket < 0) {
        NETWORK_LOGE("accept error: %s", strerror(errno));
        return;
    }

    EventReactor::GetInstance()->AddDescriptor(clientSocket, [&](int fd) { this->HandleReceive(fd); });

    std::string host = inet_ntoa(clientAddr.sin_addr);
    uint16_t port = ntohs(clientAddr.sin_port);

    NETWORK_LOGD("New client connections client[%d] %s:%d\n", clientSocket, inet_ntoa(clientAddr.sin_addr),
                 ntohs(clientAddr.sin_port));

    auto session = std::make_shared<SessionImpl>(clientSocket, host, port, shared_from_this());
    sessions_.emplace(clientSocket, session);

    if (!listener_.expired()) {
        callbackThreads_->AddTask(
            [=]() {
                NETWORK_LOGD("invoke OnAccept callback");
                auto listener = listener_.lock();
                if (listener) {
                    listener->OnAccept(sessions_[clientSocket]);
                } else {
                    NETWORK_LOGE("not found listener!");
                }
            },
            std::to_string(fd));
    } else {
        NETWORK_LOGE("listener is null");
    }
}

void TcpServer::HandleReceive(int fd)
{
    NETWORK_LOGD("fd: %d", fd);
    static char buffer[RECV_BUFFER_MAX_SIZE] = {};
    memset(buffer, 0, RECV_BUFFER_MAX_SIZE);

    while (true) {
        ssize_t nbytes = recv(fd, buffer, sizeof(buffer), 0);
        if (nbytes > 0) {
            if (!listener_.expired()) {
                auto dataBuffer = std::make_shared<DataBuffer>(nbytes);
                dataBuffer->Assign(buffer, nbytes);
                callbackThreads_->AddTask(
                    [=]() {
                        auto listener = listener_.lock();
                        if (listener) {
                            listener->OnReceive(sessions_[fd], dataBuffer);
                        }
                    },
                    std::to_string(fd));
            }
            continue;
        }

        if (nbytes == 0) {
            NETWORK_LOGW("Disconnect fd[%d]", fd);
            EventReactor::GetInstance()->RemoveDescriptor(fd);
            close(fd);

            if (!listener_.expired()) {
                callbackThreads_->AddTask(
                    [=]() {
                        auto listener = listener_.lock();
                        if (listener) {
                            listener->OnClose(sessions_[fd]);
                            sessions_.erase(fd);
                        } else {
                            NETWORK_LOGE("not found listener!");
                        }
                    },
                    std::to_string(fd));
            }

        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            std::string info = strerror(errno);
            NETWORK_LOGE("recv error: %s", info.c_str());
            EventReactor::GetInstance()->RemoveDescriptor(fd);
            close(fd);

            if (!listener_.expired()) {
                callbackThreads_->AddTask(
                    [=]() {
                        auto listener = listener_.lock();
                        if (listener) {
                            listener->OnError(sessions_[fd], info);
                            sessions_.erase(fd);
                        } else {
                            NETWORK_LOGE("not found listener!");
                        }
                    },
                    std::to_string(fd));
            }
        }

        break;
    }
}