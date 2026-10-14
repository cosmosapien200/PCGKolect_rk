#include "pcg_stream.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

// ---------------------------------------------------------------------------
// CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF)
// ---------------------------------------------------------------------------

uint16_t PcgStream::crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

// cfsetispeed()/cfsetospeed() take Bxxxx constants, not numeric rates.
static bool termiosSpeed(int baud, speed_t& out) {
    switch (baud) {
        case 115200:  out = B115200;  return true;
        case 230400:  out = B230400;  return true;
        case 460800:  out = B460800;  return true;
        case 921600:  out = B921600;  return true;
        case 1000000: out = B1000000; return true;
        case 1500000: out = B1500000; return true;
        case 2000000: out = B2000000; return true;
        case 3000000: out = B3000000; return true;
        case 4000000: out = B4000000; return true;
        default: return false;
    }
}

bool PcgStream::setTermiosBaud(int fd, int baud) {
    speed_t sp;
    if (!termiosSpeed(baud, sp)) return false;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return false;
    cfsetispeed(&tty, sp);
    cfsetospeed(&tty, sp);
    return tcsetattr(fd, TCSANOW, &tty) == 0;
}

// ---------------------------------------------------------------------------
// Ring buffer (mutex_ held)
// ---------------------------------------------------------------------------

size_t PcgStream::ringAvailable() const {
    return static_cast<size_t>(tail_abs_ - head_abs_);
}

void PcgStream::ringPush(const int16_t* data, size_t count) {
    const size_t cap = ring_.size();
    for (size_t i = 0; i < count; i++) {
        ring_[tail_abs_ % cap] = data[i];
        tail_abs_++;
    }
    if (tail_abs_ - head_abs_ > cap) {             // consumer fell behind: drop oldest
        const uint64_t drop = tail_abs_ - head_abs_ - cap;
        gaps_.push_back({head_abs_, static_cast<uint32_t>(drop), GAP_OVERRUN});
        stats_.overruns += drop;
        head_abs_ += drop;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

PcgStream::PcgStream() = default;
PcgStream::~PcgStream() { close(); }

bool PcgStream::open(const Config& cfg) {
    cfg_ = cfg;
    fd_ = ::open(cfg_.device, O_RDWR | O_NOCTTY);
    if (fd_ < 0) {
        std::cerr << "[PcgStream] Failed to open " << cfg_.device << ": " << strerror(errno) << std::endl;
        return false;
    }

    struct termios tty;
    if (tcgetattr(fd_, &tty) != 0) {
        std::cerr << "[PcgStream] tcgetattr failed: " << strerror(errno) << std::endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    cfmakeraw(&tty);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;            // return after 100 ms without data
    if (tcsetattr(fd_, TCSANOW, &tty) != 0 || !setTermiosBaud(fd_, cfg_.baud)) {
        std::cerr << "[PcgStream] cannot configure " << cfg_.device << " @ " << cfg_.baud << std::endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    tcflush(fd_, TCIOFLUSH);
    baud_ = cfg_.baud;

    ring_.assign(cfg_.ring_samples, 0);
    head_abs_ = tail_abs_ = 0;
    stats_ = {};
    stats_.baud = cfg_.baud;
    st_ = S_SYNC0;
    expect_seq_ = -1;
    txbuf_.resize(kMaxPayload + 16);
    running_ = true;
    readerThread_ = std::thread(&PcgStream::readerLoop, this);
    return true;
}

void PcgStream::close() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    if (readerThread_.joinable()) readerThread_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

size_t PcgStream::read(int16_t* out, size_t count, int timeout_ms, uint64_t* start_abs) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (ringAvailable() < count && running_) {
        if (timeout_ms <= 0 || cv_.wait_until(lock, deadline) == std::cv_status::timeout) break;
    }
    const size_t n = std::min(count, ringAvailable());
    if (start_abs) *start_abs = head_abs_;
    const size_t cap = ring_.size();
    for (size_t i = 0; i < n; i++) out[i] = ring_[(head_abs_ + i) % cap];
    head_abs_ += n;
    return n;
}

size_t PcgStream::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ringAvailable();
}

PcgStream::Stats PcgStream::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s = stats_;
    s.baud = baud_;
    return s;
}

std::vector<PcgStream::Gap> PcgStream::takeGaps() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Gap> g;
    g.swap(gaps_);
    return g;
}

