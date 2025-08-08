/**
 * @file common.h
 * @brief Common Header
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETWORK_COMMON_H
#define NETWORK_COMMON_H

namespace lmshao::network {

#ifdef _WIN32

#include <windows.h>

typedef DWORD      pid_t;
typedef SOCKET     socket_t;
typedef int        socklen_t;
typedef int        ssize_t;

constexpr int MSG_DONTWAIT = 0;

inline int close(SOCKET s) {
    return closesocket(s);
}

#else // !_WIN32

typedef int socket_t;

constexpr int INVALID_SOCKET = -1;

#endif // _WIN32

} // namespace lmshao::network

#endif // NETWORK_COMMON_H