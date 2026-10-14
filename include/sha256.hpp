#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Minimal SHA-256 (FIPS 180-4) for file integrity hashes.
class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::string hexdigest();          // finalises; call reset() to reuse

    static std::string ofFile(const std::string& path, uint64_t* size = nullptr);   // "" on error

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t  buf_[64];
    size_t   blen_;
    uint64_t total_;
};
