#include "file_service.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

void FileService::registerOps(ControlServer& srv) {
    srv.on("open", [this](ControlServer::Ctx& c) { doOpen(c); });
    srv.on("read", [this](ControlServer::Ctx& c) { doRead(c); }, false);   // idempotent; not cached
    srv.on("close", [this](ControlServer::Ctx& c) { doClose(c); });
}

FileService::Handle* FileService::find(uint32_t id) {
    for (auto& h : handles_)
        if (h.id == id && id != 0) return &h;
    return nullptr;
}

void FileService::release(Handle& h) {
    if (h.fd >= 0) ::close(h.fd);
    h = Handle{};
}

void FileService::doOpen(ControlServer::Ctx& c) {
    std::lock_guard<std::mutex> l(mu_);
    const std::string kind = ControlServer::str(c.req, "kind", "");
    Handle tmp;
    std::string sha;

    if (kind == "bench") {
        const double size = ControlServer::num(c.req, "size", 0);
        if (size <= 0 || size > 64.0 * 1024 * 1024) return ControlServer::reject(c, "INVALID", "size");
        tmp.bench = true;
        tmp.size = static_cast<uint64_t>(size);
    } else {
        const char* rid = ControlServer::str(c.req, "recording_id");
        std::string path;
        if (!rid || !resolver_ || !resolver_(rid, kind, path, sha))
            return ControlServer::reject(c, "REJECTED", "NOT_FOUND");
        tmp.fd = ::open(path.c_str(), O_RDONLY);
        if (tmp.fd < 0) return ControlServer::reject(c, "ERROR", "OPEN_FAILED");
        struct stat st;
        fstat(tmp.fd, &st);
        tmp.size = static_cast<uint64_t>(st.st_size);
    }

    // Take a free slot, else evict the least recently used handle (an HTTP
    // client that vanished mid-transfer must not lock the service forever).
    Handle* slot = nullptr;
    for (auto& h : handles_)
        if (h.id == 0) { slot = &h; break; }
    if (!slot) {
        slot = std::min_element(std::begin(handles_), std::end(handles_),
                                [](const Handle& a, const Handle& b) { return a.last_use < b.last_use; });
        release(*slot);
    }
    *slot = tmp;
    slot->id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    slot->last_use = ++tick_;

    cJSON_AddNumberToObject(c.rsp, "handle", slot->id);
    cJSON_AddNumberToObject(c.rsp, "size", static_cast<double>(slot->size));
    cJSON_AddStringToObject(c.rsp, "sha256", sha.c_str());
}

void FileService::doRead(ControlServer::Ctx& c) {
    std::lock_guard<std::mutex> l(mu_);
    Handle* h = find(static_cast<uint32_t>(ControlServer::num(c.req, "handle", 0)));
    if (!h) return ControlServer::reject(c, "INVALID", "BAD_HANDLE");
    const uint64_t off = static_cast<uint64_t>(ControlServer::num(c.req, "offset", 0));
    uint32_t len = static_cast<uint32_t>(std::min<double>(ControlServer::num(c.req, "len", kMaxRead), kMaxRead));
    h->last_use = ++tick_;

    std::vector<uint8_t> buf(len);
    size_t got = 0;
    if (off < h->size) {
        len = static_cast<uint32_t>(std::min<uint64_t>(len, h->size - off));
        if (h->bench) {
            for (uint32_t i = 0; i < len; i++) buf[i] = static_cast<uint8_t>((off + i) * 31u + 7u);
            got = len;
        } else {
            ssize_t n = ::pread(h->fd, buf.data(), len, static_cast<off_t>(off));
            if (n < 0) return ControlServer::reject(c, "ERROR", "READ_FAILED");
            got = static_cast<size_t>(n);
        }
    }
    link_.sendChunk(c.id, static_cast<uint32_t>(off), buf.data(), got);   // got == 0 -> EOF
    c.no_reply = true;
}

void FileService::doClose(ControlServer::Ctx& c) {
    std::lock_guard<std::mutex> l(mu_);
    Handle* h = find(static_cast<uint32_t>(ControlServer::num(c.req, "handle", 0)));
    if (h) release(*h);
}
