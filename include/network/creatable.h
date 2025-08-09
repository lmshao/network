/**
 * @file creatable.h
 * @brief Creatable Base Class Template
 * @author SHAO Liming <lmshao@163.com>
 * @copyright Copyright (c) 2024-2025 SHAO Liming
 * @license MIT
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETWORK_CREATABLE_H
#define NETWORK_CREATABLE_H

#include <memory>

namespace lmshao::network {
template <typename T>
class Creatable {
public:
    template <typename... Args>
    static std::shared_ptr<T> Create(Args &&...args)
    {
        return std::shared_ptr<T>(new T(std::forward<Args>(args)...));
    }

protected:
    friend T;
};
} // namespace lmshao::network
#endif // NETWORK_CREATABLE_H
