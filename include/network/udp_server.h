/**
 * @file udp_server.h
 * @brief UDP Server Header
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETWORK_UDP_SERVER_H
#define NETWORK_UDP_SERVER_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

#include <cstdint>
#include <memory>
#include <string>

#include "base_server.h"
#include "common.h"
#include "iserver_listener.h"
#include "session.h"
#include "task_queue.h"

namespace lmshao::network {
class EventHandler;

class UdpServer final : public BaseServer, public std::enable_shared_from_this<UdpServer> {
    friend class UdpServerHandler;

public:
    template <typename... Args>
    static std::shared_ptr<UdpServer> Create(Args &&...args)
    {
        return std::shared_ptr<UdpServer>(new UdpServer(std::forward<Args>(args)...));
    }

    ~UdpServer();

    bool Init() override;
    void SetListener(std::shared_ptr<IServerListener> listener) override { listener_ = listener; }
    bool Start() override;
    bool Stop() override;
    bool Send(socket_t fd, std::string host, uint16_t port, const void *data, size_t size) override;
    bool Send(socket_t fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer) override;
    bool Send(socket_t fd, std::string host, uint16_t port, const std::string &str) override;

    socket_t GetSocketFd() const { return socket_; }

    static uint16_t GetIdlePort();
    static uint16_t GetIdlePortPair();

private:
    UdpServer(std::string listenIp, uint16_t listenPort) : localIp_(listenIp), localPort_(listenPort) {}
    explicit UdpServer(uint16_t listenPort) : localPort_(listenPort) {}

    void HandleReceive(socket_t fd);

private:
    std::string localIp_ = "0.0.0.0";
    uint16_t localPort_;

    socket_t socket_ = INVALID_SOCKET;
    struct sockaddr_in serverAddr_;

    std::weak_ptr<IServerListener> listener_;
    std::unique_ptr<TaskQueue> taskQueue_;
    std::shared_ptr<DataBuffer> readBuffer_;

    std::shared_ptr<EventHandler> serverHandler_;

#ifdef _WIN32
    // Windows IOCP async receive support
    void StartAsyncReceive();
    OVERLAPPED recvOverlapped_;
    WSABUF recvBuffer_;
    struct sockaddr_in clientAddr_;
    int clientAddrLen_;
#endif
};

} // namespace lmshao::network

#endif // NETWORK_UDP_SERVER_H