#include "pcg_stream.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <algorithm>
#include <iostream>

// ---------------------------------------------------------------------------
// CRC16-CCITT-FALSE
// ---------------------------------------------------------------------------

uint16_t PcgStream::crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            else
                crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Baud-rate mapping
// ---------------------------------------------------------------------------
//
// cfsetispeed()/cfsetospeed() take a termios speed_t constant (e.g. B460800),
// NOT the numeric baud value. Passing the raw number (460800) leaves the port
// at the wrong speed because the Bxxxx encoding is a small integer
// (B460800 == 0010004 octal == 4100), while 460800 is a completely different
// bit pattern. Map explicitly.

static speed_t termiosSpeed(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}

// ---------------------------------------------------------------------------
// Ring buffer helpers (caller must hold mutex_)
// ---------------------------------------------------------------------------

size_t PcgStream::ringAvailable() const {
    if (ringTail_ >= ringHead_)
        return ringTail_ - ringHead_;
    return ringCapacity_ - ringHead_ + ringTail_;
}

void PcgStream::ringPush(const int16_t* data, size_t count) {
    // If the buffer would overflow, drop the oldest samples.
    for (size_t i = 0; i < count; i++) {
        ring_[ringTail_] = data[i];
        ringTail_ = (ringTail_ + 1) % ringCapacity_;
        if (ringTail_ == ringHead_) {
            ringHead_ = (ringHead_ + 1) % ringCapacity_;
        }
    }
}

