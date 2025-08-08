/**
 * @file tcp_server.h
 * @brief TCP Server Header
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETWORK_TCP_SERVER_H
#define NETWORK_TCP_SERVER_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
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
class TcpConnectionHandler;
class TcpServerHandler;

class TcpServer final : public BaseServer, public std::enable_shared_from_this<TcpServer> {
    friend class EventProcessor;
    friend class TcpServerHandler;
    friend class TcpConnectionHandler;

public:
    template <typename... Args>
    static std::shared_ptr<TcpServer> Create(Args &&...args)
    {
        return std::shared_ptr<TcpServer>(new TcpServer(std::forward<Args>(args)...));
    }

    ~TcpServer();

    bool Init() override;
    void SetListener(std::shared_ptr<IServerListener> listener) override { listener_ = listener; }
    bool Start() override;
    bool Stop() override;
    bool Send(socket_t fd, std::string host, uint16_t port, const void *data, size_t size) override;
    bool Send(socket_t fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer) override;
    bool Send(socket_t fd, std::string host, uint16_t port, const std::string &str) override;

    socket_t GetSocketFd() const { return socket_; }

protected:
    TcpServer(std::string listenIp, uint16_t listenPort) : localPort_(listenPort), localIp_(listenIp) {}
    explicit TcpServer(uint16_t listenPort) : localPort_(listenPort) {}

    void HandleAccept(socket_t fd);
    void HandleReceive(socket_t fd);
    void HandleConnectionClose(socket_t fd, bool isError, const std::string &reason);
    void EnableKeepAlive(socket_t fd);

private:
    uint16_t localPort_;

    socket_t socket_ = INVALID_SOCKET;

    std::string localIp_ = "0.0.0.0";
    struct sockaddr_in serverAddr_;

    std::weak_ptr<IServerListener> listener_;
    std::unordered_map<socket_t, std::shared_ptr<Session>> sessions_;
    std::unique_ptr<TaskQueue> taskQueue_;
    std::unique_ptr<DataBuffer> readBuffer_;

    std::shared_ptr<EventHandler> serverHandler_;
    std::unordered_map<socket_t, std::shared_ptr<TcpConnectionHandler>> connectionHandlers_;
};

} // namespace lmshao::network

#endif // NETWORK_TCP_SERVER_H