/**
 * @file udp_client.cpp
 * @brief UDP Client Implementation
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#include "udp_client.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <assert.h>

#include "event_reactor.h"
#include "log.h"

namespace lmshao::network {
constexpr int RECV_BUFFER_MAX_SIZE = 4096;

class UdpClientHandler : public EventHandler {
public:
    UdpClientHandler(socket_t fd, std::weak_ptr<UdpClient> client) : fd_(fd), client_(client) {}

    void HandleRead(socket_t fd) override
    {
        if (auto client = client_.lock()) {
            client->HandleReceive(fd);
        }
    }

    void HandleWrite(socket_t fd) override {}

    void HandleError(socket_t fd) override
    {
        NETWORK_LOGE("UDP client connection error on socket: " SOCKET_FMT, fd);
        if (auto client = client_.lock()) {
            client->HandleConnectionClose(fd, true, "Connection error");
        }
    }

    void HandleClose(socket_t fd) override
    {
        NETWORK_LOGD("UDP client connection close on socket: " SOCKET_FMT, fd);
        if (auto client = client_.lock()) {
            client->HandleConnectionClose(fd, false, "Connection closed");
        }
    }

    socket_t GetHandle() const override { return fd_; }

    int GetEvents() const override
    {
        return static_cast<int>(EventType::READ) | static_cast<int>(EventType::ERROR) |
               static_cast<int>(EventType::CLOSE);
    }

private:
    socket_t fd_;
    std::weak_ptr<UdpClient> client_;
};

UdpClient::UdpClient(std::string remoteIp, uint16_t remotePort, std::string localIp, uint16_t localPort)
    : remoteIp_(remoteIp), remotePort_(remotePort), localIp_(localIp), localPort_(localPort)
{
    taskQueue_ = std::make_unique<TaskQueue>("UdpClientCb");
}

UdpClient::~UdpClient()
{
    if (taskQueue_) {
        taskQueue_->Stop();
        taskQueue_.reset();
    }
    Close();
}

bool UdpClient::Init()
{
#ifdef _WIN32
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket error: %d", WSAGetLastError());
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
    socket_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket error: %s", strerror(errno));
        return false;
    }
#endif

    memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_port = htons(remotePort_);

#ifdef _WIN32
    inet_pton(AF_INET, remoteIp_.c_str(), &serverAddr_.sin_addr);
#else
    inet_aton(remoteIp_.c_str(), &serverAddr_.sin_addr);
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
        int ret = bind(socket_, (struct sockaddr *)&localAddr, (int)sizeof(localAddr));
        if (ret != 0) {
            NETWORK_LOGE("bind error: %d", WSAGetLastError());
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
#else
        inet_aton(localIp_.c_str(), &localAddr.sin_addr);
        int ret = bind(socket_, (struct sockaddr *)&localAddr, (socklen_t)sizeof(localAddr));
        if (ret != 0) {
            NETWORK_LOGE("bind error: %s", strerror(errno));
            return false;
        }
#endif
    }

    taskQueue_->Start();
    clientHandler_ = std::make_shared<UdpClientHandler>(socket_, shared_from_this());
    if (!EventReactor::GetInstance()->RegisterHandler(clientHandler_)) {
        NETWORK_LOGE("Failed to register UDP client handler");
        return false;
    }

    NETWORK_LOGD("UdpClient initialized with new EventHandler interface");
    return true;
}

void UdpClient::Close()
{
    if (socket_ != INVALID_SOCKET && clientHandler_) {
        EventReactor::GetInstance()->RemoveHandler(socket_);
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        socket_ = INVALID_SOCKET;
        clientHandler_.reset();
    }
}

bool UdpClient::Send(const void *data, size_t len)
{
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket not initialized");
        return false;
    }

    if (!data || len == 0) {
        NETWORK_LOGE("invalid send parameters: data=%p, len=%zu", data, len);
        return false;
    }

#ifdef _WIN32
    int nbytes = sendto(socket_, (const char*)data, (int)len, 0, (struct sockaddr *)&serverAddr_, (int)sizeof(serverAddr_));
    if (nbytes == SOCKET_ERROR) {
        NETWORK_LOGE("sendto error: %d", WSAGetLastError());
        return false;
    }
#else
    ssize_t nbytes = sendto(socket_, data, len, 0, (struct sockaddr *)&serverAddr_, (socklen_t)(sizeof(serverAddr_)));
    if (nbytes == -1) {
        NETWORK_LOGE("sendto error: %s", strerror(errno));
        return false;
    }
#endif
    return true;
}

bool UdpClient::Send(const std::string &str)
{
    if (str.empty()) {
        NETWORK_LOGE("invalid send parameters: empty string");
        return false;
    }
    return Send(str.data(), str.size());
}

bool UdpClient::Send(std::shared_ptr<DataBuffer> data)
{
    if (!data)
        return false;
    return Send(data->Data(), data->Size());
}

void UdpClient::HandleReceive(socket_t fd)
{
    NETWORK_LOGD("fd: " SOCKET_FMT, fd);
    if (readBuffer_ == nullptr) {
        readBuffer_ = DataBuffer::PoolAlloc(RECV_BUFFER_MAX_SIZE);
    }

    while (true) {
#ifdef _WIN32
        ssize_t nbytes = recv(fd, (char*)readBuffer_->Data(), (int)readBuffer_->Capacity(), 0);
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
            // Do not call HandleConnectionClose directly; let the event system handle EPOLLHUP
            break;
        } else {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                break;
            }
            NETWORK_LOGE("recv error: %d", err);
            std::string info = std::to_string(err);
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::string info = strerror(errno);
            NETWORK_LOGE("recv error: %s(%d)", info.c_str(), errno);
#endif
            HandleConnectionClose(fd, true, info);

        }
        break;
    }
}

void UdpClient::HandleConnectionClose(socket_t fd, bool isError, const std::string &reason)
{
    NETWORK_LOGD("Closing UDP client connection fd: " SOCKET_FMT ", reason: %s, isError: %s", fd, reason.c_str(),
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