#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cJSON.h"
#include "pcg_stream.hpp"

// Control-plane server for JSON requests arriving over the UART link
// (PROTOCOL.md §2). Requests are queued by the link reader thread and handled
// on a worker thread so slow operations never stall audio parsing.
//
// Handlers fill `rsp` (st defaults to "OK"). A handler that answers with a
// CHUNK frame instead of JSON sets ctx.no_reply. Responses for the last 16 ids
// are cached and replayed for retried requests, so each op runs once.
class ControlServer {
public:
    struct Ctx {
        const cJSON* req;
        uint32_t     id;
        cJSON*       rsp;
        bool         no_reply = false;
    };
    using Handler = std::function<void(Ctx&)>;

    explicit ControlServer(PcgStream& link);
    ~ControlServer();

    void on(const std::string& op, Handler h, bool cache = true);
    void start();
    void stop();

    // Emit an EVT frame (JSON object, not consumed).
    void sendEvent(const cJSON* evt);

    // Helpers for handlers
    static void reject(Ctx& c, const char* st, const char* err);
    static const char* str(const cJSON* o, const char* key, const char* def = nullptr);
    static double num(const cJSON* o, const char* key, double def);
    static bool   flag(const cJSON* o, const char* key, bool def);

    uint64_t requests() const { return requests_; }
    uint64_t dropped() const { return dropped_; }

private:
    void enqueue(const uint8_t* data, size_t len);
    void worker();
    void reply(uint32_t id, const std::string& txt);

    struct Entry { Handler h; bool cache; };

    PcgStream& link_;
    std::map<std::string, Entry> handlers_;
    std::deque<std::string> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    bool running_ = false;

    std::deque<std::pair<uint32_t, std::string>> cache_;   // last responses by id
    uint64_t requests_ = 0, dropped_ = 0;
};
