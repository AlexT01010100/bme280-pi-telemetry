#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#define SPI_DEVICE "/dev/spidev0.0"
#define SPI_SPEED 500000

// Registers
#define REG_ID 0xD0
#define REG_CTRL_HUM 0xF2
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG 0xF5
#define REG_PRESS_MSB 0xF7

int fd;
int32_t t_fine;

// Calibration struct
typedef struct {
    uint16_t dig_T1; int16_t dig_T2, dig_T3;
    uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t dig_H1; int16_t dig_H2; uint8_t dig_H3; int16_t dig_H4, dig_H5; int8_t dig_H6;
} bme280_calib;

bme280_calib calib;

// ---------------- SPI Helpers ----------------

// Multi-byte SPI read
int spi_read_bytes(uint8_t reg, uint8_t *buf, size_t len) {
    uint8_t tx[256] = {0};
    uint8_t rx[256] = {0};
    if(len + 1 > sizeof(tx)) return -1;

    tx[0] = reg | 0x80; // MSB=1 for read

    struct spi_ioc_transfer tr = {0};
    tr.tx_buf = (uint64_t)(uintptr_t)tx;
    tr.rx_buf = (uint64_t)(uintptr_t)rx;
    tr.len = len + 1;
    tr.speed_hz = SPI_SPEED;
    tr.bits_per_word = 8;

    if(ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 1) return -1;

    memcpy(buf, rx + 1, len);
    return 0;
}

// Single byte SPI write
int spi_write_byte(uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { reg & 0x7F, val };
    struct spi_ioc_transfer tr = {0};
    tr.tx_buf = (uint64_t)(uintptr_t)tx;
    tr.rx_buf = 0;
    tr.len = 2;
    tr.speed_hz = SPI_SPEED;
    tr.bits_per_word = 8;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

// ---------------- Calibration ----------------
int read_calibration() {
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
    calib.dig_H2 = (buf_h[1]<<8)|buf_h[0];
    calib.dig_H3 = buf_h[2];
    calib.dig_H4 = (buf_h[3]<<4) | (buf_h[4] & 0x0F);
    calib.dig_H5 = (buf_h[5]<<4) | (buf_h[4] >> 4);
    calib.dig_H6 = (int8_t)buf_h[6];
    return 0;
}

// ---------------- Compensation ----------------
double compensate_temp(int32_t adc_T) {
    int32_t var1 = ((((adc_T>>3) - ((int32_t)calib.dig_T1<<1))) * calib.dig_T2) >> 11;
    int32_t var2 = (((((adc_T>>4) - calib.dig_T1) * ((adc_T>>4) - calib.dig_T1)) >> 12) * calib.dig_T3) >> 14;
    t_fine = var1 + var2;
    return ((t_fine * 5 + 128) >> 8) / 100.0;
}

double compensate_pressure(int32_t adc_P) {
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

double compensate_humidity(int32_t adc_H) {
    int32_t v_x1_u32r = t_fine - 76800;
    v_x1_u32r = (((((adc_H << 14) - (calib.dig_H4 << 20) - (calib.dig_H5 * v_x1_u32r)) + 16384) >> 15) *
                 (((((((v_x1_u32r * calib.dig_H6) >> 10) * (((v_x1_u32r * calib.dig_H3) >> 11) + 32768)) >> 10) + 2097152) * calib.dig_H2 + 8192) >> 14));
    v_x1_u32r = v_x1_u32r - (((((v_x1_u32r>>15) * (v_x1_u32r>>15))>>7) * calib.dig_H1)>>4);
    if(v_x1_u32r < 0) v_x1_u32r = 0;
    if(v_x1_u32r > 419430400) v_x1_u32r = 419430400;
    return (double)(v_x1_u32r>>12)/1024.0;
}

// ---------------- Main ----------------
int main() {
    fd = open(SPI_DEVICE, O_RDWR);
    if(fd < 0) { perror("SPI open"); return 1; }

    uint8_t mode = SPI_MODE_0, bits = 8;
    uint32_t speed = SPI_SPEED;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    uint8_t chip_id;
    if(spi_read_bytes(REG_ID, &chip_id, 1) < 0) { perror("SPI read"); return 1; }
    printf("BME280 Chip ID: 0x%02X\n", chip_id);
    if(chip_id != 0x60) { printf("Unexpected chip ID\n"); return 1; }

    if(read_calibration() < 0) { perror("Calibration"); return 1; }

    // Configure sensor
    spi_write_byte(REG_CTRL_HUM, 0x01);
    spi_write_byte(REG_CTRL_MEAS, 0x27);

    sleep(1);

    uint8_t data[8];
    if(spi_read_bytes(REG_PRESS_MSB, data, 8) < 0) { perror("SPI read data"); return 1; }

    int32_t adc_P = (data[0]<<12) | (data[1]<<4) | (data[2]>>4);
    int32_t adc_T = (data[3]<<12) | (data[4]<<4) | (data[5]>>4);
    int32_t adc_H = (data[6]<<8) | data[7];

    double T = compensate_temp(adc_T);
    double P = compensate_pressure(adc_P);
    double H = compensate_humidity(adc_H);

    printf("Temperature: %.2f °C\nPressure: %.2f hPa\nHumidity: %.2f %%\n", T, P, H);

    close(fd);
    return 0;
}
