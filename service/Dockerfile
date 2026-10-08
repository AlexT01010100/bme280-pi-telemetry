# Build context is the repo root (see docker-compose.yml) so the C driver is available.
FROM python:3.12-slim AS driver
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev make linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile bme280.h bme280_spi.c mhz19.h mhz19_uart.c ./
RUN make libbme280.so libmhz19.so

FROM python:3.12-slim
WORKDIR /app
COPY service/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY --from=driver /src/libbme280.so /src/libmhz19.so /usr/local/lib/
COPY service/app ./app
ENV BME_LIB_PATH=/usr/local/lib/libbme280.so \
    BME_CO2_LIB_PATH=/usr/local/lib/libmhz19.so
EXPOSE 8000
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
