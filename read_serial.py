import serial
import time
import glob

ports = glob.glob('/dev/cu.usbmodem*')
if not ports:
    print("No port found")
    exit(1)

with serial.Serial(ports[0], 9600, timeout=1) as ser:
    end = time.time() + 5.0
    while time.time() < end:
        line = ser.readline()
        if line:
            print(line.decode('utf-8', errors='replace'), end='')
