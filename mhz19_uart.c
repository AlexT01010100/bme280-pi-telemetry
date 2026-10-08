// MH-Z19 CO2 sensor driver over a Linux serial port (9600 8N1, raw mode).
#define _DEFAULT_SOURCE   // cfmakeraw, CRTSCTS
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "mhz19.h"

#define CMD_READ_CO2 0x86

static int fd = -1;
static uint32_t timeout_ms = MHZ19_DEFAULT_TIMEOUT_MS;

static const uint8_t READ_CMD[MHZ19_FRAME_LEN] = { 0xFF, 0x01, CMD_READ_CO2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };

// Close without clobbering the errno the caller will report.
static int fail(int rc) {
    int saved = errno;
    mhz19_close();
    errno = saved;
    return rc;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------------- Frame ----------------
uint8_t mhz19_checksum(const uint8_t frame[MHZ19_FRAME_LEN]) {
    uint8_t sum = 0;
    for(int i = 1; i < 8; i++) sum += frame[i];
    return (uint8_t)(0xFF - sum + 1);
}

int mhz19_parse(const uint8_t frame[MHZ19_FRAME_LEN], int *ppm) {
    if(frame[0] != 0xFF || frame[1] != CMD_READ_CO2) return MHZ19_ERR_FRAME;
    if(frame[8] != mhz19_checksum(frame)) return MHZ19_ERR_CHECKSUM;
    int value = (frame[2] << 8) | frame[3];
    if(value > MHZ19_MAX_PPM) return MHZ19_ERR_RANGE;
    *ppm = value;
    return MHZ19_OK;
}

// ---------------- Serial I/O ----------------
static int write_all(const uint8_t *buf, size_t len) {
    while(len > 0) {
        ssize_t n = write(fd, buf, len);
        if(n < 0) {
            if(errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

// Collect one 9-byte frame before the deadline. Bytes ahead of the 0xFF start
// byte are dropped, so line noise or a half frame left over from an earlier
// timeout can't shift every field by one.
static int read_frame(uint8_t frame[MHZ19_FRAME_LEN]) {
    size_t have = 0;
    const int64_t deadline = now_ms() + timeout_ms;

    while(have < MHZ19_FRAME_LEN) {
        int64_t left = deadline - now_ms();
        if(left <= 0) return MHZ19_ERR_TIMEOUT;

        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, (int)left);
        if(r < 0) {
            if(errno == EINTR) continue;
            return MHZ19_ERR_IO;
        }
        if(r == 0) return MHZ19_ERR_TIMEOUT;
        if(p.revents & (POLLERR | POLLNVAL)) return MHZ19_ERR_IO;

        ssize_t n = read(fd, frame + have, MHZ19_FRAME_LEN - have);
        if(n < 0) {
            if(errno == EINTR || errno == EAGAIN) continue;
            return MHZ19_ERR_IO;
        }
        if(n == 0) {
            if(p.revents & POLLHUP) return MHZ19_ERR_IO;   // device went away
            continue;
        }
        have += (size_t)n;

        size_t skip = 0;
        while(skip < have && frame[skip] != 0xFF) skip++;
        if(skip) {
            memmove(frame, frame + skip, have - skip);
            have -= skip;
        }
    }
    return MHZ19_OK;
}

// ---------------- Public API ----------------
int mhz19_init(const char *device, uint32_t timeout) {
    if(fd >= 0) mhz19_close();
    if(!device) device = MHZ19_DEFAULT_DEVICE;
    timeout_ms = timeout ? timeout : MHZ19_DEFAULT_TIMEOUT_MS;

    fd = open(device, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if(fd < 0) return MHZ19_ERR_OPEN;

    struct termios tio;
    if(tcgetattr(fd, &tio) < 0) return fail(MHZ19_ERR_CONFIG);
    cfmakeraw(&tio);                                   // no echo, no line editing, no CR/LF translation
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;               // 8N1, ignore modem control lines
    tio.c_cc[VMIN] = 0;                                // read() never blocks; poll() handles waiting
    tio.c_cc[VTIME] = 0;
    if(cfsetispeed(&tio, B9600) < 0 || cfsetospeed(&tio, B9600) < 0 ||
       tcsetattr(fd, TCSANOW, &tio) < 0) {
        return fail(MHZ19_ERR_CONFIG);
    }
    tcflush(fd, TCIOFLUSH);
    return MHZ19_OK;
}

int mhz19_read(int *ppm) {
    if(fd < 0) return MHZ19_ERR_NOT_INIT;

    // Drop anything left over from an earlier timed-out request
    tcflush(fd, TCIFLUSH);
    if(write_all(READ_CMD, sizeof READ_CMD) < 0) return MHZ19_ERR_IO;

    uint8_t frame[MHZ19_FRAME_LEN];
    int rc = read_frame(frame);
    if(rc != MHZ19_OK) return rc;
    return mhz19_parse(frame, ppm);
}

void mhz19_close(void) {
    if(fd >= 0) close(fd);
    fd = -1;
}

// ---------------- Main ----------------
// Built as a standalone CLI by default; the shared library build defines MHZ19_NO_MAIN.
#ifndef MHZ19_NO_MAIN
int main(int argc, char **argv) {
    const char *device = argc > 1 ? argv[1] : MHZ19_DEFAULT_DEVICE;

    int rc = mhz19_init(device, MHZ19_DEFAULT_TIMEOUT_MS);
    if(rc == MHZ19_ERR_OPEN) { perror(device); return 1; }
    if(rc != MHZ19_OK) { perror("serial config"); return 1; }

    int ppm;
    rc = mhz19_read(&ppm);
    switch(rc) {
        case MHZ19_OK: printf("CO2: %d ppm\n", ppm); break;
        case MHZ19_ERR_TIMEOUT: printf("No response - check TX/RX wiring and that the UART is enabled\n"); break;
        case MHZ19_ERR_FRAME: printf("Unexpected response\n"); break;
        case MHZ19_ERR_CHECKSUM: printf("Bad checksum\n"); break;
        case MHZ19_ERR_RANGE: printf("Value out of range\n"); break;
        default: perror("serial I/O"); break;
    }
    mhz19_close();
    return rc == MHZ19_OK ? 0 : 1;
}
#endif
