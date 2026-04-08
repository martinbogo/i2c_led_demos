import sys

while True:
    frame_raw = sys.stdin.buffer.read(8192)
    if len(frame_raw) < 8192:
        break
    
    # Cast tuple array locally for floating point pixel mutations
    frame = list(frame_raw)
    out = bytearray(1024)
    
    for y in range(64):
        for x in range(128):
            old_px = frame[y * 128 + x]
            new_px = 255 if old_px > 127 else 0
            
            # Record absolute quantization error
            error = old_px - new_px
            
            # Commit threshold to physical 1-bit boolean matrix
            if new_px > 0:
                out[x + (y // 8) * 128] |= (1 << (y % 8))
                
            # Floyd-Steinberg Error Diffusion Mapping Algorithm
            if x + 1 < 128:
                frame[y * 128 + x + 1] = min(255, max(0, frame[y * 128 + x + 1] + error * 7 // 16))
            if y + 1 < 64:
                if x > 0:
                    frame[(y + 1) * 128 + x - 1] = min(255, max(0, frame[(y + 1) * 128 + x - 1] + error * 3 // 16))
                frame[(y + 1) * 128 + x] = min(255, max(0, frame[(y + 1) * 128 + x] + error * 5 // 16))
                if x + 1 < 128:
                    frame[(y + 1) * 128 + x + 1] = min(255, max(0, frame[(y + 1) * 128 + x + 1] + error * 1 // 16))
                    
    sys.stdout.buffer.write(out)
