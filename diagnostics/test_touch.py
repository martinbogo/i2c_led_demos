import smbus2
import time

bus = smbus2.SMBus(1)
while True:
    try:
        data = bus.read_i2c_block_data(0x15, 0x01, 6)
        if data[1] > 0:
            print("TOUCH DETECTED:", data)
    except OSError as e:
        # Sleep state
        pass
    except Exception as e:
        print("ERROR:", e)
    time.sleep(0.05)
