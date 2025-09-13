/**
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef LMSHAO_NETWORK_LINUX_PORT_UTILS_H
#define LMSHAO_NETWORK_LINUX_PORT_UTILS_H

#include <cstdint>

namespace lmshao::lmnet {

class PortUtils {
public:
    /**
     * @brief Get an idle UDP port
     * @return Port number if found, 0 if failed
     */
    static uint16_t GetIdleUdpPort();

    /**
     * @brief Get a pair of consecutive idle UDP ports
     * @return First port number of the pair if found, 0 if failed
     */
    static uint16_t GetIdleUdpPortPair();

private:
    static constexpr uint16_t UDP_PORT_START = 10000;
    static uint16_t nextPort_;
};

} // namespace lmshao::lmnet

#endif // LMSHAO_NETWORK_LINUX_PORT_UTILS_H
