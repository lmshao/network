//
// Copyright © 2024 SHAO Liming <lmshao@163.com>. All rights reserved.
//

#ifndef NETWORK_TCP_SERVER_H
#define NETWORK_TCP_SERVER_H

#include <netinet/in.h>

#include <cstdint>
#include <memory>
#include <string>

#include "base_server.h"
#include "iserver_listener.h"
#include "session.h"
#include "task_queue.h"

class TcpServer final : public BaseServer, public std::enable_shared_from_this<TcpServer> {
    friend class EventProcessor;
    const int INVALID_SOCKET = -1;

public:
    template <typename... Args>
    static std::shared_ptr<TcpServer> Create(Args... args)
    {
        return std::shared_ptr<TcpServer>(new TcpServer(args...));
    }

    ~TcpServer();

    bool Init() override;
    void SetListener(std::shared_ptr<IServerListener> listener) override { listener_ = listener; }
    bool Start() override;
    bool Stop() override;
    bool Send(int fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer) override;

    int GetSocketFd() const { return socket_; }

protected:
    TcpServer(std::string listenIp, uint16_t listenPort) : localPort_(listenPort), localIp_(listenIp) {}
    explicit TcpServer(uint16_t listenPort) : localPort_(listenPort) {}

    void HandleAccept(int fd);
    void HandleReceive(int fd);

private:
    uint16_t localPort_;
    int socket_ = INVALID_SOCKET;
    std::string localIp_ = "0.0.0.0";
    struct sockaddr_in serverAddr_;

    std::weak_ptr<IServerListener> listener_;
    std::unordered_map<int, std::shared_ptr<Session>> sessions_;
    std::unique_ptr<TaskQueue> taskQueue_;
    std::unique_ptr<DataBuffer> readBuffer_;
};

#endif // NETWORK_TCP_SERVER_H