CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

.PHONY: all clean test

all: bme280_spi libbme280.so mhz19 libmhz19.so

# Standalone CLI: ./bme280_spi [/dev/spidev0.0]
bme280_spi: bme280_spi.c bme280.h
	$(CC) $(CFLAGS) -o $@ bme280_spi.c

# Shared library loaded by the Python service via ctypes
libbme280.so: bme280_spi.c bme280.h
	$(CC) $(CFLAGS) -fPIC -shared -DBME280_NO_MAIN -o $@ bme280_spi.c

# Standalone CLI: ./mhz19 [/dev/ttyAMA0]
mhz19: mhz19_uart.c mhz19.h
	$(CC) $(CFLAGS) -o $@ mhz19_uart.c

# Shared library loaded by the Python service via ctypes
libmhz19.so: mhz19_uart.c mhz19.h
	$(CC) $(CFLAGS) -fPIC -shared -DMHZ19_NO_MAIN -o $@ mhz19_uart.c

# Hardware-free unit tests for calibration parsing and compensation
tests/test_compensation: tests/test_compensation.c bme280_spi.c bme280.h
	$(CC) $(CFLAGS) -o $@ tests/test_compensation.c -lm

# Hardware-free tests for MH-Z19 frame decoding and the serial path (over a pty)
tests/test_mhz19: tests/test_mhz19.c mhz19_uart.c mhz19.h
	$(CC) $(CFLAGS) -pthread -o $@ tests/test_mhz19.c

test: tests/test_compensation tests/test_mhz19
	./tests/test_compensation
	./tests/test_mhz19

clean:
	rm -f bme280_spi libbme280.so mhz19 libmhz19.so tests/test_compensation tests/test_mhz19
