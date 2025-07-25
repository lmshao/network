//
// Copyright © 2023-2025 SHAO Liming <lmshao@163.com>. All rights reserved.
//

#include "data_buffer.h"

#include <arpa/inet.h>

#include <cstdio>
#include <cstring>

constexpr size_t DATA_ALIGN = 8;
inline static size_t align(size_t len)
{
    return (len + DATA_ALIGN - 1) / DATA_ALIGN * DATA_ALIGN;
}

DataBuffer::DataBuffer(size_t len)
{
    if (len) {
        capacity_ = align(len);
        data_ = new uint8_t[capacity_ + 1];
        size_ = 0;
        data_[0] = 0;
    }
}

DataBuffer::DataBuffer(const DataBuffer &other) noexcept
{
    if (other.data_ && other.size_) {
        capacity_ = align(other.size_);
        data_ = new uint8_t[capacity_ + 1];
        memcpy(data_, other.data_, other.size_);
        size_ = other.size_;
        data_[size_] = 0;
    }
}

DataBuffer &DataBuffer::operator=(const DataBuffer &other) noexcept
{
    if (this != &other) {
        delete[] data_; // 先释放当前内存

        if (other.data_ && other.size_) {
            capacity_ = align(other.size_);
            data_ = new uint8_t[capacity_ + 1];
            memcpy(data_, other.data_, other.size_);
            size_ = other.size_;
            data_[size_] = 0;
        } else {
            data_ = nullptr;
            size_ = 0;
            capacity_ = 0;
        }
    }
    return *this;
}

DataBuffer::DataBuffer(DataBuffer &&other) noexcept
{
    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
}

DataBuffer &DataBuffer::operator=(DataBuffer &&other) noexcept
{
    if (this != &other) {
        delete[] data_;
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    return *this;
}

DataBuffer::~DataBuffer()
{
    delete[] data_;
    data_ = nullptr;
    capacity_ = 0;
    size_ = 0;
}

std::shared_ptr<DataBuffer> DataBuffer::Create(size_t len)
{
    return std::make_shared<DataBuffer>(len);
}

void DataBuffer::Assign(const void *p, size_t len)
{
    if (!p || !len) {
        return;
    }

    if (len > capacity_) {
        delete[] data_;
        capacity_ = align(len);
        data_ = new uint8_t[capacity_ + 1];
    }

    memcpy(data_, p, len);
    size_ = len;
    data_[size_] = 0;
}

void DataBuffer::Append(const void *p, size_t len)
{
    if (!p || !len) {
        return;
    }

    if (len + size_ <= capacity_) {
        memcpy(data_ + size_, p, len);
        size_ += len;
    } else {
        capacity_ = align(size_ + len);
        auto newBuffer = new uint8_t[capacity_ + 1];
        if (data_) {
            memcpy(newBuffer, data_, size_);
        }

        memcpy(newBuffer + size_, p, len);
        delete[] data_;
        data_ = newBuffer;
        size_ += len;
    }
    data_[size_] = 0;
}

void DataBuffer::Assign(uint16_t u16)
{
    uint16_t t = htons(u16);
    Assign((uint8_t *)&t, 2);
}

void DataBuffer::Assign(uint32_t u32)
{
    uint32_t t = htonl(u32);
    Assign((uint8_t *)&t, 4);
}

void DataBuffer::Append(uint16_t u16)
{
    uint16_t t = htons(u16);
    Append((uint8_t *)&t, 2);
}

void DataBuffer::Append(uint32_t u32)
{
    uint32_t t = htonl(u32);
    Append((uint8_t *)&t, 4);
}

void DataBuffer::SetSize(size_t len)
{
    if (len <= capacity_) {
        size_ = len;
        if (data_) {
            data_[size_] = 0;
        }
        return;
    }

    capacity_ = align(len);
    auto new_buffer = new uint8_t[capacity_ + 1];
    if (data_) {
        memcpy(new_buffer, data_, size_);
    }

    delete[] data_;
    data_ = new_buffer;
    size_ = len;
    data_[size_] = 0;
}

void DataBuffer::SetCapacity(size_t len)
{
    if (align(len) == capacity_) {
        return;
    }

    if (size_ == 0) {
        delete[] data_;
        capacity_ = align(len);
        data_ = new uint8_t[capacity_ + 1];
        size_ = 0;
    } else {
        capacity_ = align(len);
        if (size_ <= capacity_) {
            auto newBuffer = new uint8_t[capacity_ + 1];
            if (data_) {
                memcpy(newBuffer, data_, size_);
            }
            delete[] data_;
            data_ = newBuffer;
        } else {
            auto newBuffer = new uint8_t[capacity_ + 1];
            if (data_) {
                memcpy(newBuffer, data_, capacity_);
            }
            delete[] data_;
            data_ = newBuffer;
            size_ = capacity_;
        }
    }
}

void DataBuffer::Clear()
{
    size_ = 0;
    if (data_) {
        data_[0] = 0;
    }
}

void DataBuffer::HexDump(size_t len)
{
    if (len == 0 || len > size_) {
        len = size_;
    }

    size_t i;
    for (i = 0; i < len; ++i) {
        printf("%02x ", *(data_ + i));
        if (i % 16 == 15) {
            printf("\n");
        }
    }

    if (i % 16 != 0) {
        printf("\n");
    }
}

std::string DataBuffer::ToString()
{
    if (!data_ || size_ == 0) {
        return {};
    }
    return std::string((char *)data_, size_);
}

bool DataBuffer::operator==(const DataBuffer &other) const
{
    if (size_ != other.size_) {
        return false;
    }

    if (size_ == 0) {
        return true;
    }

    return memcmp(data_, other.data_, size_) == 0;
}