bool PcgStream::sendMsg(uint8_t type, const void* data, size_t len) {
    if (len > kMaxPayload || fd_ < 0) return false;
    std::lock_guard<std::mutex> lock(tx_mutex_);
    uint8_t* f = txbuf_.data();
    f[0] = 0xA5;
    f[1] = 0x5C;
    f[2] = static_cast<uint8_t>(len & 0xFF);
    f[3] = static_cast<uint8_t>(len >> 8);
    f[4] = type;
    f[5] = tx_seq_++;
    if (len) std::memcpy(f + 6, data, len);
    const uint16_t crc = crc16(f + 2, 4 + len);
    f[6 + len] = static_cast<uint8_t>(crc & 0xFF);
    f[7 + len] = static_cast<uint8_t>(crc >> 8);
    size_t off = 0, total = 8 + len;
    while (off < total) {
        ssize_t w = ::write(fd_, f + off, total - off);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            std::lock_guard<std::mutex> l2(mutex_);
            stats_.write_errors++;
            return false;
        }
        off += static_cast<size_t>(w);
    }
    std::lock_guard<std::mutex> l2(mutex_);
    stats_.msg_tx++;
    return true;
}

bool PcgStream::sendChunk(uint32_t req_id, uint32_t offset, const uint8_t* data, size_t len) {
    if (len + 8 > kMaxPayload) return false;
    uint8_t buf[kMaxPayload];
    for (int i = 0; i < 4; i++) {
        buf[i]     = static_cast<uint8_t>(req_id >> (8 * i));
        buf[4 + i] = static_cast<uint8_t>(offset >> (8 * i));
    }
    if (len) std::memcpy(buf + 8, data, len);
    return sendMsg(kChunk, buf, len + 8);
}

