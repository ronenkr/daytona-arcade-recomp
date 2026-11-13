// Renders one MultiPCM VGM log (vgmrips' Daytona USA pack, a .vgz or .vgm) to a 22,050 Hz mono
// 16-bit WAV for the EspRacer port, through this repository's MultiPCM (src/runtime/multipcm.cpp,
// MAME's chip). The pack's logs carry the chip's sample ROM in their data blocks, so no game ROM
// is read here.
//
//   vgm_render IN.vgz OUT.wav [--rate 22050] [--gain 1.0]
//
// Prints one line: peak, output samples, and the loop start in output samples (-1: the log does
// not loop). Timing follows the log (44,100 waits a second); the chip runs at the Model 1 sound
// board's 10 MHz / 224, as MAME does, so the pitch is the board's (HANDOFF.md notes the logs'
// 8 MHz header is about 8 cents flat of it). The loop is the log's: the player jumps from the end
// of the file back to loop_start.
//
// Build: c++ -std=c++20 -O2 -I src tools/esp32/vgm_render.cpp src/runtime/multipcm.cpp -lz
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/multipcm.h"

namespace {

constexpr double kChipClock = 10000000.0;
constexpr double kVgmRate = 44100.0;

std::vector<uint8_t> read_file(const char *path) {
    gzFile f = gzopen(path, "rb"); // reads plain files too
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(1); }
    std::vector<uint8_t> out;
    uint8_t buf[65536];
    for (int n; (n = gzread(f, buf, sizeof buf)) > 0;) out.insert(out.end(), buf, buf + n);
    gzclose(f);
    return out;
}

uint32_t le32(const std::vector<uint8_t> &d, size_t at) {
    return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24;
}

