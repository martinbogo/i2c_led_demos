import smbus2, time
bus = smbus2.SMBus(1)
try:
    bus.write_byte_data(0x15, 0xFE, 0x01)
    bus.write_byte_data(0x15, 0xFA, 0x41)
    print("Init ok")
except Exception as e:
    print("Init fail:", e)

for _ in range(50):
    try:
        data = bus.read_i2c_block_data(0x15, 0x01, 6)
        if data[1] > 0 or data[0] > 0:
            print("Touch data:", data)
    except Exception as e:
        print("Read fail:", e)
    time.sleep(0.1)
