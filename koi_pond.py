#!/usr/bin/env python3
# Author  : Martin Bogomolni
# Date    : 2026-04-21
# License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)

"""
koi_pond.py - Interactive photorealistic Koi pond demo for the GC9A01 LCD.
Simulates boid-based fish with photorealistic sprites sliced to morph on kinematics.
"""

import math
import time
import threading
import random
from PIL import Image, ImageDraw

from badapple_waveshare import DisplayDriver, LCD_WIDTH, LCD_HEIGHT

try:
    import smbus2
    import gpiod
    from gpiod.line import Direction, Value
except ImportError:
    pass

class PondAssets:
    def __init__(self):
        self.lily_sprites = [
            self._make_lilypad_sprite(134, 105, notch_angle=-0.2, hue_shift=0),
            self._make_lilypad_sprite(141, 104, notch_angle=0.25, hue_shift=8),
            self._make_lilypad_sprite(128, 102, notch_angle=-0.5, hue_shift=-6),
        ]
        self.bg = self._make_background()

    def _make_background(self):
        img = Image.new("RGB", (LCD_WIDTH, LCD_HEIGHT))
        pixels = []
        cx = LCD_WIDTH / 2.0
        cy = LCD_HEIGHT / 2.0
        max_dist = math.sqrt(cx * cx + cy * cy)

        for y in range(LCD_HEIGHT):
            for x in range(LCD_WIDTH):
                dx = x - cx
                dy = y - cy
                dist = math.sqrt(dx * dx + dy * dy) / max_dist
                ripple = math.sin((x * 0.11) + (y * 0.07)) * 6
                shimmer = math.sin((x * 0.035) - (y * 0.045)) * 4
                vignette = 1.0 - min(1.0, dist * 0.85)

                r = int(18 + 8 * vignette + ripple * 0.2)
                g = int(58 + 36 * vignette + ripple + shimmer)
                b = int(72 + 58 * vignette + ripple * 1.4 + shimmer)

                pixels.append((
                    max(0, min(255, r)),
                    max(0, min(255, g)),
                    max(0, min(255, b)),
                ))

        img.putdata(pixels)

        draw = ImageDraw.Draw(img, "RGBA")
        for _ in range(22):
            x = random.randint(-20, LCD_WIDTH)
            y = random.randint(-20, LCD_HEIGHT)
            radius = random.randint(16, 42)
            color = random.choice([
                (20, 90, 90, 18),
                (12, 70, 74, 22),
                (55, 120, 116, 12),
            ])
            draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)

        for _ in range(12):
            x = random.randint(0, LCD_WIDTH)
            y = random.randint(0, LCD_HEIGHT)
            radius = random.randint(18, 34)
            draw.ellipse(
                (x - radius, y - radius, x + radius, y + radius),
                outline=(150, 210, 205, 20),
                width=1,
            )

        return img

    def _make_lilypad_sprite(self, width, height, notch_angle=0.0, hue_shift=0):
        sprite = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        draw = ImageDraw.Draw(sprite, "RGBA")

        pad_color = (72 + hue_shift, 145 + hue_shift, 76, 255)
        shadow_color = (28, 72, 34, 90)
        vein_color = (170, 220, 140, 110)

        draw.ellipse((6, 6, width - 6, height - 6), fill=shadow_color)
        draw.ellipse((0, 0, width - 10, height - 10), fill=pad_color)
        draw.ellipse((8, 6, width - 18, height - 18), fill=(94 + hue_shift, 170 + hue_shift, 90, 235))

        cx = width * 0.46
        cy = height * 0.44
        notch_len = min(width, height) * 0.55
        notch_spread = 0.28
        p1 = (cx, cy)
        p2 = (
            cx + math.cos(notch_angle - notch_spread) * notch_len,
            cy + math.sin(notch_angle - notch_spread) * notch_len,
        )
        p3 = (
            cx + math.cos(notch_angle + notch_spread) * notch_len,
            cy + math.sin(notch_angle + notch_spread) * notch_len,
        )
        draw.polygon([p1, p2, p3], fill=(0, 0, 0, 0))

        for angle in (-1.3, -0.7, -0.15, 0.45, 1.0):
            ex = cx + math.cos(angle) * (width * 0.32)
            ey = cy + math.sin(angle) * (height * 0.32)
            draw.line((cx, cy, ex, ey), fill=vein_color, width=2)

        draw.ellipse((width * 0.18, height * 0.12, width * 0.48, height * 0.34), fill=(220, 255, 220, 26))
        return sprite

