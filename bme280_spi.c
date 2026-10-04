#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "bme280.h"

// Registers
#define REG_ID 0xD0
#define REG_CTRL_HUM 0xF2
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG 0xF5
#define REG_PRESS_MSB 0xF7

static int fd = -1;
static uint32_t spi_speed = BME280_DEFAULT_SPEED;
static uint8_t chip_id;
static int32_t t_fine;

// Calibration struct
typedef struct {
    uint16_t dig_T1; int16_t dig_T2, dig_T3;
    uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t dig_H1; int16_t dig_H2; uint8_t dig_H3; int16_t dig_H4, dig_H5; int8_t dig_H6;
} bme280_calib;

static bme280_calib calib;

// ---------------- SPI Helpers ----------------

// Multi-byte SPI read
static int spi_read_bytes(uint8_t reg, uint8_t *buf, size_t len) {
    uint8_t tx[256] = {0};
    uint8_t rx[256] = {0};
    if(len + 1 > sizeof(tx)) return -1;

    tx[0] = reg | 0x80; // MSB=1 for read

    struct spi_ioc_transfer tr = {0};
    tr.tx_buf = (uint64_t)(uintptr_t)tx;
    tr.rx_buf = (uint64_t)(uintptr_t)rx;
    tr.len = len + 1;
    tr.speed_hz = spi_speed;
    tr.bits_per_word = 8;

    if(ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 1) return -1;

    memcpy(buf, rx + 1, len);
    return 0;
}

// Single byte SPI write
static int spi_write_byte(uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { reg & 0x7F, val };
    struct spi_ioc_transfer tr = {0};
    tr.tx_buf = (uint64_t)(uintptr_t)tx;
    tr.rx_buf = 0;
    tr.len = 2;
    tr.speed_hz = spi_speed;
    tr.bits_per_word = 8;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 1 ? -1 : 0;
}

// ---------------- Calibration ----------------
static int read_calibration(void) {
    uint8_t buf[26];
    if(spi_read_bytes(0x88, buf, 26) < 0) return -1;
    calib.dig_T1 = (buf[1]<<8)|buf[0];
    calib.dig_T2 = (int16_t)((buf[3]<<8)|buf[2]);
    calib.dig_T3 = (int16_t)((buf[5]<<8)|buf[4]);
    calib.dig_P1 = (buf[7]<<8)|buf[6];
    calib.dig_P2 = (int16_t)((buf[9]<<8)|buf[8]);
    calib.dig_P3 = (int16_t)((buf[11]<<8)|buf[10]);
    calib.dig_P4 = (int16_t)((buf[13]<<8)|buf[12]);
    calib.dig_P5 = (int16_t)((buf[15]<<8)|buf[14]);
    calib.dig_P6 = (int16_t)((buf[17]<<8)|buf[16]);
    calib.dig_P7 = (int16_t)((buf[19]<<8)|buf[18]);
    calib.dig_P8 = (int16_t)((buf[21]<<8)|buf[20]);
    calib.dig_P9 = (int16_t)((buf[23]<<8)|buf[22]);
    calib.dig_H1 = buf[25];

    uint8_t buf_h[7];
    if(spi_read_bytes(0xE1, buf_h, 7) < 0) return -1;
    calib.dig_H2 = (int16_t)((buf_h[1]<<8)|buf_h[0]);
    calib.dig_H3 = buf_h[2];
    // H4/H5 are signed 12-bit: the MSB bytes (0xE4, 0xE6) carry the sign
    calib.dig_H4 = (int16_t)(((int8_t)buf_h[3] * 16) | (buf_h[4] & 0x0F));
    calib.dig_H5 = (int16_t)(((int8_t)buf_h[5] * 16) | (buf_h[4] >> 4));
    calib.dig_H6 = (int8_t)buf_h[6];
    return 0;
}

// ---------------- Compensation ----------------
static double compensate_temp(int32_t adc_T) {
    int32_t var1 = ((((adc_T>>3) - ((int32_t)calib.dig_T1<<1))) * calib.dig_T2) >> 11;
    int32_t var2 = (((((adc_T>>4) - calib.dig_T1) * ((adc_T>>4) - calib.dig_T1)) >> 12) * calib.dig_T3) >> 14;
    t_fine = var1 + var2;
    return ((t_fine * 5 + 128) >> 8) / 100.0;
}

