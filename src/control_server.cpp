#include "control_server.hpp"

#include <cstring>
#include <iostream>

static constexpr size_t kQueueMax = 32;
static constexpr size_t kCacheMax = 16;

ControlServer::ControlServer(PcgStream& link) : link_(link) {
    link_.setMessageHandler([this](uint8_t type, const uint8_t* data, size_t len) {
        if (type == PcgStream::kReq) enqueue(data, len);
    });
}

ControlServer::~ControlServer() { stop(); }

void ControlServer::on(const std::string& op, Handler h, bool cache) {
    handlers_[op] = Entry{std::move(h), cache};
}

void ControlServer::start() {
    running_ = true;
    thread_ = std::thread(&ControlServer::worker, this);
}

void ControlServer::stop() {
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ControlServer::enqueue(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> l(mu_);
    if (queue_.size() >= kQueueMax) {
        dropped_++;     // the ESP retries after its timeout
        return;
    }
    queue_.emplace_back(reinterpret_cast<const char*>(data), len);
    cv_.notify_one();
}

void ControlServer::reply(uint32_t id, const std::string& txt) {
    (void)id;
    link_.sendMsg(PcgStream::kRsp, txt.data(), txt.size());
}

void ControlServer::sendEvent(const cJSON* evt) {
    char* t = cJSON_PrintUnformatted(evt);
    if (!t) return;
    link_.sendMsg(PcgStream::kEvt, t, std::strlen(t));
    cJSON_free(t);
}

void ControlServer::reject(Ctx& c, const char* st, const char* err) {
    cJSON_ReplaceItemInObject(c.rsp, "st", cJSON_CreateString(st));
    if (err) cJSON_AddStringToObject(c.rsp, "err", err);
}

const char* ControlServer::str(const cJSON* o, const char* key, const char* def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

double ControlServer::num(const cJSON* o, const char* key, double def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

bool ControlServer::flag(const cJSON* o, const char* key, bool def) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

void ControlServer::worker() {
    for (;;) {
        std::string msg;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return !queue_.empty() || !running_; });
            if (!running_) return;
            msg = std::move(queue_.front());
            queue_.pop_front();
        }
        requests_++;

        cJSON* req = cJSON_ParseWithLength(msg.data(), msg.size());
        if (!req) continue;     // undecodable: no id to answer
        const uint32_t id = static_cast<uint32_t>(num(req, "id", 0));
        const char* op = str(req, "op", "");

        // Retried request whose answer we already sent: replay it.
        bool replayed = false;
        for (const auto& e : cache_) {
            if (e.first == id && id != 0) {
                reply(id, e.second);
                replayed = true;
                break;
            }
        }
        if (replayed) {
            cJSON_Delete(req);
            continue;
        }

        cJSON* rsp = cJSON_CreateObject();
        cJSON_AddNumberToObject(rsp, "v", 1);
        cJSON_AddNumberToObject(rsp, "id", id);
        cJSON_AddStringToObject(rsp, "st", "OK");
        Ctx ctx{req, id, rsp};

        auto it = handlers_.find(op);
        bool cache = true;
        if (num(req, "v", 0) != 1) {
            reject(ctx, "UNSUPPORTED", "CTRL_PROTO");
        } else if (it == handlers_.end()) {
            reject(ctx, "UNSUPPORTED", "UNKNOWN_OP");
        } else {
            cache = it->second.cache;
            try {
                it->second.h(ctx);
            } catch (const std::exception& e) {
                std::cerr << "[ctrl] op " << op << " failed: " << e.what() << std::endl;
                reject(ctx, "ERROR", "INTERNAL");
            }
        }

        if (!ctx.no_reply) {
            char* t = cJSON_PrintUnformatted(rsp);
            if (t) {
                std::string txt(t);
                cJSON_free(t);
                if (cache && id != 0) {
                    cache_.emplace_back(id, txt);
                    if (cache_.size() > kCacheMax) cache_.pop_front();
                }
                reply(id, txt);
            }
        }
        cJSON_Delete(rsp);
        cJSON_Delete(req);
    }
}
