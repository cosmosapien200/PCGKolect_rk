#include "pcg_stream.hpp"

#include <iostream>
#include <vector>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

// ---------------------------------------------------------------------------
// Minimal WAV header writer (44 bytes, 16-bit PCM)
// ---------------------------------------------------------------------------

static constexpr uint32_t SAMPLE_RATE = 8000;
static constexpr uint16_t CHANNELS    = 1;
static constexpr uint16_t BITS_PER_SAMPLE = 16;
static constexpr uint16_t BLOCK_ALIGN = CHANNELS * BITS_PER_SAMPLE / 8;
static constexpr uint32_t BYTES_PER_SEC = SAMPLE_RATE * BLOCK_ALIGN;

struct WavHeader {
    char riff[4]   = {'R', 'I', 'F', 'F'};
    uint32_t fileSize = 0;            // will be set to 36 + numSamples * 2
    char wave[4]   = {'W', 'A', 'V', 'E'};
    char fmt[4]    = {'f', 'm', 't', ' '};
    uint32_t fmtSize  = 16;
    uint16_t audioFormat = 1;         // PCM
    uint16_t numChannels = CHANNELS;
    uint32_t sampleRate = SAMPLE_RATE;
    uint32_t byteRate = BYTES_PER_SEC;
    uint16_t blockAlign = BLOCK_ALIGN;
    uint16_t bitsPerSample = BITS_PER_SAMPLE;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;            // will be set to numSamples * 2
};

static void writeWav(const char* path, const int16_t* samples, size_t count) {
    WavHeader hdr;
    hdr.fileSize  = 36 + count * 2;
    hdr.dataSize  = count * 2;

    FILE* fp = fopen(path, "wb");
    if (!fp) {
        std::perror("Failed to open WAV file");
        return;
    }

    fwrite(&hdr, 1, sizeof(hdr), fp);
    fwrite(samples, sizeof(int16_t), count, fp);
    fclose(fp);
}

// ---------------------------------------------------------------------------
// Ensure directory exists
// ---------------------------------------------------------------------------

static void ensureDir(const char* path) {
    // Extract directory part (everything up to last '/')
    std::string dir(path);
    auto pos = dir.rfind('/');
    if (pos != std::string::npos) {
        dir = dir.substr(0, pos);
        mkdir(dir.c_str(), 0755);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    const char* outPath = "/root/recordings/recording.wav";
    constexpr size_t RECORD_SECONDS = 5;
    constexpr size_t TOTAL_SAMPLES = RECORD_SECONDS * SAMPLE_RATE; // 40000

    // Ensure output directory exists
    ensureDir(outPath);

    // --- Configure & open UART ---
    PcgStream stream;
    PcgStream::Config cfg;
    cfg.device = "/dev/ttyS3";
    cfg.baud   = 460800;
    // Use a larger ring buffer so we can drain 5 seconds worth
    cfg.ring_samples = TOTAL_SAMPLES + 8000;

    if (!stream.open(cfg)) {
        std::cerr << "[test] Failed to open UART " << cfg.device << std::endl;
        return 1;
    }
    std::cout << "[test] UART connected: " << cfg.device
              << " @ " << cfg.baud << std::endl;

    // --- Record ---
    std::cout << "[test] Recording " << RECORD_SECONDS << "s ("
              << TOTAL_SAMPLES << " samples)..." << std::endl;

    std::vector<int16_t> buffer(TOTAL_SAMPLES);
    size_t offset = 0;

    while (offset < TOTAL_SAMPLES) {
        size_t toRead = TOTAL_SAMPLES - offset;
        // Read in chunks of 800 (100ms) to keep latency low
        size_t chunk = std::min(toRead, static_cast<size_t>(800));
        size_t got = stream.read(buffer.data() + offset, chunk, 500);
        if (got == 0) {
            std::cerr << "[test] Timeout waiting for audio data" << std::endl;
            break;
        }
        offset += got;
    }

    stream.close();

    // --- Write WAV ---
    if (offset > 0) {
        writeWav(outPath, buffer.data(), offset);
        auto s = stream.stats();
        std::cout << "[test] Saved " << offset << " samples to " << outPath
                  << " (" << (offset * 2 + 44) << " bytes)" << std::endl;
        std::cout << "[test] Stats: frames_ok=" << s.frames_ok
                  << " crc_errors=" << s.crc_errors
                  << " bytes_read=" << s.bytes_read << std::endl;
    } else {
        std::cerr << "[test] No audio data captured" << std::endl;
        return 1;
    }

    return 0;
}
