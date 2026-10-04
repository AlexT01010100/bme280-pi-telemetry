// Unit tests for calibration parsing and compensation maths - no hardware needed.
// Run with: make test
//
// The driver keeps these functions static, so the test includes the source
// directly (with its CLI main() compiled out) instead of linking libbme280.so.
#define BME280_NO_MAIN
#include "../bme280_spi.c"

#include <math.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if(!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while(0)

#define CHECK_NEAR(actual, expected, tol, what) \
    CHECK(fabs((actual) - (expected)) <= (tol), "%s = %.4f, expected %.4f (+/- %g)", what, (double)(actual), (double)(expected), (double)(tol))

static void put16(uint8_t *p, int v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

// Calibration from the Bosch BMP280 datasheet worked example (section 3.12),
// which shares the BME280's temperature/pressure registers, plus typical BME280
// humidity coefficients.
static void load_example_calibration(int h4, int h5) {
    uint8_t buf[26] = {0}, buf_h[7] = {0};
    const int tp[12] = { 27504, 26435, -1000, 36477, -10685, 3024, 2855, 140, -7, 15500, -14600, 6000 };
    for(int i = 0; i < 12; i++) put16(&buf[2 * i], tp[i]);
    buf[25] = 75;                                   // H1
    put16(&buf_h[0], 362);                          // H2
    buf_h[2] = 0;                                   // H3
    buf_h[3] = (uint8_t)(h4 >> 4);                  // 0xE4: H4[11:4]
    buf_h[4] = (uint8_t)((h4 & 0x0F) | ((h5 & 0x0F) << 4)); // 0xE5: H5[3:0] | H4[3:0]
    buf_h[5] = (uint8_t)(h5 >> 4);                  // 0xE6: H5[11:4]
    buf_h[6] = 30;                                  // H6
    parse_calibration(buf, buf_h);
}

// ---- Floating-point reference: datasheet section 8.1, independent of the integer code ----
static double ref_t_fine;

static double ref_temp(int32_t adc_T) {
    double var1 = (adc_T / 16384.0 - calib.dig_T1 / 1024.0) * calib.dig_T2;
    double d = adc_T / 131072.0 - calib.dig_T1 / 8192.0;
    double var2 = d * d * calib.dig_T3;
    ref_t_fine = var1 + var2;
    return ref_t_fine / 5120.0;
}

static double ref_pressure_hpa(int32_t adc_P) {
    double var1 = ref_t_fine / 2.0 - 64000.0;
    double var2 = var1 * var1 * calib.dig_P6 / 32768.0;
    var2 = var2 + var1 * calib.dig_P5 * 2.0;
    var2 = var2 / 4.0 + calib.dig_P4 * 65536.0;
    var1 = (calib.dig_P3 * var1 * var1 / 524288.0 + calib.dig_P2 * var1) / 524288.0;
    var1 = (1.0 + var1 / 32768.0) * calib.dig_P1;
    if(var1 == 0.0) return 0;
    double p = 1048576.0 - adc_P;
    p = (p - var2 / 4096.0) * 6250.0 / var1;
    var1 = calib.dig_P9 * p * p / 2147483648.0;
    var2 = p * calib.dig_P8 / 32768.0;
    return (p + (var1 + var2 + calib.dig_P7) / 16.0) / 100.0;
}

static double ref_humidity(int32_t adc_H) {
    double h = ref_t_fine - 76800.0;
    h = (adc_H - (calib.dig_H4 * 64.0 + calib.dig_H5 / 16384.0 * h)) *
        (calib.dig_H2 / 65536.0 * (1.0 + calib.dig_H6 / 67108864.0 * h * (1.0 + calib.dig_H3 / 67108864.0 * h)));
    h = h * (1.0 - calib.dig_H1 * h / 524288.0);
    return h < 0 ? 0 : h > 100 ? 100 : h;
}

// ---- Tests ----
static void test_parse_little_endian_and_signed(void) {
    load_example_calibration(313, 50);
    CHECK(calib.dig_T1 == 27504, "dig_T1 = %u", calib.dig_T1);
    CHECK(calib.dig_T3 == -1000, "dig_T3 = %d", calib.dig_T3);
    CHECK(calib.dig_P2 == -10685, "dig_P2 = %d", calib.dig_P2);
    CHECK(calib.dig_P9 == 6000, "dig_P9 = %d", calib.dig_P9);
    CHECK(calib.dig_H1 == 75 && calib.dig_H2 == 362 && calib.dig_H6 == 30, "H1/H2/H6 = %u/%d/%d",
          calib.dig_H1, calib.dig_H2, calib.dig_H6);
    CHECK(calib.dig_H4 == 313, "dig_H4 = %d, expected 313", calib.dig_H4);
    CHECK(calib.dig_H5 == 50, "dig_H5 = %d, expected 50", calib.dig_H5);
}

// Regression test for the H4/H5 sign bug: these are signed 12-bit values and
// used to be assembled from unsigned bytes (e.g. -22 decoded as 4074).
static void test_parse_negative_h4_h5(void) {
    load_example_calibration(-22, -300);
    CHECK(calib.dig_H4 == -22, "dig_H4 = %d, expected -22", calib.dig_H4);
    CHECK(calib.dig_H5 == -300, "dig_H5 = %d, expected -300", calib.dig_H5);

    uint8_t buf[26] = {0}, buf_h[7] = {0};
    buf_h[6] = 0xF6;
    parse_calibration(buf, buf_h);
    CHECK(calib.dig_H6 == -10, "dig_H6 = %d, expected -10", calib.dig_H6);
}

// Bosch worked example: adc_T = 519888 -> t_fine = 128422, T = 25.08 C;
// adc_P = 415148 -> p = 100653.27 Pa.
static void test_datasheet_example(void) {
    load_example_calibration(313, 50);
    double t = compensate_temp(519888);
    CHECK(t_fine == 128422, "t_fine = %d, expected 128422", t_fine);
    CHECK_NEAR(t, 25.08, 0.001, "temperature");
    CHECK_NEAR(compensate_pressure(415148), 1006.5327, 0.005, "pressure (hPa)");
}

// Across the sensor's whole operating range, the integer implementation must
// agree with the datasheet's floating-point formulas to within its resolution.
static void test_matches_float_reference(void) {
    load_example_calibration(313, 50);
    double worst_t = 0, worst_p = 0, worst_h = 0;
    int n = 0;
    for(int32_t adc_T = 380000; adc_T <= 620000; adc_T += 4000) {
        double t = compensate_temp(adc_T);
        double t_ref = ref_temp(adc_T);
        if(t_ref < -40 || t_ref > 85) continue;
        worst_t = fmax(worst_t, fabs(t - t_ref));

        for(int32_t adc_P = 250000; adc_P <= 600000; adc_P += 5000) {
            double p_ref = ref_pressure_hpa(adc_P);
            if(p_ref < 300 || p_ref > 1100) continue;
            worst_p = fmax(worst_p, fabs(compensate_pressure(adc_P) - p_ref));
            n++;
        }
        for(int32_t adc_H = 15000; adc_H <= 50000; adc_H += 500) {
            double h_ref = ref_humidity(adc_H);
            if(h_ref <= 0 || h_ref >= 100) continue;   // clamp edges are tested separately
            worst_h = fmax(worst_h, fabs(compensate_humidity(adc_H) - h_ref));
            n++;
        }
    }
    printf("  float reference: %d points, worst error T=%.4f C  P=%.4f hPa  H=%.4f %%\n", n, worst_t, worst_p, worst_h);
    CHECK(n > 1000, "only %d points in range - sweep bounds are wrong", n);
    CHECK(worst_t <= 0.01, "temperature differs from reference by %.4f C", worst_t);
    CHECK(worst_p <= 0.01, "pressure differs from reference by %.4f hPa", worst_p);
    CHECK(worst_h <= 0.01, "humidity differs from reference by %.4f %%", worst_h);
}

static void test_humidity_clamped(void) {
    load_example_calibration(313, 50);
    compensate_temp(519888);
    CHECK(compensate_humidity(0) == 0.0, "humidity at adc_H=0 should clamp to 0");
    CHECK(compensate_humidity(65535) == 100.0, "humidity at adc_H=65535 should clamp to 100, got %.2f",
          compensate_humidity(65535));
}

static void test_read_requires_init(void) {
    bme280_reading r;
    CHECK(bme280_read(&r) == BME280_ERR_NOT_INIT, "bme280_read before init should fail");
    CHECK(bme280_init("/dev/does-not-exist", 0) == BME280_ERR_OPEN, "init on missing device should fail");
}

int main(void) {
    test_parse_little_endian_and_signed();
    test_parse_negative_h4_h5();
    test_datasheet_example();
    test_matches_float_reference();
    test_humidity_clamped();
    test_read_requires_init();

    if(failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all compensation tests passed\n");
    return 0;
}
