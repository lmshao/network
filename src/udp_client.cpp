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
        return static_cast<int>(EventType::EVT_READ) | static_cast<int>(EventType::EVT_ERROR) |
               static_cast<int>(EventType::EVT_CLOSE);
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
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        NETWORK_LOGE("WSAStartup failed");
        return false;
    }

    socket_ = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
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
    } else {
        // Always bind to receive data for UDP
        struct sockaddr_in localAddr;
        memset(&localAddr, 0, sizeof(localAddr));
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = INADDR_ANY;
        localAddr.sin_port = 0; // Let system choose port

#ifdef _WIN32
        int ret = bind(socket_, (struct sockaddr *)&localAddr, (int)sizeof(localAddr));
        if (ret != 0) {
            NETWORK_LOGE("bind error: %d", WSAGetLastError());
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
#else
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

#ifdef _WIN32
    // Start async receive for Windows IOCP
    StartAsyncReceive();
#endif

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
    WSABUF wsaBuf;
    wsaBuf.buf = (CHAR *)data;
    wsaBuf.len = (ULONG)len;
    DWORD bytesSent = 0;
    int ret = WSASendTo(socket_, &wsaBuf, 1, &bytesSent, 0, (struct sockaddr *)&serverAddr_, (int)sizeof(serverAddr_),
                        NULL, NULL);
    int nbytes = (ret == 0) ? bytesSent : SOCKET_ERROR;
    if (nbytes == SOCKET_ERROR) {
        NETWORK_LOGE("sendto error: %d", WSAGetLastError());
        return false;
    }
    NETWORK_LOGE("WSASendTo sent %d bytes", nbytes);
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

#ifdef _WIN32
    // Windows: data is already received via IOCP into readBuffer_
    // Get the bytes received from IOCP completion
    DWORD bytesReceived = 0;
    DWORD flags = 0;
    if (WSAGetOverlappedResult(socket_, &recvOverlapped_, &bytesReceived, FALSE, &flags)) {
        NETWORK_LOGD("recvfrom size: %d", (int)bytesReceived);

        if (bytesReceived > 0) {
            if (!listener_.expired()) {
                auto dataBuffer = DataBuffer::PoolAlloc(bytesReceived);
                dataBuffer->Assign(readBuffer_->Data(), bytesReceived);
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
        }
    }

    // Start next async receive
    StartAsyncReceive();
#else
    // Linux: synchronous receive with recvfrom
    if (readBuffer_ == nullptr) {
        readBuffer_ = DataBuffer::PoolAlloc(RECV_BUFFER_MAX_SIZE);
    }

    while (true) {
        struct sockaddr_in fromAddr;
        socklen_t fromAddrLen = sizeof(fromAddr);
        ssize_t nbytes = recvfrom(fd, readBuffer_->Data(), readBuffer_->Capacity(), MSG_DONTWAIT,
                                  (struct sockaddr *)&fromAddr, &fromAddrLen);

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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::string info = strerror(errno);
            NETWORK_LOGE("recvfrom error: %s(%d)", info.c_str(), errno);
            HandleConnectionClose(fd, true, info);
            break;
        }
    }
#endif
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

#ifdef _WIN32
void UdpClient::StartAsyncReceive()
{
    if (readBuffer_ == nullptr) {
        readBuffer_ = DataBuffer::PoolAlloc(RECV_BUFFER_MAX_SIZE);
    }

    // Prepare for async receive
    memset(&recvOverlapped_, 0, sizeof(recvOverlapped_));
    recvBuffer_.buf = (char *)readBuffer_->Data();
    recvBuffer_.len = (ULONG)readBuffer_->Capacity();
    remoteAddrLen_ = sizeof(remoteAddr_);

    DWORD flags = 0;
    DWORD bytesReceived = 0;

    int result = WSARecvFrom(socket_, &recvBuffer_, 1, &bytesReceived, &flags, (struct sockaddr *)&remoteAddr_,
                             &remoteAddrLen_, &recvOverlapped_, NULL);

    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSA_IO_PENDING) {
            NETWORK_LOGE("WSARecvFrom failed: %d", error);
        }
        // WSA_IO_PENDING is expected for async operation
    }
}
#endif

} // namespace lmshao::network