static double compensate_pressure(int32_t adc_P) {
    int64_t var1 = t_fine - 128000;
    int64_t var2 = var1 * var1 * calib.dig_P6;
    var2 = var2 + ((var1*calib.dig_P5)<<17);
    var2 = var2 + (((int64_t)calib.dig_P4)<<35);
    var1 = ((var1 * var1 * calib.dig_P3)>>8) + ((var1 * calib.dig_P2)<<12);
    var1 = (((((int64_t)1)<<47)+var1))*calib.dig_P1>>33;
    if(var1 == 0) return 0;
    int64_t p = 1048576 - adc_P;
    p = (((p<<31) - var2)*3125)/var1;
    var1 = (calib.dig_P9 * (p>>13) * (p>>13)) >> 25;
    var2 = (calib.dig_P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + ((int64_t)calib.dig_P7<<4);
    return (double)p/256.0/100.0; // hPa
}

static double compensate_humidity(int32_t adc_H) {
    int32_t v_x1_u32r = t_fine - 76800;
    v_x1_u32r = (((((adc_H << 14) - (calib.dig_H4 << 20) - (calib.dig_H5 * v_x1_u32r)) + 16384) >> 15) *
                 (((((((v_x1_u32r * calib.dig_H6) >> 10) * (((v_x1_u32r * calib.dig_H3) >> 11) + 32768)) >> 10) + 2097152) * calib.dig_H2 + 8192) >> 14));
    v_x1_u32r = v_x1_u32r - (((((v_x1_u32r>>15) * (v_x1_u32r>>15))>>7) * calib.dig_H1)>>4);
    if(v_x1_u32r < 0) v_x1_u32r = 0;
    if(v_x1_u32r > 419430400) v_x1_u32r = 419430400;
    return (double)(v_x1_u32r>>12)/1024.0;
}

// ---------------- Public API ----------------
int bme280_init(const char *device, uint32_t speed_hz) {
    if(fd >= 0) bme280_close();
    if(!device) device = BME280_DEFAULT_DEVICE;
    spi_speed = speed_hz ? speed_hz : BME280_DEFAULT_SPEED;

    fd = open(device, O_RDWR);
    if(fd < 0) return BME280_ERR_OPEN;

    uint8_t mode = SPI_MODE_0, bits = 8;
    if(ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
       ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
       ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed) < 0) {
        bme280_close();
        return BME280_ERR_SPI;
    }

    if(spi_read_bytes(REG_ID, &chip_id, 1) < 0) { bme280_close(); return BME280_ERR_SPI; }
    if(chip_id != 0x60) { bme280_close(); return BME280_ERR_CHIP_ID; }

    if(read_calibration() < 0) { bme280_close(); return BME280_ERR_CALIB; }

    // Configure sensor: humidity x1, then temp x1 / pressure x1 / normal mode
    // (ctrl_hum only takes effect after a write to ctrl_meas)
    if(spi_write_byte(REG_CTRL_HUM, 0x01) < 0 ||
       spi_write_byte(REG_CTRL_MEAS, 0x27) < 0) {
        bme280_close();
        return BME280_ERR_SPI;
    }

    // Let the first conversion complete so an immediate read isn't the reset value
    usleep(50 * 1000);
    return BME280_OK;
}

int bme280_read(bme280_reading *out) {
    if(fd < 0) return BME280_ERR_NOT_INIT;

    uint8_t data[8];
    if(spi_read_bytes(REG_PRESS_MSB, data, 8) < 0) return BME280_ERR_SPI;

    int32_t adc_P = (data[0]<<12) | (data[1]<<4) | (data[2]>>4);
    int32_t adc_T = (data[3]<<12) | (data[4]<<4) | (data[5]>>4);
    int32_t adc_H = (data[6]<<8) | data[7];

    // Temperature must be compensated first: it sets t_fine for the others
    out->temperature_c = compensate_temp(adc_T);
    out->pressure_hpa = compensate_pressure(adc_P);
    out->humidity_pct = compensate_humidity(adc_H);
    return BME280_OK;
}

uint8_t bme280_chip_id(void) {
    return fd >= 0 ? chip_id : 0;
}

void bme280_close(void) {
    if(fd >= 0) close(fd);
    fd = -1;
    chip_id = 0;
}

// ---------------- Main ----------------
// Built as a standalone CLI by default; the shared library build defines BME280_NO_MAIN.
#ifndef BME280_NO_MAIN
int main(int argc, char **argv) {
    const char *device = argc > 1 ? argv[1] : BME280_DEFAULT_DEVICE;

    int rc = bme280_init(device, BME280_DEFAULT_SPEED);
    switch(rc) {
        case BME280_OK: break;
        case BME280_ERR_OPEN: perror("SPI open"); return 1;
        case BME280_ERR_CHIP_ID: printf("Unexpected chip ID\n"); return 1;
        case BME280_ERR_CALIB: perror("Calibration"); return 1;
        default: perror("SPI"); return 1;
    }
    printf("BME280 Chip ID: 0x%02X\n", bme280_chip_id());

    sleep(1);

    bme280_reading r;
    if(bme280_read(&r) < 0) { perror("SPI read data"); bme280_close(); return 1; }

    printf("Temperature: %.2f °C\nPressure: %.2f hPa\nHumidity: %.2f %%\n",
           r.temperature_c, r.pressure_hpa, r.humidity_pct);

    bme280_close();
    return 0;
}
#endif
