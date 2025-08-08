/**
 * @file tcp_client.cpp
 * @brief TCP Client Implementation
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#include "tcp_client.h"

#ifdef _WIN32
#include <mstcpip.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define MSG_NOSIGNAL 0
#define MSG_DONTWAIT 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <queue>

#include "event_reactor.h"
#include "log.h"

namespace lmshao::network {
constexpr int RECV_BUFFER_MAX_SIZE = 4096;

class TcpClientHandler : public EventHandler {
public:
    TcpClientHandler(socket_t fd, std::weak_ptr<TcpClient> client)
        : fd_(fd), client_(client), writeEventsEnabled_(false)
    {
    }

    void HandleRead(socket_t fd) override
    {
        if (auto client = client_.lock()) {
            client->HandleReceive(fd);
        }
    }

    void HandleWrite(socket_t fd) override { ProcessSendQueue(); }

    void HandleError(socket_t fd) override
    {
        NETWORK_LOGE("Client connection error on fd: " SOCKET_FMT, fd);
        if (auto client = client_.lock()) {
            client->HandleConnectionClose(fd, true, "Connection error");
        }
    }

    void HandleClose(socket_t fd) override
    {
        NETWORK_LOGD("Client connection close on fd: " SOCKET_FMT, fd);
        if (auto client = client_.lock()) {
            client->HandleConnectionClose(fd, false, "Connection closed");
        }
    }

    socket_t GetHandle() const override { return fd_; }

    int GetEvents() const override
    {
        int events = static_cast<int>(EventType::EVT_READ) | static_cast<int>(EventType::EVT_ERROR) |
                     static_cast<int>(EventType::EVT_CLOSE);

        if (writeEventsEnabled_) {
            events |= static_cast<int>(EventType::EVT_WRITE);
        }

        return events;
    }

    void QueueSend(std::shared_ptr<DataBuffer> buffer)
    {
        if (!buffer || buffer->Size() == 0)
            return;
        sendQueue_.push(buffer);
        EnableWriteEvents();
    }

private:
    void EnableWriteEvents()
    {
        if (!writeEventsEnabled_) {
            writeEventsEnabled_ = true;
            EventReactor::GetInstance()->ModifyHandler(fd_, GetEvents());
        }
    }

    void DisableWriteEvents()
    {
        if (writeEventsEnabled_) {
            writeEventsEnabled_ = false;
            EventReactor::GetInstance()->ModifyHandler(fd_, GetEvents());
        }
    }

    void ProcessSendQueue()
    {
#ifndef _WIN32
        while (!sendQueue_.empty()) {
            auto &buf = sendQueue_.front();
            ssize_t bytesSent = send(fd_, buf->Data(), buf->Size(), MSG_NOSIGNAL);

            if (bytesSent > 0) {
                if (static_cast<size_t>(bytesSent) == buf->Size()) {
                    sendQueue_.pop();
                } else {
                    auto remaining = DataBuffer::PoolAlloc(buf->Size() - bytesSent);
                    remaining->Assign(buf->Data() + bytesSent, buf->Size() - bytesSent);
                    sendQueue_.front() = remaining;
                    break;
                }
            } else if (bytesSent == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else {
                    NETWORK_LOGE("Send error on fd %d: %s", fd_, strerror(errno));
                    return;
                }
            }
        }

        if (sendQueue_.empty()) {
            DisableWriteEvents();
        }
#endif
    }

private:
    socket_t fd_;
    std::weak_ptr<TcpClient> client_;
    std::queue<std::shared_ptr<DataBuffer>> sendQueue_;
    bool writeEventsEnabled_;
};

TcpClient::TcpClient(std::string remoteIp, uint16_t remotePort, std::string localIp, uint16_t localPort)
    : remoteIp_(remoteIp), remotePort_(remotePort), localIp_(localIp), localPort_(localPort)
{
    taskQueue_ = std::make_unique<TaskQueue>("TcpClientCb");
}

TcpClient::~TcpClient()
{
    if (taskQueue_) {
        taskQueue_->Stop();
        taskQueue_.reset();
    }
    Close();
}

bool TcpClient::Init()
{
#ifdef _WIN32
    socket_ = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("WSASocket error: %d", WSAGetLastError());
        return false;
    }
    u_long mode = 1;
    if (ioctlsocket(socket_, FIONBIO, &mode) != 0) {
        NETWORK_LOGE("ioctlsocket FIONBIO error: %d", WSAGetLastError());
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
#else
    socket_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("Socket error: %s", strerror(errno));
        return false;
    }
#endif

    if (!localIp_.empty() || localPort_ != 0) {
        struct sockaddr_in localAddr;
        memset(&localAddr, 0, sizeof(localAddr));
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = htons(localPort_);
        if (localIp_.empty()) {
            localIp_ = "0.0.0.0";
        }
#ifdef _WIN32
        if (inet_pton(AF_INET, localIp_.c_str(), &localAddr.sin_addr) != 1) {
            NETWORK_LOGE("inet_pton localIp error");
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
        int optval = 1;
        if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval)) < 0) {
            NETWORK_LOGE("setsockopt SO_REUSEADDR error: %d", WSAGetLastError());
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
        int ret = bind(socket_, (struct sockaddr *)&localAddr, (int)sizeof(localAddr));
        if (ret != 0) {
            NETWORK_LOGE("bind error: %d", WSAGetLastError());
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
#else
        inet_aton(localIp_.c_str(), &localAddr.sin_addr);
        int optval = 1;
        if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
            NETWORK_LOGE("setsockopt SO_REUSEADDR error: %s", strerror(errno));
            close(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
        int ret = bind(socket_, (struct sockaddr *)&localAddr, (socklen_t)sizeof(localAddr));
        if (ret != 0) {
            NETWORK_LOGE("bind error: %s", strerror(errno));
            close(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
#endif
    }

    return true;
}

void TcpClient::ReInit()
{
    if (socket_ != INVALID_SOCKET) {
        close(socket_);
        socket_ = INVALID_SOCKET;
    }
    Init();
}

bool TcpClient::Connect()
{
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket not initialized");
        return false;
    }

    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_port = htons(remotePort_);
    if (remoteIp_.empty()) {
        remoteIp_ = "127.0.0.1";
    }
#ifdef _WIN32
    if (inet_pton(AF_INET, remoteIp_.c_str(), &serverAddr_.sin_addr) != 1) {
        NETWORK_LOGE("inet_pton remoteIp error");
        ReInit();
        return false;
    }
    int ret = connect(socket_, (struct sockaddr *)&serverAddr_, sizeof(serverAddr_));
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINPROGRESS) {
        NETWORK_LOGE("connect(%s:%d) failed: %d", remoteIp_.c_str(), remotePort_, WSAGetLastError());
        ReInit();
        return false;
    }
    int select_n = 0;
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(socket_, &writefds);
    TIMEVAL timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    select_n = select(0, NULL, &writefds, NULL, &timeout);
    int error = 0;
    int len = sizeof(error);
    if (select_n > 0) {
        if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, (char *)&error, &len) == SOCKET_ERROR) {
            NETWORK_LOGE("getsockopt error, %d", WSAGetLastError());
            ReInit();
            return false;
        }
    } else {
        error = WSAGetLastError();
    }
    if (error != 0) {
        NETWORK_LOGE("connect error, %d", error);
        ReInit();
        return false;
    }
#else
    inet_aton(remoteIp_.c_str(), &serverAddr_.sin_addr);
    int ret = connect(socket_, (struct sockaddr *)&serverAddr_, sizeof(serverAddr_));
    if (ret < 0 && errno != EINPROGRESS) {
        NETWORK_LOGE("connect(%s:%d) failed: %s", remoteIp_.c_str(), remotePort_, strerror(errno));
        ReInit();
        return false;
    }
    int select_n = 0;
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(socket_, &writefds);
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    select_n = select(socket_ + 1, NULL, &writefds, NULL, &timeout);
    int error = 0;
    socklen_t len = sizeof(error);
    if (select_n > 0) {
        if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
            NETWORK_LOGE("getsockopt error, %s", strerror(errno));
            ReInit();
            return false;
        }
    } else {
        error = errno;
    }
    if (error != 0) {
        NETWORK_LOGE("connect error, %s", strerror(error));
        ReInit();
        return false;
    }
#endif

    taskQueue_->Start();

    clientHandler_ = std::make_shared<TcpClientHandler>(socket_, shared_from_this());
    if (!EventReactor::GetInstance()->RegisterHandler(clientHandler_)) {
        NETWORK_LOGE("Failed to register client handler");
        return false;
    }

    NETWORK_LOGD("Connect (%s:%d) success with new EventHandler interface.", remoteIp_.c_str(), remotePort_);
    return true;
}

bool TcpClient::Send(const std::string &str)
{
    if (str.empty()) {
        NETWORK_LOGE("Invalid string data");
        return false;
    }

    auto buf = DataBuffer::PoolAlloc(str.size());
    buf->Assign(str.data(), str.size());
    return Send(buf);
}

bool TcpClient::Send(const char *data, size_t len)
{
    if (!data || len == 0) {
        NETWORK_LOGE("Invalid data");
        return false;
    }

    auto buf = DataBuffer::PoolAlloc(len);
    buf->Assign(data, len);
    return Send(buf);
}

bool TcpClient::Send(std::shared_ptr<DataBuffer> data)
{
    if (!data || data->Size() == 0) {
        NETWORK_LOGE("Invalid data buffer");
        return false;
    }

    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket not initialized");
        return false;
    }

#ifdef _WIN32
    // TODO: loop until the data is sent completely
    DWORD bytesSent = 0;
    WSABUF wsaBuf;
    wsaBuf.buf = (CHAR *)data->Data();
    wsaBuf.len = (ULONG)data->Size();
    int ret = WSASend(socket_, &wsaBuf, 1, &bytesSent, 0, NULL, NULL);
    if (ret == SOCKET_ERROR || bytesSent != data->Size()) {
        NETWORK_LOGE("WSASend error: %d, sent: %lu/%zu", WSAGetLastError(), bytesSent, data->Size());
        return false;
    }
    return true;
#else
    if (clientHandler_) {
        clientHandler_->QueueSend(data);
        return true;
    }
    NETWORK_LOGE("Client handler not found");
    return false;
#endif
}

void TcpClient::Close()
{
    if (socket_ != INVALID_SOCKET && clientHandler_) {
        EventReactor::GetInstance()->RemoveHandler(socket_);
        close(socket_);
        socket_ = INVALID_SOCKET;
        clientHandler_.reset();
    }
}

void TcpClient::HandleReceive(socket_t fd)
{
    NETWORK_LOGD("fd: " SOCKET_FMT, fd);
    if (readBuffer_ == nullptr) {
        readBuffer_ = DataBuffer::PoolAlloc(RECV_BUFFER_MAX_SIZE);
    }

    while (true) {
#ifdef _WIN32
        int nbytes = recv(fd, (char *)readBuffer_->Data(), (int)readBuffer_->Capacity(), 0);
#else
        ssize_t nbytes = recv(fd, readBuffer_->Data(), readBuffer_->Capacity(), MSG_DONTWAIT);
#endif
        if (nbytes > 0) {
            if (!listener_.expired()) {
                auto dataBuffer = DataBuffer::PoolAlloc(nbytes);
                dataBuffer->Assign(readBuffer_->Data(), nbytes);
                auto listenerWeak = listener_;
                auto task = std::make_shared<TaskHandler<void>>([listenerWeak, dataBuffer, fd]() {
                    auto listener = listenerWeak.lock();
                    if (listener) {
                        listener->OnReceive(fd, dataBuffer);
                    }
                });
                if (taskQueue_) {
                    taskQueue_->EnqueueTask(task);
                }
            }
            continue;
        } else if (nbytes == 0) {
            NETWORK_LOGW("Disconnect fd[" SOCKET_FMT "]", fd);
            break;
        } else {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                break;
            }
            std::string info = std::to_string(err);
            NETWORK_LOGE("recv error: %s(%d)", info.c_str(), err);
            HandleConnectionClose(fd, true, info);
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::string info = strerror(errno);
            NETWORK_LOGE("recv error: %s(%d)", info.c_str(), errno);
            HandleConnectionClose(fd, true, info);
#endif
        }
        break;
    }
}

void TcpClient::HandleConnectionClose(socket_t fd, bool isError, const std::string &reason)
{
    NETWORK_LOGD("Closing client connection fd: " SOCKET_FMT ", reason: %s, isError: %s", fd, reason.c_str(),
                 isError ? "true" : "false");

    if (socket_ != fd) {
        NETWORK_LOGD("Connection fd: " SOCKET_FMT " already cleaned up", fd);
        return;
    }

    EventReactor::GetInstance()->RemoveHandler(fd);
    close(fd);
    socket_ = INVALID_SOCKET;
    clientHandler_.reset();

    if (!listener_.expired()) {
        auto listenerWeak = listener_;
        auto task = std::make_shared<TaskHandler<void>>([listenerWeak, reason, isError, fd]() {
            auto listener = listenerWeak.lock();
            if (listener != nullptr) {
                if (isError) {
                    listener->OnError(fd, reason);
                } else {
                    listener->OnClose(fd);
                }
            }
        });
        if (taskQueue_) {
            taskQueue_->EnqueueTask(task);
        }
    }
}
} // namespace lmshao::network