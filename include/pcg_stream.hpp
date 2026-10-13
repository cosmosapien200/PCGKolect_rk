#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

// Async UART reader with frame parsing and ring-buffer buffering.
// Background thread decodes frames from /dev/ttyS3 and pushes samples
// into a ring buffer. Consumers call read() to pull PCM chunks.
class PcgStream {
public:
    struct Config {
        const char* device   = "/dev/ttyS3";
        int         baud     = 460800;
        size_t      ring_samples = 32000;  // ~4 s @ 8 kHz
    };

    struct Stats {
        uint64_t frames_ok    = 0;
        uint64_t crc_errors   = 0;
        uint64_t bytes_read   = 0;
    };

    PcgStream();
    ~PcgStream();

    // Open UART, start reader thread. Must call before read().
    bool open(const Config& cfg);

    // Stop reader thread, close UART.
    void close();

    // Blocking read: copies up to `count` samples into `out`.
    // Waits up to `timeout_ms` for data. Returns samples actually copied.
    size_t read(int16_t* out, size_t count, int timeout_ms = 100);

    // Non-blocking: available samples in ring buffer.
    size_t available() const;

    // Thread-safe stats copy.
    Stats stats() const;

private:
    // Reader thread entry point.
    static void* readerThread(void* arg);
    void readerLoop();

    // Push decoded samples into the ring buffer (called from reader thread).
    void pushToRing(const int16_t* data, size_t count);

    // Ring buffer helpers (caller must hold mutex_).
    size_t ringAvailable() const;
    void   ringPush(const int16_t* data, size_t count);
    size_t ringPop(int16_t* out, size_t count);

    // CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF).
    static uint16_t crc16(const uint8_t* data, size_t len);

    // -- State --
    Config cfg_;
    int fd_ = -1;
    std::thread readerThread_;
    std::atomic<bool> running_{false};

    // Ring buffer (protected by mutex_).
    std::vector<int16_t> ring_;
    size_t ringHead_ = 0, ringTail_ = 0, ringCapacity_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    Stats stats_;

    // Frame parser state machine.
    enum FrameState { SEARCH, SYNC_HI, LEN_LO, LEN_HI, SEQ, PAYLOAD, CRC_LO, CRC_HI };
    FrameState frameState_ = SEARCH;
    uint8_t frameBuf_[4096];
    size_t frameIdx_ = 0;
};
