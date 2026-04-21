import smbus2, time, gpiod, traceback
from gpiod.line import Direction, Value

print("Starting touch test on I2C bus 3...")
try:
    chip = gpiod.Chip("/dev/gpiochip0")
    # Reset TP_RST (GPIO 17)
    rst = chip.request_lines(config={17: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE)}, consumer="tr")
    
    rst.set_value(17, Value.INACTIVE) # 0
    time.sleep(0.1)
    rst.set_value(17, Value.ACTIVE) # 1
    time.sleep(0.2)

    bus = smbus2.SMBus(3)
    
    # Initialize Point mode
    bus.write_byte_data(0x15, 0xFE, 0x01)
    bus.write_byte_data(0x15, 0xFA, 0x41)

    print("--- TOUCH READY: TOUCH THE SCREEN --- (Ctrl+C to stop)")
    while True:
        try:
            data = bus.read_i2c_block_data(0x15, 0x01, 6)
            if data[1] > 0 or data[0] > 0: # points or gesture
                x = ((data[2] & 0x0F) << 8) | data[3]
                y = ((data[4] & 0x0F) << 8) | data[5]
                print(f"Touch activity! points={data[1]}, X={x}, Y={y}, gesture={data[0]}")
        except OSError:
            pass
        time.sleep(0.05)

except KeyboardInterrupt:
    print("Exiting test.")
except Exception as e:
    traceback.print_exc()
finally:
    try: rst.release()
    except: pass
