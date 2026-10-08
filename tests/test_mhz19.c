// Unit tests for the MH-Z19 driver - no hardware needed.
// Run with: make test
//
// Frame decoding is tested directly. The serial path (termios setup, write,
// poll/read with timeout, resync) runs against a pseudo-terminal, with a thread
// on the other end playing the sensor.
#define _GNU_SOURCE   // posix_openpt, ptsname_r
#define MHZ19_NO_MAIN
#include "../mhz19_uart.c"

#include <pthread.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if(!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while(0)

// A real response: 0x05B5 = 1461 ppm
static const uint8_t GOOD[MHZ19_FRAME_LEN] = { 0xFF, 0x86, 0x05, 0xB5, 0x40, 0x00, 0x00, 0x00, 0x80 };

// ---------------- Frame decoding ----------------
static void test_checksum(void) {
    CHECK(mhz19_checksum(READ_CMD) == 0x79, "read command checksum = 0x%02X, expected 0x79", mhz19_checksum(READ_CMD));
    CHECK(mhz19_checksum(GOOD) == 0x80, "response checksum = 0x%02X, expected 0x80", mhz19_checksum(GOOD));
}

static void test_parse(void) {
    int ppm = -1;
    CHECK(mhz19_parse(GOOD, &ppm) == MHZ19_OK && ppm == 1461, "good frame -> %d ppm", ppm);

    uint8_t f[MHZ19_FRAME_LEN];
    memcpy(f, GOOD, sizeof f);
    f[8] ^= 0x01;
    CHECK(mhz19_parse(f, &ppm) == MHZ19_ERR_CHECKSUM, "corrupt checksum accepted");

    memcpy(f, GOOD, sizeof f);
    f[1] = 0x99;
    f[8] = mhz19_checksum(f);
    CHECK(mhz19_parse(f, &ppm) == MHZ19_ERR_FRAME, "reply to another command accepted");

    uint8_t big[MHZ19_FRAME_LEN] = { 0xFF, 0x86, 0xFF, 0xFF, 0, 0, 0, 0, 0 };
    big[8] = mhz19_checksum(big);
    CHECK(mhz19_parse(big, &ppm) == MHZ19_ERR_RANGE, "65535 ppm accepted");

    uint8_t zero[MHZ19_FRAME_LEN] = { 0xFF, 0x86, 0, 0, 0, 0, 0, 0, 0 };
    zero[8] = mhz19_checksum(zero);
    CHECK(mhz19_parse(zero, &ppm) == MHZ19_OK && ppm == 0, "0 ppm rejected");
}

// ---------------- Serial path over a pty ----------------
typedef struct {
    int master;
    const uint8_t *reply;   // sent once the full command has arrived
    size_t reply_len;
    size_t split;           // if non-zero, send the reply in two writes split here
    int got_cmd;            // set when the 9 bytes received were exactly READ_CMD
} fake_sensor;

static void sensor_send(int fd, const uint8_t *buf, size_t len) {
    if(write(fd, buf, len) != (ssize_t)len) printf("fake sensor: short write\n");
}

static void *sensor_thread(void *arg) {
    fake_sensor *s = arg;
    uint8_t cmd[MHZ19_FRAME_LEN];
    size_t have = 0;
    while(have < sizeof cmd) {
        ssize_t n = read(s->master, cmd + have, sizeof cmd - have);
        if(n <= 0) return NULL;   // driver closed the port without sending
        have += (size_t)n;
    }
    s->got_cmd = memcmp(cmd, READ_CMD, sizeof cmd) == 0;
    if(s->split) {
        sensor_send(s->master, s->reply, s->split);
        usleep(50 * 1000);
        sensor_send(s->master, s->reply + s->split, s->reply_len - s->split);
    } else if(s->reply_len) {
        sensor_send(s->master, s->reply, s->reply_len);
    }
    return NULL;
}

// One request against a fake sensor that answers with `reply`. Returns mhz19_read()'s code.
static int exchange(const uint8_t *reply, size_t len, size_t split, int *ppm, int *got_cmd) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    char name[64];
    if(master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 || ptsname_r(master, name, sizeof name) != 0) {
        perror("pty");
        exit(2);
    }
    int rc = mhz19_init(name, 300);
    CHECK(rc == MHZ19_OK, "mhz19_init(%s) = %d", name, rc);

    fake_sensor s = { master, reply, len, split, 0 };
    pthread_t t;
    pthread_create(&t, NULL, sensor_thread, &s);
    rc = mhz19_read(ppm);
    mhz19_close();
    pthread_join(t, NULL);
    close(master);
    if(got_cmd) *got_cmd = s.got_cmd;
    return rc;
}

static void test_serial_round_trip(void) {
    int ppm = -1, got_cmd = 0;
    int rc = exchange(GOOD, sizeof GOOD, 0, &ppm, &got_cmd);
    CHECK(got_cmd, "sensor did not receive the read command");
    CHECK(rc == MHZ19_OK && ppm == 1461, "round trip: rc=%d ppm=%d", rc, ppm);
}

static void test_serial_split_reply(void) {
    int ppm = -1;
    int rc = exchange(GOOD, sizeof GOOD, 4, &ppm, NULL);
    CHECK(rc == MHZ19_OK && ppm == 1461, "reply in two chunks: rc=%d ppm=%d", rc, ppm);
}

static void test_serial_resyncs_on_leading_garbage(void) {
    uint8_t noisy[3 + MHZ19_FRAME_LEN] = { 0x00, 0x42, 0x13 };
    memcpy(noisy + 3, GOOD, sizeof GOOD);
    int ppm = -1;
    int rc = exchange(noisy, sizeof noisy, 0, &ppm, NULL);
    CHECK(rc == MHZ19_OK && ppm == 1461, "leading garbage: rc=%d ppm=%d", rc, ppm);
}

static void test_serial_timeouts(void) {
    int ppm = -1;
    int64_t start = now_ms();
    int rc = exchange(NULL, 0, 0, &ppm, NULL);
    int64_t took = now_ms() - start;
    CHECK(rc == MHZ19_ERR_TIMEOUT, "no reply: rc=%d, expected timeout", rc);
    CHECK(took >= 250 && took < 2000, "no reply took %lld ms, expected ~300", (long long)took);

    rc = exchange(GOOD, 5, 0, &ppm, NULL);
    CHECK(rc == MHZ19_ERR_TIMEOUT, "truncated reply: rc=%d, expected timeout", rc);
}

static void test_serial_bad_checksum(void) {
    uint8_t bad[MHZ19_FRAME_LEN];
    memcpy(bad, GOOD, sizeof bad);
    bad[3] ^= 0x10;   // one flipped bit on the wire
    int ppm = -1;
    int rc = exchange(bad, sizeof bad, 0, &ppm, NULL);
    CHECK(rc == MHZ19_ERR_CHECKSUM, "flipped bit: rc=%d, expected checksum error", rc);
}

static void test_api_errors(void) {
    int ppm;
    mhz19_close();
    CHECK(mhz19_read(&ppm) == MHZ19_ERR_NOT_INIT, "mhz19_read before init should fail");
    CHECK(mhz19_init("/dev/does-not-exist", 0) == MHZ19_ERR_OPEN, "init on missing device should fail");
    CHECK(errno == ENOENT, "errno = %d, expected ENOENT", errno);
}

int main(void) {
    test_checksum();
    test_parse();
    test_serial_round_trip();
    test_serial_split_reply();
    test_serial_resyncs_on_leading_garbage();
    test_serial_timeouts();
    test_serial_bad_checksum();
    test_api_errors();

    if(failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All MH-Z19 tests passed\n");
    return 0;
}
