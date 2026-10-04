#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

int main() {
    const char *device = "/dev/spidev0.1";
    uint8_t mode = 0;           // SPI mode 0
    uint8_t bits = 8;           // 8 bits per word
    uint32_t speed = 100000; // 100 kHz

    // Open SPI device
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("Failed to open SPI device");
        return 1;
    }

    // Configure SPI
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("Failed to configure SPI");
        close(fd);
        return 1;
    }

    // Prepare SPI transfer
    // MSB=1 for read as per BME280 datasheet
    uint8_t tx[] = {0xD0 | 0x80, 0x00};
    uint8_t rx[2] = {0};

    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = 2,
        .speed_hz = speed,
        .bits_per_word = bits,
        .cs_change = 0,
    };

    // Perform SPI transfer
    if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 1) {
        perror("Failed to send SPI message");
        close(fd);
        return 1;
    }

    printf("BME280 Chip ID: 0x%02X\n", rx[1]);

    close(fd);
    return 0;
}