assets = None

class Lilypad:
    def __init__(self, x, y, rot=0, scale=1.0):
        self.pos = [x, y]
        b = random.choice(assets.lily_sprites)
        fin_w = int(b.size[0] * 0.4 * scale)
        fin_h = int(b.size[1] * 0.4 * scale)
        b = b.resize((fin_w, fin_h), Image.Resampling.LANCZOS)
        if rot != 0:
            b = b.rotate(rot, expand=True, resample=Image.Resampling.BICUBIC)
        self.sprite = b

    def draw(self, img_bg):
        w, h = self.sprite.size
        # Paste lilypad on the pond
        px, py = int(self.pos[0] - w//2), int(self.pos[1] - h//2)
        try:
            img_bg.paste(self.sprite, (px, py), self.sprite)
        except ValueError:
            pass

class Koi:
    def __init__(self, x, y):
        self.pos = [x, y]
        self.hiding_state = "normal"
        self.hide_timer = 0
        self.hide_target = None
        self.scared = random.uniform(0, 9.0)
        self.vel = [random.uniform(-1, 1), random.uniform(-1, 1)]
        self.normalize(self.vel)
        self.vel[0] *= 2.0
        self.vel[1] *= 2.0
        
        self.num_chunks = 10
        self.segment_dist = 3.5
        
        # True comma shape, fading from head to tail tip
        self.radii = [7.0, 8.0, 7.5, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0, 0.5]
        self.color = random.choice([(255, 90, 40), (220, 220, 220), (255, 170, 50), (240, 100, 40)])
        
        self.segments = [[x, y] for _ in range(self.num_chunks)]

    def normalize(self, v):
        mag = math.sqrt(v[0]**2 + v[1]**2)
        if mag > 0:
            v[0] /= mag
            v[1] /= mag
        return mag

    def update(self, target=None, flee_points=None, lilypads=None):
        if not hasattr(self, 'hiding_state'):
            self.hiding_state = "normal" # normal, hiding, hidden, returning
            self.hide_timer = 0
            self.hide_target = None
            
        speed = 2.5
        ax, ay = 0, 0
        
        if flee_points and self.hiding_state in ("normal", "returning"):
            # A touch happened! Scatter and hide!
            self.hiding_state = "hiding"
            
            touch_x, touch_y = flee_points[0]
            
            if self.scared <= 1.0:
                # Swim TOWARDS the touch
                self.hide_target = [touch_x + random.uniform(-10, 10), touch_y + random.uniform(-10, 10)]
            elif self.scared < 5.0:
                # Move away, but not off screen
                angle = math.atan2(self.pos[1] - touch_y, self.pos[0] - touch_x) + random.uniform(-0.5, 0.5)
                dist = 60 + self.scared * 10
                hx = self.pos[0] + math.cos(angle) * dist
                hy = self.pos[1] + math.sin(angle) * dist
                # Clamp to screen bounds
                hx = max(20, min(LCD_WIDTH - 20, hx))
                hy = max(20, min(LCD_HEIGHT - 20, hy))
                self.hide_target = [hx, hy]
            else:
                # Very scared (5-9)! Hide under lilypad or offscreen
                if lilypads and random.random() > 0.3:
                    # hide under lilypad
                    pad = random.choice(lilypads)
                    self.hide_target = [pad.pos[0] + random.uniform(-10, 10), pad.pos[1] + random.uniform(-10, 10)]
                else:
                    # hide offscreen
                    angle = math.atan2(self.pos[1] - touch_y, self.pos[0] - touch_x) + random.uniform(-0.5, 0.5)
                    dist = (LCD_WIDTH / 2.0) + (self.scared / 9.0) * (LCD_WIDTH)
                    self.hide_target = [self.pos[0] + math.cos(angle)*dist, self.pos[1] + math.sin(angle)*dist]
                
        if self.hiding_state == "normal":
            # Basic wandering behavior
            wander_angle = random.uniform(-0.4, 0.4)
            current_angle = math.atan2(self.vel[1], self.vel[0])
            new_angle = current_angle + wander_angle
            
            # Keep them in bounds gently
            cx, cy = LCD_WIDTH/2, LCD_HEIGHT/2
            dx, dy = cx - self.pos[0], cy - self.pos[1]
            dist_center = math.sqrt(dx**2 + dy**2)
            if dist_center > LCD_WIDTH/2 - 20:
                ax += dx * 0.05
                ay += dy * 0.05
                
            self.vel[0] = math.cos(new_angle) * speed + ax
            self.vel[1] = math.sin(new_angle) * speed + ay
            
        elif self.hiding_state == "hiding":
            speed = 3.0 + (self.scared / 9.0) * 5.0 # speed proportional to scared
            dx = self.hide_target[0] - self.pos[0]
            dy = self.hide_target[1] - self.pos[1]
            dist = math.sqrt(dx**2 + dy**2)
            
            if dist < 20:
                # reached hiding spot!
                self.hiding_state = "hidden"
                self.hide_timer = time.time() + (self.scared / 9.0) * 10.0
                # Do not stop completely, just slow down
                speed = 0.6
            else:
                # Steer towards target
                tx, ty = (dx/dist), (dy/dist)
                
                # Blend current velocity with target direction
                current_angle = math.atan2(self.vel[1], self.vel[0])
                target_angle = math.atan2(ty, tx)
                
                # Turn quickly towards target
                diff = (target_angle - current_angle + math.pi) % (2*math.pi) - math.pi
                new_angle = current_angle + diff * 0.2
                
                self.vel[0] = math.cos(new_angle) * speed
                self.vel[1] = math.sin(new_angle) * speed

        elif self.hiding_state == "hidden":
            speed = 0.6 # gently wiggle/wander around hiding spot
            wander_angle = random.uniform(-0.6, 0.6)
            current_angle = math.atan2(self.vel[1], self.vel[0])
            new_angle = current_angle + wander_angle
            
            # Keep clustered near hide_target
            dx = self.hide_target[0] - self.pos[0]
            dy = self.hide_target[1] - self.pos[1]
            dist_to_target = math.sqrt(dx**2 + dy**2)
            if dist_to_target > 15:
                ax += (dx/max(1, dist_to_target)) * 0.1
                ay += (dy/max(1, dist_to_target)) * 0.1
                
            self.vel[0] = math.cos(new_angle) * speed + ax
            self.vel[1] = math.sin(new_angle) * speed + ay
            
            if time.time() > self.hide_timer:
                self.hiding_state = "returning"
                speed = 1.0
                
        elif self.hiding_state == "returning":
            speed = 1.0 # creep back in slowly
            
            # Wander slowly but prefer center
            wander_angle = random.uniform(-0.5, 0.5)
            current_angle = math.atan2(self.vel[1], self.vel[0])
            new_angle = current_angle + wander_angle
            
            cx, cy = LCD_WIDTH/2, LCD_HEIGHT/2
            dx, dy = cx - self.pos[0], cy - self.pos[1]
            dist_center = math.sqrt(dx**2 + dy**2)
            if dist_center > LCD_WIDTH/2 - 50: # stronger pull to center
                ax += dx * 0.08
                ay += dy * 0.08
            else:
                self.hiding_state = "normal" # we are back!
                
            self.vel[0] = math.cos(new_angle) * speed + ax
            self.vel[1] = math.sin(new_angle) * speed + ay

        if speed > 0:
            self.normalize(self.vel)
            self.vel[0] *= speed
            self.vel[1] *= speed
            
            self.pos[0] += self.vel[0]
            self.pos[1] += self.vel[1]
            
            # Kinematic segments chase the head
            self.segments[0] = list(self.pos)
            for i in range(1, len(self.segments)):
                s_dx = self.segments[i-1][0] - self.segments[i][0]
                s_dy = self.segments[i-1][1] - self.segments[i][1]
                dist = math.sqrt(s_dx**2 + s_dy**2)
                if dist > self.segment_dist:
                    self.segments[i][0] += (s_dx/dist) * (dist - self.segment_dist)
                    self.segments[i][1] += (s_dy/dist) * (dist - self.segment_dist)
        
    def draw(self, img_bg):
        draw = ImageDraw.Draw(img_bg)
        
        # Draw tail
        last = self.segments[-1]
        prev = self.segments[-2]
        ang = math.atan2(last[1] - prev[1], last[0] - prev[0])
        t_len = 14
        t_spread = 0.5
        p1 = (last[0] + math.cos(ang - t_spread) * t_len, last[1] + math.sin(ang - t_spread) * t_len)
        p2 = (last[0] + math.cos(ang + t_spread) * t_len, last[1] + math.sin(ang + t_spread) * t_len)
        p3 = (last[0] + math.cos(ang) * (t_len * 0.3), last[1] + math.sin(ang) * (t_len * 0.3))
        draw.polygon([last, p1, p3, p2], fill=self.color)
        
        # Pectoral fins on segment 2
        f_seg = self.segments[2]
        f_prev = self.segments[1]
        f_ang = math.atan2(f_prev[1] - f_seg[1], f_prev[0] - f_seg[0]) # Facing forward direction
        flen = 10
        f_width = 5
        
        fx1 = f_seg[0] + math.cos(f_ang - math.pi/2 - 0.4) * (self.radii[2] + flen)
        fy1 = f_seg[1] + math.sin(f_ang - math.pi/2 - 0.4) * (self.radii[2] + flen)
        draw.polygon([f_seg, (fx1, fy1), (f_seg[0] + math.cos(f_ang)*f_width, f_seg[1] + math.sin(f_ang)*f_width)], fill=self.color)
        
        fx2 = f_seg[0] + math.cos(f_ang + math.pi/2 + 0.4) * (self.radii[2] + flen)
        fy2 = f_seg[1] + math.sin(f_ang + math.pi/2 + 0.4) * (self.radii[2] + flen)
        draw.polygon([f_seg, (fx2, fy2), (f_seg[0] + math.cos(f_ang)*f_width, f_seg[1] + math.sin(f_ang)*f_width)], fill=self.color)
        
        # Draw body chunks from tail to head
        for i in reversed(range(self.num_chunks)):
            seg = self.segments[i]
            r = self.radii[i]
            draw.ellipse((seg[0]-r, seg[1]-r, seg[0]+r, seg[1]+r), fill=self.color)
            
            # Tiny eyes
            if i == 0:
                h_ang = math.atan2(self.vel[1], self.vel[0])
                ex1 = seg[0] + math.cos(h_ang - math.pi/2.5) * (r * 0.7)
                ey1 = seg[1] + math.sin(h_ang - math.pi/2.5) * (r * 0.7)
                ex2 = seg[0] + math.cos(h_ang + math.pi/2.5) * (r * 0.7)
                ey2 = seg[1] + math.sin(h_ang + math.pi/2.5) * (r * 0.7)
                draw.ellipse((ex1-1.5, ey1-1.5, ex1+1.5, ey1+1.5), fill=(0,0,0))
                draw.ellipse((ex2-1.5, ey2-1.5, ex2+1.5, ey2+1.5), fill=(0,0,0))

class Pond:
    def __init__(self):
        self.lilypads = [
            Lilypad(40, 40, rot=random.uniform(0, 360), scale=0.8),
            Lilypad(200, 60, rot=random.uniform(0, 360), scale=1.2),
            Lilypad(50, 200, rot=random.uniform(0, 360), scale=0.9),
            Lilypad(180, 190, rot=random.uniform(0, 360), scale=1.1)
        ]
        
        self.fish = [Koi(LCD_WIDTH/2 + random.uniform(-50,50), LCD_HEIGHT/2 + random.uniform(-50,50)) for _ in range(6)]
        self.ripples = [] 
        self.touch_points = []
    
    def add_ripple(self, x, y):
        self.ripples.append([x, y, 0, 1.0])
        self.touch_points.append({"pos":(x,y), "life":0.5})

    def update(self):
        active_touches = [t["pos"] for t in self.touch_points]
        
        for f in self.fish:
            f.update(flee_points=active_touches, lilypads=self.lilypads)
            
        for r in self.ripples:
            r[2] += 2.0 
            r[3] -= 0.02 
        self.ripples = [r for r in self.ripples if r[3] > 0]
        
        for t in self.touch_points:
            t["life"] -= 0.05
        self.touch_points = [t for t in self.touch_points if t["life"] > 0]

    def render(self):
        img = assets.bg.copy()
        draw = ImageDraw.Draw(img)
        
        for f in self.fish:
            f.draw(img)
            
        for pad in self.lilypads:
            pad.draw(img)
            
        for r in self.ripples:
            alpha = int(255 * r[3])
            rad = r[2]
            draw.ellipse((r[0]-rad, r[1]-rad, r[0]+rad, r[1]+rad), outline=(100, 200, 220))
            
        return img

# Shared state
touch_x, touch_y, is_touched = -1, -1, False

def touch_thread(driver):
    global touch_x, touch_y, is_touched
    try:
        if "gpiod" in globals():
            from gpiod.line import Direction, Value
            rst = driver.chip.request_lines(config={17: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE)}, consumer="tr")
            rst.set_value(17, Value.INACTIVE)
            time.sleep(0.01)
            rst.set_value(17, Value.ACTIVE)
            time.sleep(0.05)
            
        bus = smbus2.SMBus(3)
        try:
            bus.write_byte_data(0x15, 0xFE, 0x01) 
            bus.write_byte_data(0x15, 0xFA, 0x41) 
        except:
            pass

        while True:
            try:
                data = bus.read_i2c_block_data(0x15, 0x01, 6)
                if data[1] > 0: 
                    x = ((data[2] & 0x0F) << 8) | data[3]
                    y = ((data[4] & 0x0F) << 8) | data[5]
                    x = (LCD_WIDTH - 1) - x
                    touch_x, touch_y, is_touched = x, y, True
                else:
                    is_touched = False
            except Exception:
                is_touched = False
            time.sleep(0.02)
    except Exception as e:
        pass

