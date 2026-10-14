#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ESP32 <-> RV1103 UART link endpoint (PCGKolect_app/docs/PROTOCOL.md §1).
//
//   audio   (ESP->RV): A5 5A | LEN u16 | SEQ u8  | PCM[LEN]       | CRC16
//   message (both)   : A5 5C | LEN u16 | TYPE u8 | SEQ u8 | DATA  | CRC16
//
// A background thread parses both frame kinds. Audio goes into a ring buffer
// with an absolute sample counter; missing audio frames (SEQ gap) are filled
// with zeros so the time axis stays true, and every gap is reported. Message
// frames are handed to a callback. sendMsg()/sendChunk() write whole frames.
class PcgStream {
public:
    static constexpr int kLinkProto  = 1;
    static constexpr int kAudioProto = 1;
    static constexpr int kDefaultBaud = 460800;

    static constexpr uint8_t kReq   = 0x01;
    static constexpr uint8_t kRsp   = 0x02;
    static constexpr uint8_t kEvt   = 0x03;
    static constexpr uint8_t kChunk = 0x10;
    static constexpr size_t  kMaxPayload = 4200;
    static constexpr int     kMaxFillFrames = 20;   // longer SEQ jumps are discontinuities, not filled

    struct Config {
        const char* device       = "/dev/ttyS3";
        int         baud         = kDefaultBaud;
        size_t      ring_samples = 64000;   // 8 s @ 8 kHz
    };

    struct Stats {
        uint64_t frames_ok     = 0;   // audio frames with valid CRC
        uint64_t crc_errors    = 0;   // audio + message frames with bad CRC
        uint64_t malformed     = 0;   // impossible LEN
        uint64_t seq_gaps      = 0;   // missing audio frames (by SEQ)
        uint64_t discontinuity = 0;   // SEQ jumps too large to fill
        uint64_t filled_samples= 0;   // zero samples inserted for gaps
        uint64_t overruns      = 0;   // samples dropped because the consumer fell behind
        uint64_t bytes_read    = 0;
        uint64_t msg_ok        = 0;
        uint64_t msg_tx        = 0;
        uint64_t write_errors  = 0;
        int      baud          = 0;
    };

    enum GapReason : uint8_t { GAP_SEQ = 1, GAP_DISCONTINUITY = 2, GAP_OVERRUN = 3 };
    struct Gap {
        uint64_t at_sample;   // absolute stream sample index where the gap starts
        uint32_t samples;     // samples zero-filled (SEQ) or dropped (OVERRUN); 0 for DISCONTINUITY
        GapReason reason;
    };

    using MsgHandler = std::function<void(uint8_t type, const uint8_t* data, size_t len)>;

    PcgStream();
    ~PcgStream();

    bool open(const Config& cfg);
    void close();

    // Blocking read of up to `count` samples (waits up to timeout_ms).
    // *start_abs receives the absolute index of out[0].
    size_t read(int16_t* out, size_t count, int timeout_ms = 100, uint64_t* start_abs = nullptr);
    size_t available() const;
    Stats stats() const;
    std::vector<Gap> takeGaps();

    void setMessageHandler(MsgHandler h) { msgHandler_ = std::move(h); }
    bool sendMsg(uint8_t type, const void* data, size_t len);
    bool sendChunk(uint32_t req_id, uint32_t offset, const uint8_t* data, size_t len);

    // Change baud after the current output drains. Reverts automatically if no
    // valid frame arrives within 2 s at the new rate.
    bool switchBaud(int baud);
    int  baud() const { return baud_.load(); }

    static uint16_t crc16(const uint8_t* data, size_t len);
    static bool setTermiosBaud(int fd, int baud);

private:
    void readerLoop();
    void handleAudio(uint8_t seq, const uint8_t* pcm, size_t len);
    void ringPush(const int16_t* data, size_t count);     // mutex_ held
    size_t ringAvailable() const;                         // mutex_ held

    Config cfg_;
    int fd_ = -1;
    std::thread readerThread_;
    std::atomic<bool> running_{false};
    std::atomic<int> baud_{0};
    MsgHandler msgHandler_;

    // ring with absolute indices: samples [head_abs_, tail_abs_) are buffered
    std::vector<int16_t> ring_;
    uint64_t head_abs_ = 0, tail_abs_ = 0;
    std::vector<Gap> gaps_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Stats stats_;

    int  expect_seq_ = -1;
    std::mutex tx_mutex_;
    uint8_t tx_seq_ = 0;
    std::vector<uint8_t> txbuf_;

    // baud revert watchdog
    std::atomic<bool> revert_armed_{false};
    int revert_to_ = 0;
    uint64_t ok_at_switch_ = 0;
    std::chrono::steady_clock::time_point revert_deadline_;

    // parser
    enum State { S_SYNC0, S_SYNC1, S_HDR, S_BODY };
    State st_ = S_SYNC0;
    bool is_msg_ = false;
    size_t idx_ = 0, need_ = 0;
    uint8_t fbuf_[kMaxPayload + 16];
};
