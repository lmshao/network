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

#include <cstdint>
#include <memory>
#include <string>

#include "base_server.h"
#include "common.h"
#include "creatable.h"
#include "data_buffer.h"
#include "iserver_listener.h"

namespace lmshao::network {

class IUdpServer;

class UdpServer : public BaseServer, public Creatable<UdpServer> {
public:
    UdpServer(std::string listenIp, uint16_t listenPort);
    explicit UdpServer(uint16_t listenPort);
    ~UdpServer();

    bool Init() override;
    bool Start() override;
    bool Stop() override;

    void SetListener(std::shared_ptr<IServerListener> listener) override;

    bool Send(int fd, std::string host, uint16_t port, const void *data, size_t size) override;
    bool Send(int fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer) override;
    bool Send(int fd, std::string host, uint16_t port, const std::string &str) override;

    socket_t GetSocketFd() const;

    static uint16_t GetIdlePort();
    static uint16_t GetIdlePortPair();

private:
    std::shared_ptr<IUdpServer> impl_;
};

} // namespace lmshao::network

#endif // NETWORK_UDP_SERVER_H