// Command lengths after the opcode (the VGM subset these logs use; unknown ones stop the render).
int length_of(uint8_t c) {
    if (c >= 0x30 && c <= 0x3f) return 1;
    if (c >= 0x40 && c <= 0x4e) return 2;
    if (c == 0x4f || c == 0x50) return 1;
    if (c >= 0x51 && c <= 0x5f) return 2;
    if (c == 0x61) return 2;
    if (c == 0x62 || c == 0x63 || c == 0x66) return 0;
    if (c >= 0x70 && c <= 0x8f) return 0;
    if (c >= 0xa0 && c <= 0xbf) return 2;
    if (c >= 0xc0 && c <= 0xdf) return 3;
    if (c >= 0xe0) return 4;
    return -1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: vgm_render IN.vgz OUT.wav [--rate N] [--gain G]\n"); return 2; }
    double out_rate = 22050, gain = 1.0;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--rate")) out_rate = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--gain")) gain = std::atof(argv[i + 1]);
    }
    const std::vector<uint8_t> d = read_file(argv[1]);
    if (d.size() < 0x40 || std::memcmp(d.data(), "Vgm ", 4)) { std::fprintf(stderr, "not a VGM\n"); return 1; }
    const size_t data_start = 0x34 + le32(d, 0x34);
    const size_t loop_at = le32(d, 0x1c) ? 0x1c + le32(d, 0x1c) : 0; // absolute position of the loop point

    std::vector<uint8_t> rom(0x400000, 0);
    snd::MultiPcm chip(uint32_t(kChipClock), rom.data(), uint32_t(rom.size()));
    // The ROM is complete before the first register write, so every data block is read first.
    for (size_t i = data_start; i < d.size() && d[i] != 0x66;) {
        const uint8_t c = d[i];
        if (c == 0x67) {
            const uint32_t size = le32(d, i + 3) & 0x7fffffff;
            if (d[i + 2] == 0x89 && size >= 8) {
                const uint32_t start = le32(d, i + 11);
                const size_t n = size - 8;
                if (start + n <= rom.size()) std::memcpy(rom.data() + start, d.data() + i + 15, n);
            }
            i += 7 + size;
            continue;
        }
        const int len = length_of(c);
        if (len < 0) { std::fprintf(stderr, "VGM command %02x at %zu not supported\n", c, i); return 1; }
        i += 1 + size_t(len);
    }

    const double chip_rate = chip.sample_rate();
    std::vector<float> mono;
    mono.reserve(size_t(le32(d, 0x18) * chip_rate / kVgmRate) + 65536);
    double chip_target = 0;
    int64_t loop_chip_sample = -1;
    auto advance = [&](double vgm_samples) {
        chip_target += vgm_samples * chip_rate / kVgmRate;
        const size_t want = size_t(std::llround(chip_target));
        while (mono.size() < want) {
            float l[256], r[256];
            const int n = int(std::min<size_t>(256, want - mono.size()));
            chip.generate(l, r, n);
            for (int k = 0; k < n; ++k) mono.push_back((l[k] + r[k]) * 0.5f);
        }
    };
    unsigned slot_port = 0;
    for (size_t i = data_start; i < d.size() && d[i] != 0x66;) {
        if (i == loop_at) loop_chip_sample = int64_t(mono.size());
        const uint8_t c = d[i];
        if (c == 0x67) { i += 7 + (le32(d, i + 3) & 0x7fffffff); continue; }
        if (c == 0x61) advance(d[i + 1] | d[i + 2] << 8);
        else if (c == 0x62) advance(735);
        else if (c == 0x63) advance(882);
        else if (c >= 0x70 && c <= 0x7f) advance((c & 15) + 1);
        else if (c == 0xb5) {
            const unsigned port = d[i + 1] & 0x7f; // 0 data, 1 slot, 2 register
            if (d[i + 1] & 0x80) { std::fprintf(stderr, "second MultiPCM in a log\n"); return 1; }
            chip.write(port, d[i + 2]);
            slot_port = port;
        } else if (c == 0xc3) {
            // bank: offset (little-endian 16 bits) in units of 64 KiB; a 1 MiB bank starts at bit 4
            chip.set_bank(((d[i + 2] | d[i + 3] << 8) >> 4) & 3);
        }
        i += 1 + size_t(length_of(c));
    }
    (void)slot_port;

    // Resample to the output rate: windowed-sinc low-pass at 0.45 of the output rate, evaluated at the
    // fractional input position of each output sample.
    const double step = chip_rate / out_rate;
    const double cutoff = 0.45 * out_rate / chip_rate; // cycles per input sample
    const int half = 24;
    const size_t out_n = size_t(double(mono.size()) / step);
    std::vector<int16_t> pcm(out_n);
    float peak = 0;
    for (size_t k = 0; k < out_n; ++k) {
        const double c = double(k) * step;
        const int64_t base = int64_t(std::floor(c));
        double acc = 0, norm = 0;
        for (int64_t n = base - half + 1; n <= base + half; ++n) {
            const double x = double(n) - c;
            const double w = 0.5 + 0.5 * std::cos(M_PI * x / half); // Hann window
            const double s = x == 0 ? 2 * cutoff : std::sin(2 * M_PI * cutoff * x) / (M_PI * x);
            const double h = s * w;
            norm += h;
            if (n >= 0 && size_t(n) < mono.size()) acc += h * mono[size_t(n)];
        }
        const float v = float(acc / norm * gain * 32768.0); // the chip renders +-1.0
        peak = std::max(peak, std::fabs(v));
        pcm[k] = int16_t(std::lrintf(std::clamp(v, -32768.0f, 32767.0f)));
    }

    FILE *f = std::fopen(argv[2], "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = uint32_t(pcm.size() * 2), rate = uint32_t(out_rate);
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(1); w32(rate); w32(rate * 2); w16(2); w16(16);
    std::fwrite("data", 1, 4, f); w32(bytes);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);

    const long loop_out = loop_chip_sample < 0 ? -1 : long(double(loop_chip_sample) / step);
    std::printf("peak %.0f samples %zu loop %ld\n", peak, pcm.size(), loop_out);
    return 0;
}