size_t PcgStream::ringPop(int16_t* out, size_t count) {
    size_t avail = ringAvailable();
    size_t n = std::min(count, avail);
    for (size_t i = 0; i < n; i++) {
        out[i] = ring_[ringHead_];
        ringHead_ = (ringHead_ + 1) % ringCapacity_;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

PcgStream::PcgStream() = default;
PcgStream::~PcgStream() { close(); }

bool PcgStream::open(const Config& cfg) {
    cfg_ = cfg;

    // Open UART (global ::open — the member function shadows it otherwise)
    fd_ = ::open(cfg_.device, O_RDWR | O_NOCTTY);
    if (fd_ < 0) {
        std::cerr << "[PcgStream] Failed to open " << cfg_.device
                  << ": " << strerror(errno) << std::endl;
        return false;
    }

    // Configure termios
    struct termios tty;
    if (tcgetattr(fd_, &tty) != 0) {
        std::cerr << "[PcgStream] tcgetattr failed: " << strerror(errno) << std::endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if (cfsetispeed(&tty, termiosSpeed(cfg_.baud)) != 0 ||
        cfsetospeed(&tty, termiosSpeed(cfg_.baud)) != 0) {
        std::cerr << "[PcgStream] unsupported baud rate: " << cfg_.baud << std::endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 8N1, raw mode
    tty.c_cflag &= ~PARENB;   // no parity
    tty.c_cflag &= ~CSTOPB;   // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;       // 8 data bits
    tty.c_cflag |= CREAD | CLOCAL;  // enable receiver, ignore modem lines
    tty.c_cflag &= ~CRTSCTS;  // no hardware flow control

    // Act as raw device
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                     INLCR | IGNCR | ICRNL | IXON | IXOFF);
    tty.c_oflag &= ~OPOST;

    // Block up to 100 ms for at least one byte, then return.
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        std::cerr << "[PcgStream] tcsetattr failed: " << strerror(errno) << std::endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Flush any stale bytes in the driver buffers.
    tcflush(fd_, TCIOFLUSH);

    // Allocate ring buffer
    ringCapacity_ = cfg_.ring_samples;
    ring_.resize(ringCapacity_);
    ringHead_ = ringTail_ = 0;

    // Start reader thread
    running_ = true;
    frameState_ = SEARCH;
    frameIdx_ = 0;
    stats_ = {};
    readerThread_ = std::thread(PcgStream::readerThread, this);

    return true;
}

void PcgStream::close() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();

    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

size_t PcgStream::read(int16_t* out, size_t count, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);

    while (ringAvailable() < count) {
        if (timeout_ms <= 0) break;
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
            break;
    }

    return ringPop(out, count);
}

size_t PcgStream::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ringAvailable();
}

PcgStream::Stats PcgStream::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ---------------------------------------------------------------------------
// Reader thread
// ---------------------------------------------------------------------------

void* PcgStream::readerThread(void* arg) {
    static_cast<PcgStream*>(arg)->readerLoop();
    return nullptr;
}

void PcgStream::readerLoop() {
    uint8_t readBuf[4096];
    int16_t pcmBuf[1000];   // max LEN = 2000 bytes -> 1000 samples

    while (running_) {
        ssize_t n = ::read(fd_, readBuf, sizeof(readBuf));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;  // non-blocking / interrupted — retry
            }
            if (n < 0) {
                usleep(10000);  // transient read error — back off
                continue;
            }
            continue;  // n == 0: VTIME tick with no data
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.bytes_read += static_cast<uint64_t>(n);
        }

        // Parse frames from readBuf
        for (ssize_t i = 0; i < n; i++) {
            const uint8_t b = readBuf[i];

            switch (frameState_) {
            case SEARCH:
                if (b == 0xA5) {
                    frameBuf_[0] = 0xA5;
                    frameIdx_ = 1;
                    frameState_ = SYNC_HI;
                }
                break;

            case SYNC_HI:
                // Second sync byte must be 0x5A.
                if (b == 0x5A) {
                    frameBuf_[1] = 0x5A;
                    frameIdx_ = 2;
                    frameState_ = LEN_LO;
                } else if (b == 0xA5) {
                    // This byte may itself start a new sync.
                    frameBuf_[0] = 0xA5;
                    frameIdx_ = 1;
                } else {
                    frameState_ = SEARCH;
                    frameIdx_ = 0;
                }
                break;

            case LEN_LO:
                frameBuf_[2] = b;
                frameIdx_ = 3;
                frameState_ = LEN_HI;
                break;

            case LEN_HI: {
                frameBuf_[3] = b;
                const uint16_t len = static_cast<uint16_t>(frameBuf_[2])
                                   | (static_cast<uint16_t>(frameBuf_[3]) << 8);
                if (len == 0 || len > 2000 || (len % 2) != 0) {
                    // Malformed length — resync.
                    frameState_ = SEARCH;
                    frameIdx_ = 0;
                    break;
                }
                frameIdx_ = 4;
                frameState_ = SEQ;
                break;
            }

            case SEQ:
                frameBuf_[4] = b;
                frameIdx_ = 5;
                frameState_ = PAYLOAD;
                break;

            case PAYLOAD: {
                const uint16_t len = static_cast<uint16_t>(frameBuf_[2])
                                   | (static_cast<uint16_t>(frameBuf_[3]) << 8);
                frameBuf_[frameIdx_++] = b;
                if (frameIdx_ >= static_cast<size_t>(5 + len)) {
                    frameState_ = CRC_LO;
                }
                break;
            }

            case CRC_LO:
                // First CRC byte lands at index 5 + len.
                frameBuf_[frameIdx_++] = b;
                frameState_ = CRC_HI;
                break;

            case CRC_HI: {
                // Second CRC byte lands at index 6 + len.
                frameBuf_[frameIdx_++] = b;

                const uint16_t len = static_cast<uint16_t>(frameBuf_[2])
                                   | (static_cast<uint16_t>(frameBuf_[3]) << 8);
                const uint16_t rx_crc =
                    static_cast<uint16_t>(frameBuf_[5 + len])
                  | (static_cast<uint16_t>(frameBuf_[6 + len]) << 8);
                const uint16_t calc = crc16(frameBuf_ + 2, 3 + len);

                if (rx_crc == calc) {
                    // Copy PCM into an aligned, little-endian buffer.
                    const size_t samples = len / 2;
                    for (size_t k = 0; k < samples; k++) {
                        pcmBuf[k] = static_cast<int16_t>(
                            static_cast<uint16_t>(frameBuf_[5 + 2 * k]) |
                            (static_cast<uint16_t>(frameBuf_[6 + 2 * k]) << 8));
                    }
                    pushToRing(pcmBuf, samples);
                    std::lock_guard<std::mutex> lock(mutex_);
                    stats_.frames_ok++;
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stats_.crc_errors++;
                }

                frameState_ = SEARCH;
                frameIdx_ = 0;
                break;
            }
            }
        }
    }
}

void PcgStream::pushToRing(const int16_t* data, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    ringPush(data, count);
    cv_.notify_all();
}
