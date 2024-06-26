//
// Copyright © 2024 SHAO Liming <lmshao@163.com>. All rights reserved.
//

#ifndef NETWORK_TCP_CLIENT_H
#define NETWORK_TCP_CLIENT_H

#include <cstdint>
#include <netinet/in.h>
#include <string>
#include "data_buffer.h"
#include "iclient_listener.h"
#include "thread_pool.h"

class TcpClient final {
    friend class EventProcessor;
    const int INVALID_SOCKET = -1;

public:
    template <typename... Args>
    static std::shared_ptr<TcpClient> Create(Args... args)
    {
        return std::shared_ptr<TcpClient>(new TcpClient(args...));
    }

    TcpClient(std::string remoteIp, uint16_t remotePort, std::string localIp = "", uint16_t localPort = 0);
    ~TcpClient();

    bool Init();
    void SetListener(std::shared_ptr<IClientListener> listener) { listener_ = listener; }
    bool Connect();

    bool Send(const std::string &str);
    bool Send(const char *data, size_t len);
    bool Send(std::shared_ptr<DataBuffer> data);

    void Close();

protected:
    void HandleReceive(int fd);

private:
    std::string remoteIp_;
    uint16_t remotePort_;

    std::string localIp_;
    uint16_t localPort_;

    int socket_ = INVALID_SOCKET;
    struct sockaddr_in serverAddr_ {};

    std::weak_ptr<IClientListener> listener_;
    std::unique_ptr<ThreadPool> callbackThreads_;
};

#endif // NETWORK_TCP_CLIENT_H