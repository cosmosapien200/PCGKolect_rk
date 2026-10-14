#include "recording_store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "sha256.hpp"

// ---------------------------------------------------------------------------
// fsutil
// ---------------------------------------------------------------------------

namespace fsutil {

bool mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

void fsyncDir(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = ::write(fd, data.data() + off, data.size() - off);
        if (w <= 0) {
            ::close(fd);
            return false;
        }
        off += static_cast<size_t>(w);
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) return false;
    fsyncDir(path.substr(0, path.find_last_of('/')));
    return true;
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool removeTree(const std::string& path) {
    DIR* d = ::opendir(path.c_str());
    if (!d) return ::unlink(path.c_str()) == 0 || errno == ENOENT;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        const std::string p = path + "/" + e->d_name;
        struct stat st;
        if (::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) removeTree(p);
        else ::unlink(p.c_str());
    }
    ::closedir(d);
    return ::rmdir(path.c_str()) == 0;
}

}  // namespace fsutil

// ---------------------------------------------------------------------------

static bool isRid(const std::string& s) {
    // opaque id: 8-64 chars of [0-9a-f-]
    if (s.size() < 8 || s.size() > 64) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-'; });
}

bool RecordingStore::diskSpace(const std::string& path, uint64_t& total, uint64_t& free_bytes) {
    struct statvfs v;
    if (::statvfs(path.c_str(), &v) != 0) return false;
    total = static_cast<uint64_t>(v.f_blocks) * v.f_frsize;
    free_bytes = static_cast<uint64_t>(v.f_bavail) * v.f_frsize;
    return true;
}

bool RecordingStore::init() {
    if (!fsutil::mkdirs(root_ + "/.partial")) return false;
    std::lock_guard<std::mutex> l(mu_);
    items_.clear();
    DIR* d = ::opendir(root_.c_str());
    if (!d) return false;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string rid = e->d_name;
        if (!isRid(rid)) continue;
        Item it;
        if (loadItem(rid, it)) items_[rid] = it;
    }
    ::closedir(d);
    return true;
}

std::vector<std::string> RecordingStore::partials() const {
    std::vector<std::string> out;
    DIR* d = ::opendir((root_ + "/.partial").c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr)
        if (isRid(e->d_name)) out.push_back(e->d_name);
    ::closedir(d);
    return out;
}

bool RecordingStore::loadItem(const std::string& rid, Item& it) {
    const std::string dir = finalDir(rid);
    std::string txt;
    if (!fsutil::readFile(dir + "/meta.json", txt)) return false;
    cJSON* m = cJSON_Parse(txt.c_str());
    if (!m) return false;
    auto s = [&](const cJSON* o, const char* k) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
        return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
    };
    auto n = [&](const cJSON* o, const char* k) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
        return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
    };
    it.rid = rid;
    it.dataset_id = s(m, "dataset_id");
    it.subject_id = s(m, "subject_id");
    it.take = static_cast<int>(n(m, "take_index"));
    it.start_utc_ms = static_cast<int64_t>(n(m, "start_utc_ms"));
    it.completion = s(m, "completion_state");
    const cJSON* q = cJSON_GetObjectItemCaseSensitive(m, "quality");
    it.duration_ms = n(q, "captured_duration_ms");
    it.usable_ms = n(q, "usable_duration_ms");
    cJSON_Delete(m);

    if (fsutil::readFile(dir + "/files.json", txt)) {
        cJSON* f = cJSON_Parse(txt.c_str());
        for (int k = 0; k < 4 && f; k++) {
            const cJSON* fi = cJSON_GetObjectItemCaseSensitive(f, kKinds[k]);
            it.files[k].size = static_cast<uint64_t>(n(fi, "size"));
            it.files[k].sha256 = s(fi, "sha256");
        }
        cJSON_Delete(f);
    } else {   // older/recovered entry: hash now
        for (int k = 0; k < 4; k++) it.files[k].sha256 = Sha256::ofFile(dir + "/" + kFiles[k], &it.files[k].size);
    }
    if (fsutil::readFile(dir + "/transfer.json", txt)) {
        cJSON* t = cJSON_Parse(txt.c_str());
        if (t) {
            it.transfer = s(t, "state");
            it.acked_sha = s(t, "sha256");
            cJSON_Delete(t);
        }
    }
    if (it.transfer.empty()) it.transfer = "available";
    return true;
}

bool RecordingStore::writeTransfer(const Item& it) {
    cJSON* t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "state", it.transfer.c_str());
    cJSON_AddStringToObject(t, "sha256", it.acked_sha.c_str());
    char* txt = cJSON_PrintUnformatted(t);
    const bool ok = fsutil::writeFileAtomic(finalDir(it.rid) + "/transfer.json", txt);
    cJSON_free(txt);
    cJSON_Delete(t);
    return ok;
}

