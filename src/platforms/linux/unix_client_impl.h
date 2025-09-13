/**
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef LMSHAO_NETWORK_LINUX_UNIX_CLIENT_IMPL_H
#define LMSHAO_NETWORK_LINUX_UNIX_CLIENT_IMPL_H

// Unix domain sockets are only supported on Unix-like systems (Linux, macOS, BSD)
#if !defined(__unix__) && !defined(__unix) && !defined(unix) && !defined(__APPLE__)
#error "Unix domain sockets are not supported on this platform"
#endif

#include <lmcore/data_buffer.h>
#include <lmcore/task_queue.h>
#include <sys/un.h>

#include <cstdint>
#include <memory>
#include <string>

#include "../../iunix_client.h"
#include "lmnet/common.h"
#include "lmnet/iclient_listener.h"

namespace lmshao::lmnet {
using namespace lmshao::lmcore;
class UnixClientHandler;
class EventHandler;
class UnixClientImpl final : public IUnixClient,
                             public std::enable_shared_from_this<UnixClientImpl>,
                             public Creatable<UnixClientImpl> {
    friend class UnixClientHandler;
    friend class Creatable<UnixClientImpl>;

public:
    ~UnixClientImpl();

    bool Init() override;
    void SetListener(std::shared_ptr<IClientListener> listener) override { listener_ = listener; }
    bool Connect() override;

    bool Send(const std::string &str) override;
    bool Send(const void *data, size_t len) override;
    bool Send(std::shared_ptr<DataBuffer> data) override;

    void Close() override;

    socket_t GetSocketFd() const override { return socket_; }

protected:
    UnixClientImpl(const std::string &socketPath);

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

} // namespace lmshao::lmnet

#endif // LMSHAO_NETWORK_LINUX_UNIX_CLIENT_IMPL_H
