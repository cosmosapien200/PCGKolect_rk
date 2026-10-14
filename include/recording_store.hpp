#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"

// On-disk recordings (PROTOCOL.md §5):
//   <root>/<rid>/{audio.wav, beats.csv, quality.csv, meta.json}   immutable after finalise
//   <root>/<rid>/files.json      sizes + SHA-256 of the four files (written at finalise)
//   <root>/<rid>/transfer.json   {"state","sha256","acked_utc_ms"}   transfer state, survives reboot
//   <root>/.partial/<rid>/       session in progress; renamed into place on finalise
// Deletion is only allowed for recordings the phone acknowledged with the matching audio hash.
class RecordingStore {
public:
    static constexpr const char* kKinds[4] = {"audio", "beats", "quality", "meta"};
    static constexpr const char* kFiles[4] = {"audio.wav", "beats.csv", "quality.csv", "meta.json"};

    struct FileInfo { uint64_t size = 0; std::string sha256; };
    struct Item {
        std::string rid, dataset_id, subject_id, completion, transfer = "available", acked_sha;
        int         take = 0;
        int64_t     start_utc_ms = 0;
        double      duration_ms = 0, usable_ms = 0;
        FileInfo    files[4];
    };

    explicit RecordingStore(std::string root = "/root/recordings") : root_(std::move(root)) {}

    bool init();                                     // mkdir + load index
    const std::string& root() const { return root_; }
    std::string partialDir(const std::string& rid) const { return root_ + "/.partial/" + rid; }
    std::string finalDir(const std::string& rid) const { return root_ + "/" + rid; }
    std::vector<std::string> partials() const;       // rids of unfinished sessions (after a crash)

    // Hash files in the partial dir, write files.json + transfer.json, fsync, rename into place.
    bool commit(const std::string& rid);

    bool resolve(const std::string& rid, const std::string& kind, std::string& path, std::string& sha);
    cJSON* manifest(int offset, int limit);          // {"total","items":[...]}
    cJSON* itemJson(const Item& it) const;

    // Returns "" on success or an error code (NOT_FOUND, HASH_MISMATCH, NOT_VERIFIED, IO).
    std::string ack(const std::string& rid, const std::string& sha, int64_t utc_ms);
    std::string remove(const std::string& rid, const std::string& sha);
    cJSON* clear(bool force);                        // {"deleted":[...],"kept":[...]}

    struct Counts { int total = 0, unverified = 0, incomplete = 0; double total_ms = 0; };
    Counts counts();
    static bool diskSpace(const std::string& path, uint64_t& total, uint64_t& free_bytes);

private:
    bool loadItem(const std::string& rid, Item& it);
    bool writeTransfer(const Item& it);

    std::string root_;
    std::map<std::string, Item> items_;              // rid -> item
    std::mutex mu_;
};

// Small file helpers shared with the session engine.
namespace fsutil {
bool writeFileAtomic(const std::string& path, const std::string& data);   // tmp + fsync + rename
bool readFile(const std::string& path, std::string& out);
bool mkdirs(const std::string& path);
bool removeTree(const std::string& path);
void fsyncDir(const std::string& path);
}