bool RecordingStore::commit(const std::string& rid) {
    const std::string src = partialDir(rid);
    cJSON* f = cJSON_CreateObject();
    for (int k = 0; k < 4; k++) {
        uint64_t size = 0;
        const std::string sha = Sha256::ofFile(src + "/" + kFiles[k], &size);
        cJSON* fi = cJSON_AddObjectToObject(f, kKinds[k]);
        cJSON_AddNumberToObject(fi, "size", static_cast<double>(size));
        cJSON_AddStringToObject(fi, "sha256", sha.c_str());
    }
    char* txt = cJSON_PrintUnformatted(f);
    bool ok = fsutil::writeFileAtomic(src + "/files.json", txt);
    cJSON_free(txt);
    cJSON_Delete(f);
    ok = ok && fsutil::writeFileAtomic(src + "/transfer.json", "{\"state\":\"available\",\"sha256\":\"\"}");
    fsutil::fsyncDir(src);
    if (!ok || ::rename(src.c_str(), finalDir(rid).c_str()) != 0) {
        std::cerr << "[store] commit of " << rid << " failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    fsutil::fsyncDir(root_ + "/.partial");
    fsutil::fsyncDir(root_);
    std::lock_guard<std::mutex> l(mu_);
    Item it;
    if (loadItem(rid, it)) items_[rid] = it;
    return true;
}

bool RecordingStore::resolve(const std::string& rid, const std::string& kind, std::string& path, std::string& sha) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = items_.find(rid);
    if (it == items_.end()) return false;
    for (int k = 0; k < 4; k++) {
        if (kind == kKinds[k]) {
            path = finalDir(rid) + "/" + kFiles[k];
            sha = it->second.files[k].sha256;
            return true;
        }
    }
    return false;
}

cJSON* RecordingStore::itemJson(const Item& it) const {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "recording_id", it.rid.c_str());
    cJSON_AddStringToObject(o, "dataset_id", it.dataset_id.c_str());
    cJSON_AddStringToObject(o, "subject_id", it.subject_id.c_str());
    cJSON_AddNumberToObject(o, "take", it.take);
    cJSON_AddNumberToObject(o, "start_utc_ms", static_cast<double>(it.start_utc_ms));
    cJSON_AddNumberToObject(o, "duration_ms", it.duration_ms);
    cJSON_AddNumberToObject(o, "usable_ms", it.usable_ms);
    cJSON_AddStringToObject(o, "completion", it.completion.c_str());
    cJSON_AddStringToObject(o, "transfer", it.transfer.c_str());
    cJSON* f = cJSON_AddObjectToObject(o, "files");
    for (int k = 0; k < 4; k++) {
        cJSON* fi = cJSON_AddObjectToObject(f, kKinds[k]);
        cJSON_AddNumberToObject(fi, "size", static_cast<double>(it.files[k].size));
        cJSON_AddStringToObject(fi, "sha256", it.files[k].sha256.c_str());
    }
    return o;
}

cJSON* RecordingStore::manifest(int offset, int limit) {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<const Item*> v;
    for (const auto& kv : items_) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const Item* a, const Item* b) { return a->start_utc_ms < b->start_utc_ms; });
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "total", static_cast<double>(v.size()));
    cJSON_AddNumberToObject(o, "offset", offset);
    cJSON* arr = cJSON_AddArrayToObject(o, "items");
    for (int i = offset; i < static_cast<int>(v.size()) && i < offset + limit; i++)
        cJSON_AddItemToArray(arr, itemJson(*v[i]));
    return o;
}

std::string RecordingStore::ack(const std::string& rid, const std::string& sha, int64_t) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = items_.find(rid);
    if (it == items_.end()) return "NOT_FOUND";
    if (sha != it->second.files[0].sha256) return "HASH_MISMATCH";
    if (it->second.transfer == "verified") return "";     // idempotent
    Item copy = it->second;
    copy.transfer = "verified";
    copy.acked_sha = sha;
    if (!writeTransfer(copy)) return "IO";
    it->second = copy;
    return "";
}

std::string RecordingStore::remove(const std::string& rid, const std::string& sha) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = items_.find(rid);
    if (it == items_.end()) return "";                   // already gone: idempotent
    if (it->second.transfer != "verified" || it->second.acked_sha != sha || sha != it->second.files[0].sha256)
        return "NOT_VERIFIED";
    if (!fsutil::removeTree(finalDir(rid))) return "IO";
    fsutil::fsyncDir(root_);
    items_.erase(it);
    return "";
}

cJSON* RecordingStore::clear(bool force) {
    std::vector<std::string> rids;
    {
        std::lock_guard<std::mutex> l(mu_);
        for (const auto& kv : items_) rids.push_back(kv.first);
    }
    cJSON* o = cJSON_CreateObject();
    cJSON* del = cJSON_AddArrayToObject(o, "deleted");
    cJSON* kept = cJSON_AddArrayToObject(o, "kept");
    for (const auto& rid : rids) {
        std::string err;
        {
            std::lock_guard<std::mutex> l(mu_);
            Item& it = items_[rid];
            if (it.transfer == "verified" || force) {
                if (fsutil::removeTree(finalDir(rid))) items_.erase(rid);
                else err = "IO";
            } else {
                err = "NOT_VERIFIED";
            }
        }
        if (err.empty()) cJSON_AddItemToArray(del, cJSON_CreateString(rid.c_str()));
        else {
            cJSON* k = cJSON_CreateObject();
            cJSON_AddStringToObject(k, "recording_id", rid.c_str());
            cJSON_AddStringToObject(k, "err", err.c_str());
            cJSON_AddItemToArray(kept, k);
        }
    }
    fsutil::fsyncDir(root_);
    return o;
}

RecordingStore::Counts RecordingStore::counts() {
    std::lock_guard<std::mutex> l(mu_);
    Counts c;
    for (const auto& kv : items_) {
        c.total++;
        if (kv.second.transfer != "verified") c.unverified++;
        if (kv.second.completion != "complete") c.incomplete++;
        c.total_ms += kv.second.duration_ms;
    }
    return c;
}
