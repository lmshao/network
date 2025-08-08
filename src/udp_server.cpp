/**
 * @file udp_server.cpp
 * @brief UDP Server Implementation
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#include "udp_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <cerrno>

#include "event_reactor.h"
#include "log.h"
#include "session_impl.h"

namespace lmshao::network {
constexpr int RECV_BUFFER_MAX_SIZE = 4096;
constexpr uint16_t UDP_SERVER_DEFAULT_PORT_START = 10000;
static uint16_t gIdlePort = UDP_SERVER_DEFAULT_PORT_START;

class UdpServerHandler : public EventHandler {
public:
    explicit UdpServerHandler(std::weak_ptr<UdpServer> server) : server_(server) {}

    void HandleRead(socket_t fd) override
    {
        if (auto server = server_.lock()) {
            server->HandleReceive(fd);
        }
    }

    void HandleWrite(socket_t fd) override {}

    void HandleError(socket_t fd) override { NETWORK_LOGE("UDP server socket error on fd: " SOCKET_FMT, fd); }

    void HandleClose(socket_t fd) override { NETWORK_LOGD("UDP server socket close on fd: " SOCKET_FMT, fd); }

    socket_t GetHandle() const override
    {
        if (auto server = server_.lock()) {
            return server->GetSocketFd();
        }
        return -1;
    }

    int GetEvents() const override
    {
        return static_cast<int>(EventType::EVT_READ) | static_cast<int>(EventType::EVT_ERROR) |
               static_cast<int>(EventType::EVT_CLOSE);
    }

private:
    std::weak_ptr<UdpServer> server_;
};

UdpServer::~UdpServer()
{
    NETWORK_LOGD("destructor");
    Stop();
}

bool UdpServer::Init()
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
    NETWORK_LOGD("fd:" SOCKET_FMT, socket_);

    int optval = 1;
#ifdef _WIN32
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof(optval)) < 0) {
        NETWORK_LOGE("setsockopt error: %d", WSAGetLastError());
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
#else
    if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        NETWORK_LOGE("setsockopt error: %s", strerror(errno));
        close(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
#endif

    memset(&serverAddr_, 0, sizeof(serverAddr_));
    serverAddr_.sin_family = AF_INET;
    serverAddr_.sin_port = htons(localPort_);
#ifdef _WIN32
    if (inet_pton(AF_INET, localIp_.c_str(), &serverAddr_.sin_addr) != 1) {
        NETWORK_LOGE("inet_pton localIp error");
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
#else
    inet_aton(localIp_.c_str(), &serverAddr_.sin_addr);
#endif

#ifdef _WIN32
    if (bind(socket_, (struct sockaddr *)&serverAddr_, (int)sizeof(serverAddr_)) < 0) {
        NETWORK_LOGE("bind error: %d", WSAGetLastError());
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
#else
    if (bind(socket_, (struct sockaddr *)&serverAddr_, sizeof(serverAddr_)) < 0) {
        NETWORK_LOGE("bind error: %s", strerror(errno));
        close(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
#endif

    taskQueue_ = std::make_unique<TaskQueue>("UdpServerCb");
    return true;
}

bool UdpServer::Start()
{
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket not initialized");
        return false;
    }

    taskQueue_->Start();

    serverHandler_ = std::make_shared<UdpServerHandler>(shared_from_this());
    if (!EventReactor::GetInstance()->RegisterHandler(serverHandler_)) {
        NETWORK_LOGE("Failed to register UDP server handler");
        return false;
    }

#ifdef _WIN32
    // Start async receive for Windows IOCP
    StartAsyncReceive();
#endif

    NETWORK_LOGD("UdpServer started with new EventHandler interface");
    return true;
}

bool UdpServer::Stop()
{
    if (socket_ != INVALID_SOCKET && serverHandler_) {
        EventReactor::GetInstance()->RemoveHandler(socket_);
        close(socket_);
        socket_ = INVALID_SOCKET;
        serverHandler_.reset();
    }

    if (taskQueue_) {
        taskQueue_->Stop();
        taskQueue_.reset();
    }

    NETWORK_LOGD("UdpServer stopped");
    return true;
}

bool UdpServer::Send(socket_t fd, std::string host, uint16_t port, const void *data, size_t size)
{
    if (socket_ == INVALID_SOCKET) {
        NETWORK_LOGE("socket not initialized");
        return false;
    }

    struct sockaddr_in remoteAddr;
    memset(&remoteAddr, 0, sizeof(remoteAddr));
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_port = htons(port);
#ifdef _WIN32
    if (inet_pton(AF_INET, host.c_str(), &remoteAddr.sin_addr) != 1) {
        NETWORK_LOGE("inet_pton remote host error");
        return false;
    }
    int nbytes =
        sendto(socket_, (const char *)data, (int)size, 0, (struct sockaddr *)&remoteAddr, (int)sizeof(remoteAddr));
    if (nbytes != (int)size) {
        NETWORK_LOGE("send error: %d %d/%zu", WSAGetLastError(), nbytes, size);
        return false;
    }
#else
    inet_aton(host.c_str(), &remoteAddr.sin_addr);
    ssize_t nbytes = sendto(socket_, data, size, 0, (struct sockaddr *)&remoteAddr, sizeof(remoteAddr));
    if (nbytes != (ssize_t)size) {
        NETWORK_LOGE("send error: %s %zd/%zu", strerror(errno), nbytes, size);
        return false;
    }
#endif
    return true;
}

bool UdpServer::Send(socket_t fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer)
{
    if (!buffer)
        return false;
    return Send(fd, host, port, buffer->Data(), buffer->Size());
}

bool UdpServer::Send(socket_t fd, std::string host, uint16_t port, const std::string &str)
{
    return Send(fd, host, port, str.data(), str.size());
}

void UdpServer::HandleReceive(socket_t fd)
{
    if (fd != socket_) {
        NETWORK_LOGE("fd (" SOCKET_FMT ") mismatch socket(" SOCKET_FMT ")", fd, socket_);
        return;
    }

#ifdef _WIN32
    // Windows: data is already received via IOCP into readBuffer_
    std::string host = inet_ntoa(clientAddr_.sin_addr);
    uint16_t port = ntohs(clientAddr_.sin_port);

    // Get the bytes received from IOCP completion
    DWORD bytesReceived = 0;
    DWORD flags = 0;
    if (WSAGetOverlappedResult(socket_, &recvOverlapped_, &bytesReceived, FALSE, &flags)) {
        NETWORK_LOGD("recvfrom %s:%d, size: %d", host.c_str(), port, (int)bytesReceived);

        if (bytesReceived > 0) {
            if (!listener_.expired()) {
                auto dataBuffer = DataBuffer::PoolAlloc(bytesReceived);
                dataBuffer->Assign(readBuffer_->Data(), bytesReceived);
                auto listenerWeak = listener_;
                auto self = shared_from_this();
                auto task = std::make_shared<TaskHandler<void>>([listenerWeak, self, fd, host, port, dataBuffer]() {
                    auto listener = listenerWeak.lock();
                    if (listener) {
                        auto session = std::make_shared<SessionImpl>(fd, host, port, self);
                        listener->OnReceive(session, dataBuffer);
                    }
                });
                taskQueue_->EnqueueTask(task);
            }
        }
    }

    // Start next async receive
    StartAsyncReceive();
#else
    // Linux: synchronous receive
    if (readBuffer_ == nullptr) {
        readBuffer_ = DataBuffer::PoolAlloc(RECV_BUFFER_MAX_SIZE);
    }

    readBuffer_->Clear();
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);

    while (true) {
        ssize_t nbytes = recvfrom(socket_, readBuffer_->Data(), readBuffer_->Capacity(), 0,
                                  (struct sockaddr *)&clientAddr, &addrLen);
        std::string host = inet_ntoa(clientAddr.sin_addr);
        uint16_t port = ntohs(clientAddr.sin_port);
        NETWORK_LOGD("recvfrom %s:%d, size: %d", host.c_str(), port, (int)nbytes);

        if (nbytes > 0) {
            if (!listener_.expired()) {
                auto dataBuffer = DataBuffer::PoolAlloc(nbytes);
                dataBuffer->Assign(readBuffer_->Data(), nbytes);
                auto listenerWeak = listener_;
                auto self = shared_from_this();
                auto task = std::make_shared<TaskHandler<void>>([listenerWeak, self, fd, host, port, dataBuffer]() {
                    auto listener = listenerWeak.lock();
                    if (listener) {
                        auto session = std::make_shared<SessionImpl>(fd, host, port, self);
                        listener->OnReceive(session, dataBuffer);
                    }
                });
                taskQueue_->EnqueueTask(task);
            }
            continue;
        }

        if (nbytes == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        std::string info = strerror(errno);
        if (!listener_.expired()) {
            auto listenerWeak = listener_;
            auto self = shared_from_this();
            auto task = std::make_shared<TaskHandler<void>>([listenerWeak, self, fd, host, port, info]() {
                auto listener = listenerWeak.lock();
                if (listener) {
                    auto session = std::make_shared<SessionImpl>(fd, host, port, self);
                    listener->OnError(session, info);
                } else {
                    NETWORK_LOGE("not found listener!");
                }
            });
            taskQueue_->EnqueueTask(task);
        }
        break;
    }
#endif
}

uint16_t UdpServer::GetIdlePort()
{
    socket_t sock;
    struct sockaddr_in addr;

    uint16_t i;
    for (i = gIdlePort; i < gIdlePort + 100; i++) {
#ifdef _WIN32
        sock = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (sock == INVALID_SOCKET) {
            NETWORK_LOGE("WSASocket creation failed: %d", WSAGetLastError());
            return (uint16_t)-1;
        }
#else
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            perror("socket creation failed");
            return (uint16_t)-1;
        }
#endif

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(i);

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock);
            continue;
        }
        close(sock);
        break;
    }

    if (i == gIdlePort + 100) {
        NETWORK_LOGE("Can't find idle port");
        return 0;
    }

    gIdlePort = i + 1;

    return i;
}

uint16_t UdpServer::GetIdlePortPair()
{
    uint16_t firstPort;
    uint16_t secondPort;
    firstPort = GetIdlePort();

    while (true) {
        secondPort = GetIdlePort();
        if (firstPort + 1 == secondPort || secondPort == 0) {
            return firstPort;
        } else {
            firstPort = secondPort;
        }
    }
}

#ifdef _WIN32
void UdpServer::StartAsyncReceive()
{
    if (readBuffer_ == nullptr) {
        readBuffer_ = DataBuffer::PoolAlloc(RECV_BUFFER_MAX_SIZE);
    }

    // Prepare for async receive
    memset(&recvOverlapped_, 0, sizeof(recvOverlapped_));
    recvBuffer_.buf = (char *)readBuffer_->Data();
    recvBuffer_.len = (ULONG)readBuffer_->Capacity();
    clientAddrLen_ = sizeof(clientAddr_);

    DWORD flags = 0;
    DWORD bytesReceived = 0;

    int result = WSARecvFrom(socket_, &recvBuffer_, 1, &bytesReceived, &flags, (struct sockaddr *)&clientAddr_,
                             &clientAddrLen_, &recvOverlapped_, NULL);

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