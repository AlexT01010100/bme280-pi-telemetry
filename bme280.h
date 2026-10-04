#ifndef BME280_H
#define BME280_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BME280_DEFAULT_DEVICE "/dev/spidev0.0"
#define BME280_DEFAULT_SPEED  500000

// Return codes
#define BME280_OK              0
#define BME280_ERR_OPEN       -1   // could not open the spidev device (see errno)
#define BME280_ERR_SPI        -2   // SPI ioctl failed (see errno)
#define BME280_ERR_CHIP_ID    -3   // chip ID was not 0x60
#define BME280_ERR_CALIB      -4   // failed to read calibration registers
#define BME280_ERR_NOT_INIT   -5   // bme280_read() called before bme280_init()

typedef struct {
    double temperature_c;
    double pressure_hpa;
    double humidity_pct;
} bme280_reading;

// Open the SPI device, verify the chip, load calibration and start normal mode.
// device may be NULL to use BME280_DEFAULT_DEVICE; speed_hz 0 uses BME280_DEFAULT_SPEED.
int bme280_init(const char *device, uint32_t speed_hz);

// Burst-read the latest measurement and apply compensation.
int bme280_read(bme280_reading *out);

// Chip ID read during bme280_init(), or 0 if not initialised.
uint8_t bme280_chip_id(void);

void bme280_close(void);

#ifdef __cplusplus
}
#endif

#endif