def main():
    global assets, is_touched
    print("Loading assets...")
    assets = PondAssets()
    
    print("Initializing display...")
    driver = DisplayDriver(spi_speed_hz=80000000)
    driver.init()
    
    print("Starting touch thread...")
    t = threading.Thread(target=touch_thread, args=(driver,), daemon=True)
    t.start()
    
    pond = Pond()
    
    print("Running Koi pond...")
    try:
        last_touch = False
        import numpy as np
        while True:
            t0 = time.time()
            if is_touched and not last_touch:
                pond.add_ripple(touch_x, touch_y)
            last_touch = is_touched
                
            pond.update()
            img = pond.render()
            
            img_arr = np.array(img.convert("RGB"), dtype=np.uint16)
            r = (img_arr[..., 0] >> 3) << 11
            g = (img_arr[..., 1] >> 2) << 5
            b = (img_arr[..., 2] >> 3)
            rgb565 = r | g | b
            driver.draw_frame(rgb565.byteswap().tobytes())
            
            # Simple frame limiter ~30fps
            elapsed = time.time() - t0
            if elapsed < 0.033:
                time.sleep(0.033 - elapsed)
            
    except KeyboardInterrupt:
        print("Exiting.")
    except Exception as e:
        import traceback
        with open("/tmp/koi_crash_trace.log", "w") as f:
            traceback.print_exc(file=f)
        raise

if __name__ == "__main__":
    main()
