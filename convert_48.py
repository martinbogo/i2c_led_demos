import sys

# Dither Array translator restricted entirely to 128x48 boundaries
while True:
    frame_raw = sys.stdin.buffer.read(6144)
    if len(frame_raw) < 6144:
        break
    
    frame = list(frame_raw)
    out = bytearray(768)
    
    for y in range(48):
        for x in range(128):
            old_px = frame[y * 128 + x]
            new_px = 255 if old_px > 127 else 0
            error = old_px - new_px
            
            if new_px > 0:
                out[x + (y // 8) * 128] |= (1 << (y % 8))
                
            if x + 1 < 128:
                frame[y * 128 + x + 1] = min(255, max(0, frame[y * 128 + x + 1] + error * 7 // 16))
            if y + 1 < 48:
                if x > 0:
                    frame[(y + 1) * 128 + x - 1] = min(255, max(0, frame[(y + 1) * 128 + x - 1] + error * 3 // 16))
                frame[(y + 1) * 128 + x] = min(255, max(0, frame[(y + 1) * 128 + x] + error * 5 // 16))
                if x + 1 < 128:
                    frame[(y + 1) * 128 + x + 1] = min(255, max(0, frame[(y + 1) * 128 + x + 1] + error * 1 // 16))
                    
    sys.stdout.buffer.write(out)
