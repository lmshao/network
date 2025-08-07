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

// typedefs for cross-platform compatibility
typedef DWORD pid_t;
typedef SOCKET socket_t;
typedef int socklen_t;

#define cast_socket_t(s) (s)

#else

typedef int socket_t;

constexpr int INVALID_SOCKET = -1;
#define cast_socket_t(s) static_cast<unsigned long long>(s)

#endif

} // namespace lmshao::network
#endif // NETWORK_COMMON_H