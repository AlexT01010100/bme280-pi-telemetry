#ifndef MHZ19_H
#define MHZ19_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GPIO14/15 UART on a Raspberry Pi 5 (needs dtparam=uart0=on).
// /dev/serial0 points at the separate debug connector (ttyAMA10) on the Pi 5.
#define MHZ19_DEFAULT_DEVICE     "/dev/ttyAMA0"
#define MHZ19_DEFAULT_TIMEOUT_MS 1000
#define MHZ19_FRAME_LEN          9
#define MHZ19_MAX_PPM            10000   // widest MH-Z19 variant; anything above is a garbled frame

// Return codes
#define MHZ19_OK              0
#define MHZ19_ERR_OPEN       -1   // could not open the serial device (see errno)
#define MHZ19_ERR_CONFIG     -2   // termios configuration failed (see errno)
#define MHZ19_ERR_IO         -3   // write/read/poll failed (see errno)
#define MHZ19_ERR_TIMEOUT    -4   // no complete response before the timeout
#define MHZ19_ERR_FRAME      -5   // response was not a reply to the read command
#define MHZ19_ERR_CHECKSUM   -6   // response checksum mismatch
#define MHZ19_ERR_RANGE      -7   // decoded value outside 0..MHZ19_MAX_PPM
#define MHZ19_ERR_NOT_INIT   -8   // mhz19_read() called before mhz19_init()

// Open the serial device and configure it for 9600 8N1 raw mode.
// device may be NULL to use MHZ19_DEFAULT_DEVICE; timeout_ms 0 uses MHZ19_DEFAULT_TIMEOUT_MS.
int mhz19_init(const char *device, uint32_t timeout_ms);

// Send the "read CO2" command (0x86) and wait for the 9-byte reply.
int mhz19_read(int *ppm);

// Validate a 9-byte reply to command 0x86 and decode it. No I/O, so it can be tested directly.
int mhz19_parse(const uint8_t frame[MHZ19_FRAME_LEN], int *ppm);

// Checksum over bytes 1..7, as defined by the MH-Z19 datasheet.
uint8_t mhz19_checksum(const uint8_t frame[MHZ19_FRAME_LEN]);

void mhz19_close(void);

#ifdef __cplusplus
}
#endif

#endif
