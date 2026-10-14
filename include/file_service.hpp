#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "control_server.hpp"

// Bounded file-read service for the ESP32 HTTP proxy (PROTOCOL.md §2 open/read/close).
// Up to kMaxHandles files are open at once; reads are pread() of <= 4096 bytes
// answered with a CHUNK frame. kind "bench" serves a virtual pattern file for
// link benchmarks (byte i = i*31+7).
class FileService {
public:
    static constexpr int kMaxHandles = 4;
    static constexpr uint32_t kMaxRead = 4096;

    // Maps (recording_id, kind) -> path + sha256; returns false if unknown.
    using Resolver = std::function<bool(const std::string& rid, const std::string& kind,
                                        std::string& path, std::string& sha256)>;

    explicit FileService(PcgStream& link) : link_(link) {}
    void setResolver(Resolver r) { resolver_ = std::move(r); }
    void registerOps(ControlServer& srv);

private:
    struct Handle {
        uint32_t    id = 0;     // 0 = free
        int         fd = -1;
        uint64_t    size = 0;
        bool        bench = false;
        uint64_t    last_use = 0;
    };

    void doOpen(ControlServer::Ctx& c);
    void doRead(ControlServer::Ctx& c);
    void doClose(ControlServer::Ctx& c);
    Handle* find(uint32_t id);
    void release(Handle& h);

    PcgStream& link_;
    Resolver resolver_;
    Handle handles_[kMaxHandles];
    uint32_t next_id_ = 1;
    uint64_t tick_ = 0;
    std::mutex mu_;
};
