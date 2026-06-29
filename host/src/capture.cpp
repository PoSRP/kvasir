#include "frame_parser.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char* argv[])
{
    const char* port   = argc > 1 ? argv[1] : "/dev/ttyACM0";
    const char* output = argc > 2 ? argv[2] : "output.bin";

    int fd = open(port, O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    termios tty{};
    tcgetattr(fd, &tty);
    cfmakeraw(&tty);
    tcsetattr(fd, TCSANOW, &tty);

    FILE* out = fopen(output, "wb");
    if (!out) {
        perror("fopen");
        close(fd);
        return 1;
    }

    FrameParser parser;
    uint8_t     buf[4096];
    size_t      interval_bytes   = 0;
    size_t      interval_samples = 0;
    auto        t0               = std::chrono::steady_clock::now();

    fprintf(stderr, "Capturing %s → %s\n", port, output);

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        interval_bytes += n;
        for (const auto& f : parser.feed({buf, size_t(n)})) {
            interval_samples += f.samples.size();
            fwrite(f.samples.data(), sizeof(uint16_t), f.samples.size(), out);
        }

        auto   now     = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - t0).count();
        if (elapsed >= 1.0) {
            fprintf(stderr, "%.2f MB/s  %.0f kSa/s  drops: %u\n", interval_bytes / elapsed / 1e6,
                    interval_samples / elapsed / 1e3, parser.seq_drops());
            interval_bytes = interval_samples = 0;
            t0                                = now;
        }
    }

    fprintf(stderr, "done. drops: %u\n", parser.seq_drops());
    fclose(out);
    close(fd);
    return 0;
}
