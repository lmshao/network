/**
 * @file unix_client.h
 * @brief Unix Domain Socket Client Header
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETWORK_UNIX_CLIENT_H
#define NETWORK_UNIX_CLIENT_H

#ifndef _WIN32

#include <sys/un.h>

#include <memory>
#include <string>

#include "data_buffer.h"
#include "iclient_listener.h"
#include "task_queue.h"
#include "common.h"
namespace lmshao::network {
class EventHandler;
class UnixClientHandler;

class UnixClient : public std::enable_shared_from_this<UnixClient> {
    friend class UnixClientHandler;

public:
    static std::shared_ptr<UnixClient> Create(const std::string &socketPath)
    {
        return std::shared_ptr<UnixClient>(new UnixClient(socketPath));
    }

    ~UnixClient();

    bool Init();
    void SetListener(std::shared_ptr<IClientListener> listener) { listener_ = listener; }
    bool Connect();
    bool Send(const std::string &str);
    bool Send(const void *data, size_t len);
    bool Send(std::shared_ptr<DataBuffer> data);
    void Close();
    socket_t GetSocketFd() const { return socket_; }

protected:
    explicit UnixClient(const std::string &socketPath);
    void HandleReceive(socket_t fd);
    void HandleConnectionClose(socket_t fd, bool isError, const std::string &reason);

private:
    std::string socketPath_;
    socket_t socket_ = INVALID_SOCKET;
    struct sockaddr_un serverAddr_;

    std::weak_ptr<IClientListener> listener_;
    std::unique_ptr<TaskQueue> taskQueue_;
    std::shared_ptr<DataBuffer> readBuffer_;

    std::shared_ptr<UnixClientHandler> clientHandler_;
};

} // namespace lmshao::network

#endif // _WIN32
#endif // NETWORK_UNIX_CLIENT_H
