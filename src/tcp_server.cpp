//
// Copyright © 2024 SHAO Liming <lmshao@163.com>. All rights reserved.
//

#include "tcp_server.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

#include "event_reactor.h"
#include "log.h"
#include "session_impl.h"
constexpr int TCP_BACKLOG = 10;
constexpr int RECV_BUFFER_MAX_SIZE = 4096;
constexpr int TCP_KEEP_IDLE = 3;     // Start probing after 3 seconds of no data interaction
constexpr int TCP_KEEP_INTERVAL = 1; // Probe interval 1 second
constexpr int TCP_KEEP_COUNT = 2;    // Probe 2 times
TcpServer::~TcpServer()
{
    NETWORK_LOGD("fd:%d", socket_);
    Stop();
}

bool TcpServer::Init()
{
    socket_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGD("socket error: %s", strerror(errno));
        return false;
    }
    NETWORK_LOGD("init ip: %s, port: %d fd:%d", localIp_.c_str(), localPort_, socket_);

    int optval = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        NETWORK_LOGD("setsockopt error: %s", strerror(errno));
        return false;
    }

    memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_port = htons(localPort_);
    inet_aton(localIp_.c_str(), &serverAddr_.sin_addr);

    if (bind(socket_, (struct sockaddr *)&serverAddr_, sizeof(serverAddr_)) < 0) {
        NETWORK_LOGD("bind error: %s", strerror(errno));
        return false;
    }

    if (listen(socket_, TCP_BACKLOG) < 0) {
        NETWORK_LOGD("listen error: %s", strerror(errno));
        return false;
    }

    taskQueue_ = std::make_unique<TaskQueue>("TcPServerCb");
    return true;
}

bool TcpServer::Start()
{
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGD("socket not initialized");
        return false;
    }

    taskQueue_->Start();
    EventReactor::GetInstance()->AddDescriptor(socket_, [&](int fd) { this->HandleAccept(fd); });

    return true;
}

bool TcpServer::Stop()
{
    std::vector<int> clientFds;
    for (const auto &[fd, session] : sessions_) {
        clientFds.push_back(fd);
    }

    for (int clientFd : clientFds) {
        NETWORK_LOGD("close client fd: %d", clientFd);
        EventReactor::GetInstance()->RemoveDescriptor(clientFd);
        close(clientFd);
    }
    sessions_.clear();

    if (socket_ != INVALID_SOCKET) {
        NETWORK_LOGD("close server fd: %d", socket_);
        EventReactor::GetInstance()->RemoveDescriptor(socket_);
        close(socket_);
        socket_ = INVALID_SOCKET;
    }

    if (taskQueue_) {
        taskQueue_->Stop();
        taskQueue_.reset();
    }

    return true;
}

bool TcpServer::Send(int fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer)
{
    if (sessions_.find(fd) == sessions_.end()) {
        NETWORK_LOGD("invalid session fd");
        return false;
    }

    size_t bytes = send(fd, buffer->Data(), buffer->Size(), 0);
    if (bytes != buffer->Size()) {
        NETWORK_LOGD("send failed with error: %s, %zd/%zu", strerror(errno), bytes, buffer->Size());
        return false;
    }

    return true;
}

bool TcpServer::Send(int fd, std::string host, uint16_t port, const std::string &str)
{
    if (sessions_.find(fd) == sessions_.end()) {
        NETWORK_LOGD("invalid session fd");
        return false;
    }

    size_t bytes = send(fd, str.c_str(), str.length(), 0);
    if (bytes != str.length()) {
        NETWORK_LOGD("send failed with error: %s, %zd/%zu", strerror(errno), bytes, str.length());
        return false;
    }

    return true;
}

void TcpServer::HandleAccept(int fd)
{
    NETWORK_LOGD("enter");
    struct sockaddr_in clientAddr = {};
    socklen_t addrLen = sizeof(struct sockaddr_in);
    int clientSocket = accept4(fd, (struct sockaddr *)&clientAddr, &addrLen, SOCK_NONBLOCK);
    if (clientSocket < 0) {
        NETWORK_LOGD("accept error: %s", strerror(errno));
        return;
    }

    EventReactor::GetInstance()->AddDescriptor(clientSocket, [&](int fd) { this->HandleReceive(fd); });

    EnableKeepAlive(clientSocket);

    std::string host = inet_ntoa(clientAddr.sin_addr);
    uint16_t port = ntohs(clientAddr.sin_port);

    NETWORK_LOGD("New client connections client[%d] %s:%d\n", clientSocket, inet_ntoa(clientAddr.sin_addr),
                 ntohs(clientAddr.sin_port));

    auto session = std::make_shared<SessionImpl>(clientSocket, host, port, shared_from_this());
    sessions_.emplace(clientSocket, session);

    if (!listener_.expired()) {
        auto task = std::make_shared<TaskHandler<void>>([=]() {
            NETWORK_LOGD("invoke OnAccept callback");
            auto listener = listener_.lock();
            if (listener) {
                listener->OnAccept(sessions_[clientSocket]);
            } else {
                NETWORK_LOGD("not found listener!");
            }
        });
        if (taskQueue_) {
            taskQueue_->EnqueueTask(task);
        }
    } else {
        NETWORK_LOGD("listener is null");
    }
}

