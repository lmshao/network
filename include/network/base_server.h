//
// Copyright © 2024 SHAO Liming <lmshao@163.com>. All rights reserved.
//

#ifndef NETWORK_BASE_SERVER_H
#define NETWORK_BASE_SERVER_H

#include <memory>

#include "data_buffer.h"

class IServerListener;
class BaseServer {
public:
    virtual ~BaseServer() = default;
    virtual bool Init() = 0;
    virtual void SetListener(std::shared_ptr<IServerListener> listener) = 0;
    virtual bool Start() = 0;
    virtual bool Stop() = 0;
    virtual bool Send(int fd, std::string host, uint16_t port, std::shared_ptr<DataBuffer> buffer) = 0;
    virtual bool Send(int fd, std::string host, uint16_t port, const std::string &str) = 0;
};

#endif // NETWORK_BASE_SERVER_H