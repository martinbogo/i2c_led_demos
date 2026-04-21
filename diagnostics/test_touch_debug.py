import smbus2, time, gpiod, traceback
from gpiod.line import Direction, Value

print("Starting debug...")
try:
    chip = gpiod.Chip("/dev/gpiochip0")
    # Reset
    rst = chip.request_lines(config={17: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE)}, consumer="tr")
    
    print("Holding reset low...")
    rst.set_value(17, Value.INACTIVE) # 0
    time.sleep(0.1)
    print("Releasing reset (high)...")
    rst.set_value(17, Value.ACTIVE) # 1
    time.sleep(0.2)

    bus = smbus2.SMBus(1)
    
    # check WhoAmI
    try:
        whoami = bus.read_byte_data(0x15, 0xA7)
        print("WhoAmI (0xA7) =", hex(whoami))
        fw = bus.read_byte_data(0x15, 0xA9)
        print("FW Ver (0xA9) =", hex(fw))
    except Exception as e:
        print("Could not read whoami:", e)

    bus.write_byte_data(0x15, 0xFE, 0x01)
    bus.write_byte_data(0x15, 0xFA, 0x41)

    print("Configured. Polling for 3 seconds...")
    for _ in range(30):
        try:
            data = bus.read_i2c_block_data(0x15, 0x01, 6)
            if data[1] > 0 or data[0] > 0:
                print("Activity!", data)
        except OSError:
            pass
        time.sleep(0.1)

except Exception as e:
    traceback.print_exc()
finally:
    try: rst.release()
    except: pass