void TcpServer::HandleReceive(int fd)
{
    NETWORK_LOGD("fd: %d", fd);
    if (readBuffer_ == nullptr) {
        readBuffer_ = std::make_unique<DataBuffer>(RECV_BUFFER_MAX_SIZE);
    }

    while (true) {
        ssize_t nbytes = recv(fd, readBuffer_->Data(), readBuffer_->Capacity(), 0);

        if (nbytes > 0) {
            if (nbytes > RECV_BUFFER_MAX_SIZE) {
                NETWORK_LOGD("recv %zd bytes", nbytes);
                break;
            }

            if (!listener_.expired()) {
                auto dataBuffer = std::make_shared<DataBuffer>(nbytes);
                dataBuffer->Assign(readBuffer_->Data(), nbytes);

                std::shared_ptr<Session> session;
                auto it = sessions_.find(fd);
                if (it != sessions_.end()) {
                    session = it->second;
                }

                if (session) {
                    auto task = std::make_shared<TaskHandler<void>>([session, dataBuffer, this]() {
                        auto listener = listener_.lock();
                        if (listener != nullptr) {
                            listener->OnReceive(session, dataBuffer);
                        }
                    });
                    if (taskQueue_) {
                        taskQueue_->EnqueueTask(task);
                    }
                }
            }
            continue;
        } else if (nbytes == 0) {
            NETWORK_LOGW("Disconnect fd[%d]", fd);
            EventReactor::GetInstance()->RemoveDescriptor(fd);
            close(fd);

            std::shared_ptr<Session> session;
            auto it = sessions_.find(fd);
            if (it != sessions_.end()) {
                session = it->second;
                sessions_.erase(it);
            }

            if (!listener_.expired() && session) {
                auto task = std::make_shared<TaskHandler<void>>([session, this]() {
                    auto listener = listener_.lock();
                    if (listener != nullptr) {
                        listener->OnClose(session);
                    }
                });
                if (taskQueue_) {
                    taskQueue_->EnqueueTask(task);
                }
            }
        } else {
            std::string info = strerror(errno);
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (IsConnectionAlive(fd)) {
                    NETWORK_LOGD("EAGAIN: connection is alive");
                    break;
                } else {
                    NETWORK_LOGD("EAGAIN: connection is dead");
                }
            }
            NETWORK_LOGD("recv error: %s(%d)", info.c_str(), errno);

            if (errno == ETIMEDOUT) {
                NETWORK_LOGD("ETIME: connection is timeout");
                break;
            }

            EventReactor::GetInstance()->RemoveDescriptor(fd);
            close(fd);

            std::shared_ptr<Session> session;
            auto it = sessions_.find(fd);
            if (it != sessions_.end()) {
                session = it->second;
                sessions_.erase(it);
            }

            if (!listener_.expired() && session) {
                auto task = std::make_shared<TaskHandler<void>>([session, info, this]() {
                    auto listener = listener_.lock();
                    if (listener != nullptr) {
                        listener->OnError(session, info);
                    }
                });
                if (taskQueue_) {
                    taskQueue_->EnqueueTask(task);
                }
            }
        }

        break;
    }
}

bool TcpServer::IsConnectionAlive(int fd)
{
    fd_set readfds;
    struct timeval timeout;

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000; // 1ms

    int result = select(fd + 1, &readfds, nullptr, nullptr, &timeout);
    if (result < 0) {
        return false;
    }
    return true;
}

void TcpServer::EnableKeepAlive(int fd)
{
    int keepAlive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &TCP_KEEP_IDLE, sizeof(TCP_KEEP_IDLE));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &TCP_KEEP_INTERVAL, sizeof(TCP_KEEP_INTERVAL));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &TCP_KEEP_COUNT, sizeof(TCP_KEEP_COUNT));
}