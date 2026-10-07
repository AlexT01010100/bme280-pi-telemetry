import serial, time

# GPIO14/15 UART on a Pi 5 is ttyAMA0; serial0 points at ttyAMA10, the debug connector
ser = serial.Serial('/dev/ttyAMA0', 9600, timeout=2)
time.sleep(3)  # sensor needs warm-up before readings are valid

cmd = bytes([0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79])
ser.write(cmd)
resp = ser.read(9)

if len(resp) == 9 and resp[0] == 0xFF and resp[1] == 0x86:
    co2 = resp[2] * 256 + resp[3]
    print(f"CO2: {co2} ppm")
else:
    print("bad response:", resp.hex())