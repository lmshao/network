/**
 * @file common.h
 * @brief Common Header
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETWORK_COMMON_H
#define NETWORK_COMMON_H

namespace lmshao::network {
#ifdef __linux__
inline constexpr int INVALID_SOCKET = -1;
typedef int socket_t;
#endif
} // namespace lmshao::network

#endif // NETWORK_COMMON_H