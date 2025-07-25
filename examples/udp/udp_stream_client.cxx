// UDP 打流测试客户端
// Copyright © 2024 SHAO Liming <lmshao@163.com>. All rights reserved.

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <cstring>
#include <iostream>

#include <unordered_set>
#include <mutex>

struct Config {
    std::string server_ip = "127.0.0.1";
    uint16_t server_port = 7777;
    size_t total_bytes = 1024 * 1024; // 1MB 默认
    size_t packet_size = 1024;        // 每包 1KB 默认
    size_t send_rate = 0;             // 0 表示不限速，单位: 字节/秒
};

std::atomic<size_t> sent_packets{0};
std::atomic<size_t> recv_packets{0};
std::atomic<size_t> lost_packets{0};
std::unordered_set<uint32_t> sent_seq;
std::unordered_set<uint32_t> recv_seq;
std::mutex seq_mutex;

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <server_ip> <server_port> <total_bytes> <packet_size> [send_rate_bytes_per_sec]" << std::endl;
}

void sender(int sock, const Config& cfg, sockaddr_in& server_addr) {
    size_t total_packets = cfg.total_bytes / cfg.packet_size;
    char* buffer = new char[cfg.packet_size];
    auto start = std::chrono::steady_clock::now();
    for (uint32_t seq = 0; seq < total_packets; ++seq) {
        // 包头4字节为序号
        memcpy(buffer, &seq, sizeof(seq));
        memset(buffer + sizeof(seq), seq % 256, cfg.packet_size - sizeof(seq));
        ssize_t n = sendto(sock, buffer, cfg.packet_size, 0, (sockaddr*)&server_addr, sizeof(server_addr));
        if (n == (ssize_t)cfg.packet_size) {
            sent_packets++;
            std::lock_guard<std::mutex> lock(seq_mutex);
            sent_seq.insert(seq);
        }
        // 限速
        if (cfg.send_rate > 0) {
            static size_t bytes_sent = 0;
            bytes_sent += cfg.packet_size;
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start).count();
            if (elapsed > 0 && bytes_sent / elapsed > cfg.send_rate) {
                usleep(1000); // 微调
            }
        }
    }
    delete[] buffer;
}

void receiver(int sock, size_t total_packets) {
    char buffer[65536];
    while (recv_packets < total_packets) {
        ssize_t n = recv(sock, buffer, sizeof(buffer), 0);
        if (n >= 4) {
            uint32_t seq = 0;
            memcpy(&seq, buffer, sizeof(seq));
            std::lock_guard<std::mutex> lock(seq_mutex);
            if (sent_seq.count(seq)) {
                recv_seq.insert(seq);
                recv_packets++;
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 5) {
        print_usage(argv[0]);
        return 1;
    }
    Config cfg;
    cfg.server_ip = argv[1];
    cfg.server_port = atoi(argv[2]);
    cfg.total_bytes = atoll(argv[3]);
    cfg.packet_size = atoi(argv[4]);
    if (argc >= 6) cfg.send_rate = atoll(argv[5]);
    size_t total_packets = cfg.total_bytes / cfg.packet_size;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(cfg.server_port);
    inet_aton(cfg.server_ip.c_str(), &server_addr.sin_addr);

    auto t_send = std::thread(sender, sock, std::ref(cfg), std::ref(server_addr));
    auto t_recv = std::thread(receiver, sock, total_packets);
    auto start = std::chrono::steady_clock::now();
    t_send.join();
    t_recv.join();
    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();

    // 统计丢包
    size_t lost = 0;
    {
        std::lock_guard<std::mutex> lock(seq_mutex);
        for (auto seq : sent_seq) {
            if (!recv_seq.count(seq)) lost++;
        }
    }
    std::cout << "发送包数: " << sent_packets << std::endl;
    std::cout << "接收包数: " << recv_packets << std::endl;
    std::cout << "丢包数: " << lost << std::endl;
    std::cout << "丢包率: " << (100.0 * lost / sent_packets) << "%" << std::endl;
    std::cout << "耗时: " << elapsed << " 秒" << std::endl;
    close(sock);
    return 0;
}
