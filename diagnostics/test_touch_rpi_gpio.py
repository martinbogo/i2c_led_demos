import smbus2
import time
import RPi.GPIO as GPIO

GPIO.setwarnings(False)
GPIO.setmode(GPIO.BCM)

TP_RST = 17

GPIO.setup(TP_RST, GPIO.OUT)

print("Asserting reset low...")
GPIO.output(TP_RST, GPIO.LOW)
time.sleep(0.01)
print("Releasing reset high...")
GPIO.output(TP_RST, GPIO.HIGH)
time.sleep(0.05)

bus = smbus2.SMBus(1)

try:
    whoami = bus.read_byte_data(0x15, 0xA7)
    print(f"WhoAmI (0xA7) = {hex(whoami)}")
except Exception as e:
    print("Could not read ID:", e)

try:
    bus.write_byte_data(0x15, 0xFE, 0x01)
    bus.write_byte_data(0x15, 0xFA, 0x41)
    print("Configured to point mode.")
except Exception as e:
    print("Config failed:", e)

for i in range(20):
    try:
        data = bus.read_i2c_block_data(0x15, 0x01, 6)
        if data[1] > 0 or data[0] > 0:
            print("Activity:", data)
    except OSError:
        pass
    time.sleep(0.1)

GPIO.cleanup()
