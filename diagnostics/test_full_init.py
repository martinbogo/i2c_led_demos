import sys, time
import badapple_waveshare as lcd

display = lcd.DisplayDriver()
display.init()

print("LCD init done.")
time.sleep(0.5)

import gpiod
from gpiod.line import Direction, Value
import traceback
try:
    chip = gpiod.Chip("/dev/gpiochip0")
    rst = chip.request_lines(config={17: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE)}, consumer="tr")
    rst.set_value(17, Value.INACTIVE)
    time.sleep(0.01)
    rst.set_value(17, Value.ACTIVE)
    time.sleep(0.05)

    import smbus2
    bus = smbus2.SMBus(1)
    try:
        whoami = bus.read_byte_data(0x15, 0xA7)
        print("WhoAmI (0xA7) =", hex(whoami))
        bus.write_byte_data(0x15, 0xFE, 0x01)
        bus.write_byte_data(0x15, 0xFA, 0x41)
        print("Polling touch for 2 seconds...")
        for _ in range(20):
            try:
                data = bus.read_i2c_block_data(0x15, 0x01, 6)
                if data[1] > 0 or data[0] > 0:
                    print("Touch:", data)
            except: pass
            time.sleep(0.1)
    except Exception as e:
        print("Could not read ID:", e)
except Exception as e:
    traceback.print_exc()

