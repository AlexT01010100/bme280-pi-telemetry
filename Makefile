CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

.PHONY: all clean test

all: bme280_spi libbme280.so

# Standalone CLI: ./bme280_spi [/dev/spidev0.0]
bme280_spi: bme280_spi.c bme280.h
	$(CC) $(CFLAGS) -o $@ bme280_spi.c

# Shared library loaded by the Python service via ctypes
libbme280.so: bme280_spi.c bme280.h
	$(CC) $(CFLAGS) -fPIC -shared -DBME280_NO_MAIN -o $@ bme280_spi.c

# Hardware-free unit tests for calibration parsing and compensation
tests/test_compensation: tests/test_compensation.c bme280_spi.c bme280.h
	$(CC) $(CFLAGS) -o $@ tests/test_compensation.c -lm

test: tests/test_compensation
	./tests/test_compensation

clean:
	rm -f bme280_spi libbme280.so tests/test_compensation