bool PcgStream::switchBaud(int baud) {
    speed_t sp;
    if (fd_ < 0 || !termiosSpeed(baud, sp)) return false;
    std::lock_guard<std::mutex> lock(tx_mutex_);
    tcdrain(fd_);
    usleep(5000);
    const int old = baud_;
    if (!setTermiosBaud(fd_, baud)) return false;
    tcflush(fd_, TCIFLUSH);
    {
        std::lock_guard<std::mutex> l2(mutex_);
        ok_at_switch_ = stats_.frames_ok + stats_.msg_ok;
        st_ = S_SYNC0;
    }
    revert_to_ = old;
    revert_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    revert_armed_ = true;
    baud_ = baud;
    std::cout << "[PcgStream] baud " << old << " -> " << baud << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Reader thread
// ---------------------------------------------------------------------------

void PcgStream::handleAudio(uint8_t seq, const uint8_t* pcm, size_t len) {
    static int16_t samples[1000];
    static const int16_t zeros[800] = {};
    const size_t n = len / 2;
    for (size_t k = 0; k < n; k++)
        samples[k] = static_cast<int16_t>(static_cast<uint16_t>(pcm[2 * k]) | (static_cast<uint16_t>(pcm[2 * k + 1]) << 8));

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.frames_ok++;
    if (expect_seq_ >= 0 && seq != expect_seq_) {
        const int missing = (seq - expect_seq_ + 256) % 256;
        if (missing <= kMaxFillFrames) {
            gaps_.push_back({tail_abs_, static_cast<uint32_t>(missing * n), GAP_SEQ});
            stats_.seq_gaps += missing;
            stats_.filled_samples += missing * n;
            for (int m = 0; m < missing; m++) ringPush(zeros, std::min<size_t>(n, 800));
        } else {
            gaps_.push_back({tail_abs_, 0, GAP_DISCONTINUITY});
            stats_.discontinuity++;
        }
    }
    expect_seq_ = (seq + 1) & 0xFF;
    ringPush(samples, n);
    cv_.notify_all();
}

void PcgStream::readerLoop() {
    uint8_t rb[4096];
    uint64_t last_ok = 0;
    auto last_ok_time = std::chrono::steady_clock::now();
    while (running_) {
        // Link-loss fallback: after 3 s without any valid frame at a non-default
        // rate (e.g. the ESP rebooted to the default), return to the default.
        {
            uint64_t ok;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ok = stats_.frames_ok + stats_.msg_ok;
            }
            const auto now = std::chrono::steady_clock::now();
            if (ok != last_ok) {
                last_ok = ok;
                last_ok_time = now;
            } else if (baud_ != kDefaultBaud && !revert_armed_ && now - last_ok_time > std::chrono::seconds(3)) {
                std::lock_guard<std::mutex> lock(tx_mutex_);
                setTermiosBaud(fd_, kDefaultBaud);
                std::cout << "[PcgStream] link silent at " << baud_ << ", back to " << kDefaultBaud << std::endl;
                baud_ = kDefaultBaud;
                last_ok_time = now;
            }
        }
        if (revert_armed_ && std::chrono::steady_clock::now() > revert_deadline_) {
            revert_armed_ = false;
            uint64_t ok;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ok = stats_.frames_ok + stats_.msg_ok;
            }
            if (ok == ok_at_switch_) {
                std::lock_guard<std::mutex> lock(tx_mutex_);
                setTermiosBaud(fd_, revert_to_);
                std::cout << "[PcgStream] no valid frames at " << baud_ << ", reverted to " << revert_to_ << std::endl;
                baud_ = revert_to_;
            }
        }

        ssize_t n = ::read(fd_, rb, sizeof(rb));
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EINTR) usleep(10000);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.bytes_read += static_cast<uint64_t>(n);
        }

        for (ssize_t i = 0; i < n; i++) {
            const uint8_t b = rb[i];
            switch (st_) {
            case S_SYNC0:
                if (b == 0xA5) st_ = S_SYNC1;
                break;
            case S_SYNC1:
                if (b == 0x5A || b == 0x5C) {
                    is_msg_ = (b == 0x5C);
                    fbuf_[0] = 0xA5;
                    fbuf_[1] = b;
                    idx_ = 2;
                    st_ = S_HDR;
                } else if (b != 0xA5) {
                    st_ = S_SYNC0;
                }
                break;
            case S_HDR: {
                fbuf_[idx_++] = b;
                const size_t hdr = is_msg_ ? 6 : 5;
                if (idx_ == hdr) {
                    const size_t len = static_cast<size_t>(fbuf_[2]) | (static_cast<size_t>(fbuf_[3]) << 8);
                    const bool bad = is_msg_ ? (len > kMaxPayload) : (len == 0 || len > 2000 || (len % 2) != 0);
                    if (bad) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        stats_.malformed++;
                        st_ = S_SYNC0;
                        break;
                    }
                    need_ = hdr + len + 2;
                    st_ = S_BODY;
                }
                break;
            }
            case S_BODY:
                fbuf_[idx_++] = b;
                if (idx_ == need_) {
                    const size_t hdr = is_msg_ ? 6 : 5;
                    const size_t len = need_ - hdr - 2;
                    const uint16_t rx = static_cast<uint16_t>(fbuf_[need_ - 2]) | (static_cast<uint16_t>(fbuf_[need_ - 1]) << 8);
                    if (crc16(fbuf_ + 2, hdr - 2 + len) == rx) {
                        if (is_msg_) {
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                stats_.msg_ok++;
                            }
                            if (msgHandler_) msgHandler_(fbuf_[4], fbuf_ + 6, len);
                        } else {
                            handleAudio(fbuf_[4], fbuf_ + 5, len);
                        }
                    } else {
                        std::lock_guard<std::mutex> lock(mutex_);
                        stats_.crc_errors++;
                    }
                    st_ = S_SYNC0;
                }
                break;
            }
        }
    }
}
