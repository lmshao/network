/**
 * @file udp_server.cpp
 * @brief UDP Server Public Interface Implementation
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#include "udp_server.h"

#include "iudp_server.h"
#include "log.h"

#ifdef __linux__
#include "platforms/linux/port_utils.h"
#include "platforms/linux/udp_server_impl.h"
#endif

namespace lmshao::network {

UdpServer::UdpServer(std::string listenIp, uint16_t listenPort)
{
#ifdef __linux__
    impl_ = UdpServerImpl::Create(std::move(listenIp), listenPort);
#endif
    if (!impl_) {
        NETWORK_LOGE("Failed to create UDP server implementation");
    }
}

UdpServer::UdpServer(uint16_t listenPort) : UdpServer("0.0.0.0", listenPort) {}

UdpServer::~UdpServer() = default;

bool UdpServer::Init()
{
    if (!impl_) {
        NETWORK_LOGE("UDP server implementation is not initialized");
        return false;
    }
    return impl_->Init();
}

bool UdpServer::Start()
{
    if (!impl_) {
        NETWORK_LOGE("UDP server implementation is not initialized");
        return false;
    }
    return impl_->Start();
}

bool UdpServer::Stop()
{
    if (!impl_) {
        NETWORK_LOGE("UDP server implementation is not initialized");
        return false;
    }
    return impl_->Stop();
}

void UdpServer::SetListener(std::shared_ptr<IServerListener> listener)
{
    if (!impl_) {
        NETWORK_LOGE("UDP server implementation is not initialized");
        return;
    }
    impl_->SetListener(listener);
}

bool UdpServer::Send(int fd, std::string host, uint16_t port, const void *data, size_t size)
{
    if (!impl_) {
        NETWORK_LOGE("UDP server implementation is not initialized");
        return false;
    }
    return impl_->Send(fd, std::move(host), port, data, size);
}

bool UdpServer::Send(int fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer)
{
    if (!impl_) {
        NETWORK_LOGE("UDP server implementation is not initialized");
        return false;
    }
    return impl_->Send(fd, std::move(host), port, buffer);
}

bool UdpServer::Send(int fd, std::string host, uint16_t port, const std::string &str)
{
    if (!impl_) {
        NETWORK_LOGE("UDP server implementation is not initialized");
        return false;
    }
    return impl_->Send(fd, std::move(host), port, str);
}

socket_t UdpServer::GetSocketFd() const
{
    if (!impl_) {
        NETWORK_LOGE("UDP server implementation is not initialized");
        return INVALID_SOCKET;
    }
    return impl_->GetSocketFd();
}

uint16_t UdpServer::GetIdlePort()
{
#ifdef __linux__
    return PortUtils::GetIdleUdpPort();
#else
    // For non-Linux platforms, implement platform-specific port discovery
    return 0;
#endif
}

uint16_t UdpServer::GetIdlePortPair()
{
#ifdef __linux__
    return PortUtils::GetIdleUdpPortPair();
#else
    // For non-Linux platforms, implement platform-specific port pair discovery
    return 0;
#endif
}

} // namespace lmshao::network
