/**
 * @file unix_server.h
 * @brief Unix Domain Socket Server Header
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETWORK_UNIX_SERVER_H
#define NETWORK_UNIX_SERVER_H

#ifndef _WIN32
#include <sys/un.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "iserver_listener.h"
#include "session.h"
#include "task_queue.h"
#include "common.h"

namespace lmshao::network {
class EventHandler;
class UnixConnectionHandler;

class UnixServer : public BaseServer, public std::enable_shared_from_this<UnixServer> {

    friend class UnixServerHandler;
    friend class UnixConnectionHandler;

public:
    static std::shared_ptr<UnixServer> Create(const std::string &socketPath)
    {
        return std::shared_ptr<UnixServer>(new UnixServer(socketPath));
    }

    ~UnixServer();

    bool Init() override;
    void SetListener(std::shared_ptr<IServerListener> listener) override { listener_ = listener; }
    bool Start() override;
    bool Stop() override;
    bool Send(socket_t fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer) override;
    bool Send(socket_t fd, std::string host, uint16_t port, const std::string &str) override;
    bool Send(socket_t fd, std::string host, uint16_t port, const void *data, size_t size) override;
    void Close();
    socket_t GetSocketFd() const { return socket_; }

protected:
    explicit UnixServer(const std::string &socketPath);
    void HandleAccept(socket_t fd);
    void HandleReceive(socket_t fd);
    void HandleConnectionClose(socket_t fd, bool isError, const std::string &reason);

private:
    std::string socketPath_;
    socket_t socket_ = INVALID_SOCKET;
    struct sockaddr_un serverAddr_;

    std::weak_ptr<IServerListener> listener_;
    std::unordered_map<int, std::shared_ptr<Session>> sessions_;
    std::unique_ptr<TaskQueue> taskQueue_;
    std::shared_ptr<DataBuffer> readBuffer_;

    std::shared_ptr<EventHandler> serverHandler_;
    std::unordered_map<int, std::shared_ptr<UnixConnectionHandler>> connectionHandlers_;
};

} // namespace lmshao::network

#endif // _WIN32
#endif // NETWORK_UNIX_SERVER_H
