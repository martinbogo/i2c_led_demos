#!/usr/bin/env python3
"""
Full-color water slosh demo for the Waveshare 1.28" round GC9A01 display.

Behavior:
- Simulates water inside the circular display boundary using particle dynamics.
- Randomly changes gravity direction every 2 to 15 seconds.
- Renders a full-color blue/cyan water mass with foam-like highlights.
- Keeps pixels outside the round display mask black.
"""

import argparse
import math
import random
import sys
import time

from badapple_waveshare import DisplayDriver, LCD_HEIGHT, LCD_WIDTH, SPI_BUS, SPI_DEVICE, SPI_SPEED

SIM_SIZE = 80
SCALE = LCD_WIDTH // SIM_SIZE
CONTAINER_CENTER = (SIM_SIZE - 1) * 0.5
CONTAINER_RADIUS = CONTAINER_CENTER - 1.75
CONTAINER_RADIUS_SQ = CONTAINER_RADIUS * CONTAINER_RADIUS
INNER_RADIUS_SQ = (CONTAINER_RADIUS - 1.5) * (CONTAINER_RADIUS - 1.5)

PARTICLE_COUNT = 560
PARTICLE_REST_RADIUS = 1.9
PARTICLE_REST_RADIUS_SQ = PARTICLE_REST_RADIUS * PARTICLE_REST_RADIUS
GRID_CELL_SIZE = 2.5
GRAVITY_ACCEL = 18.0
VELOCITY_DAMPING = 0.996
WALL_BOUNCE = 0.45
VISCOSITY = 0.015
POSITION_RELAX = 0.42
SURFACE_TENSION = 0.0015
TARGET_FPS = 18
PHYSICS_HZ = 60.0

BLACK_HI = 0x00
BLACK_LO = 0x00


def clamp(value, lo, hi):
    return lo if value < lo else hi if value > hi else value


def smoothstep(edge0, edge1, value):
    if edge0 == edge1:
        return 0.0
    t = clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def pack_panel_color(r, g, b):
    """Pack color for the current panel configuration.

    The GC9A01 init in `badapple_waveshare.py` uses MADCTL=0x08, which means
    the panel is in BGR mode. Swapping red/blue here keeps the rendered result
    visually correct without disturbing the rest of the established driver path.
    """
    return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3)


class WavesAndWaterDemo:
    def __init__(self, fps=TARGET_FPS, spi_bus=SPI_BUS, spi_device=SPI_DEVICE, spi_speed_hz=SPI_SPEED,
                 backlight_active_high=True, use_gpio_cs=False, verbose=False):
        self.verbose = verbose
        self.frame_delay = 1.0 / max(1, fps)
        self.display = DisplayDriver(
            verbose=verbose,
            spi_bus=spi_bus,
            spi_device=spi_device,
            spi_speed_hz=spi_speed_hz,
            backlight_active_high=backlight_active_high,
            use_gpio_cs=use_gpio_cs,
        )

        self.grid_w = SIM_SIZE
        self.grid_h = SIM_SIZE
        self.center = CONTAINER_CENTER
        self.radius = CONTAINER_RADIUS
        self.radius_sq = CONTAINER_RADIUS_SQ
        self.inner_radius_sq = INNER_RADIUS_SQ

        self.px = []
        self.py = []
        self.vx = []
        self.vy = []

        self.density = [0.0] * (self.grid_w * self.grid_h)
        self.blur = [0.0] * (self.grid_w * self.grid_h)
        self.frame = bytearray(LCD_WIDTH * LCD_HEIGHT * 2)

        self.sim_time = 0.0
        self.gravity_timer = 0.0
        self.gravity_target_x = 0.0
        self.gravity_target_y = 1.0
        self.gravity_x = 0.0
        self.gravity_y = 1.0

        self._spawn_initial_particles()
        self._pick_new_gravity(initial=True)

    def _log(self, message):
        if self.verbose:
            print(message)

    def _pick_new_gravity(self, initial=False):
        angle = random.random() * math.tau
        self.gravity_target_x = math.cos(angle)
        self.gravity_target_y = math.sin(angle)
        self.gravity_timer = random.uniform(2.0, 15.0)

        # Add a short impulse so the water visibly reacts when gravity changes.
        if not initial:
            impulse = 1.6
            for i in range(PARTICLE_COUNT):
                jitter = 0.75 + random.random() * 0.5
                self.vx[i] += self.gravity_target_x * impulse * jitter
                self.vy[i] += self.gravity_target_y * impulse * jitter

        self._log(f"[SIM] Gravity -> {math.degrees(angle):.1f} deg for {self.gravity_timer:.1f}s")

    def _spawn_initial_particles(self):
        while len(self.px) < PARTICLE_COUNT:
            x = random.uniform(self.center - self.radius * 0.82, self.center + self.radius * 0.82)
            y = random.uniform(self.center - self.radius * 0.15, self.center + self.radius * 0.88)
            dx = x - self.center
            dy = y - self.center
            if dx * dx + dy * dy > (self.radius * 0.84) * (self.radius * 0.84):
                continue
            if y < self.center - 3.5:
                continue
            self.px.append(x)
            self.py.append(y)
            self.vx.append((random.random() - 0.5) * 0.4)
            self.vy.append((random.random() - 0.5) * 0.4)

    def init(self):
        self.display.init()
        self.display.draw_frame(bytearray([BLACK_HI, BLACK_LO] * (LCD_WIDTH * LCD_HEIGHT)))

    def step(self, dt):
        self.sim_time += dt
        self.gravity_timer -= dt
        if self.gravity_timer <= 0.0:
            self._pick_new_gravity()

        # Smoothly rotate toward the next gravity direction.
        blend = min(1.0, dt * 1.8)
        self.gravity_x += (self.gravity_target_x - self.gravity_x) * blend
        self.gravity_y += (self.gravity_target_y - self.gravity_y) * blend
        g_len = math.hypot(self.gravity_x, self.gravity_y)
        if g_len > 1e-6:
            self.gravity_x /= g_len
            self.gravity_y /= g_len

        dt60 = dt * PHYSICS_HZ

        for i in range(PARTICLE_COUNT):
            self.vx[i] += self.gravity_x * GRAVITY_ACCEL * dt
            self.vy[i] += self.gravity_y * GRAVITY_ACCEL * dt
            self.vx[i] *= VELOCITY_DAMPING
            self.vy[i] *= VELOCITY_DAMPING

            self.px[i] += self.vx[i] * dt60
            self.py[i] += self.vy[i] * dt60

        # Spatial hash for local interactions.
        buckets = {}
        for i in range(PARTICLE_COUNT):
            gx = int(self.px[i] / GRID_CELL_SIZE)
            gy = int(self.py[i] / GRID_CELL_SIZE)
            buckets.setdefault((gx, gy), []).append(i)

        for i in range(PARTICLE_COUNT):
            gx = int(self.px[i] / GRID_CELL_SIZE)
            gy = int(self.py[i] / GRID_CELL_SIZE)

            for oy in (-1, 0, 1):
                for ox in (-1, 0, 1):
                    cell = (gx + ox, gy + oy)
                    if cell not in buckets:
                        continue
                    for j in buckets[cell]:
                        if j <= i:
                            continue
                        dx = self.px[j] - self.px[i]
                        dy = self.py[j] - self.py[i]
                        d2 = dx * dx + dy * dy
                        if d2 <= 1e-8 or d2 >= PARTICLE_REST_RADIUS_SQ:
                            continue

                        dist = math.sqrt(d2)
                        nx = dx / dist
                        ny = dy / dist
                        overlap = PARTICLE_REST_RADIUS - dist
                        push = overlap * POSITION_RELAX

                        self.px[i] -= nx * push
                        self.py[i] -= ny * push
                        self.px[j] += nx * push
                        self.py[j] += ny * push

                        rvx = self.vx[j] - self.vx[i]
                        rvy = self.vy[j] - self.vy[i]
                        self.vx[i] += rvx * VISCOSITY
                        self.vy[i] += rvy * VISCOSITY
                        self.vx[j] -= rvx * VISCOSITY
                        self.vy[j] -= rvy * VISCOSITY

                        cohesion = overlap * SURFACE_TENSION
                        self.vx[i] += nx * cohesion
                        self.vy[i] += ny * cohesion
                        self.vx[j] -= nx * cohesion
                        self.vy[j] -= ny * cohesion

        # Circular boundary constraint.
        for i in range(PARTICLE_COUNT):
            dx = self.px[i] - self.center
            dy = self.py[i] - self.center
            d2 = dx * dx + dy * dy
            if d2 > self.radius_sq:
                dist = math.sqrt(d2)
                nx = dx / dist
                ny = dy / dist
                limit = self.radius - 0.6
                self.px[i] = self.center + nx * limit
                self.py[i] = self.center + ny * limit

                vn = self.vx[i] * nx + self.vy[i] * ny
                if vn > 0.0:
                    self.vx[i] -= (1.0 + WALL_BOUNCE) * vn * nx
                    self.vy[i] -= (1.0 + WALL_BOUNCE) * vn * ny

                self.vx[i] *= 0.98
                self.vy[i] *= 0.98

    def _rasterize_density(self):
        cell_count = self.grid_w * self.grid_h
        for i in range(cell_count):
            self.density[i] = 0.0
            self.blur[i] = 0.0

        for i in range(PARTICLE_COUNT):
            x = self.px[i]
            y = self.py[i]
            ix = int(x)
            iy = int(y)
            for oy in (-1, 0, 1):
                sy = iy + oy
                if sy < 0 or sy >= self.grid_h:
                    continue
                for ox in (-1, 0, 1):
                    sx = ix + ox
                    if sx < 0 or sx >= self.grid_w:
                        continue
                    dx = (sx + 0.5) - x
                    dy = (sy + 0.5) - y
                    d2 = dx * dx + dy * dy
                    if d2 > 4.0:
                        continue
                    weight = 1.0 - d2 * 0.25
                    self.density[sy * self.grid_w + sx] += weight

        # Two cheap blur passes for a smoother liquid body.
        for y in range(self.grid_h):
            row = y * self.grid_w
            for x in range(self.grid_w):
                left = self.density[row + max(0, x - 1)]
                mid = self.density[row + x]
                right = self.density[row + min(self.grid_w - 1, x + 1)]
                self.blur[row + x] = (left + mid * 2.0 + right) * 0.25

        for y in range(self.grid_h):
            row = y * self.grid_w
            up_row = max(0, y - 1) * self.grid_w
            dn_row = min(self.grid_h - 1, y + 1) * self.grid_w
            for x in range(self.grid_w):
                up = self.blur[up_row + x]
                mid = self.blur[row + x]
                dn = self.blur[dn_row + x]
                self.density[row + x] = (up + mid * 2.0 + dn) * 0.25

    def _sample_density(self, x, y):
        x = clamp(x, 0, self.grid_w - 1)
        y = clamp(y, 0, self.grid_h - 1)
        return self.density[int(y) * self.grid_w + int(x)]

    def _pool_background(self, sx, sy, dx, dy, radial_sq):
        radius_mix = clamp(1.0 - radial_sq / max(self.radius_sq, 1.0), 0.0, 1.0)
        wall_shadow = smoothstep(self.radius_sq, self.inner_radius_sq, radial_sq)

        world_x = (sx - self.center) / self.radius
        world_y = (sy - self.center) / self.radius
        t = self.sim_time

        tile_wave = 0.5 + 0.5 * math.sin(world_x * 11.0 + t * 0.9)
        tile_wave *= 0.5 + 0.5 * math.sin(world_y * 10.0 - t * 0.7)
        tile_wave_2 = 0.5 + 0.5 * math.sin((world_x + world_y) * 13.0 - t * 1.1)
        tile_mix = clamp(tile_wave * 0.65 + tile_wave_2 * 0.35, 0.0, 1.0)

        base_r = 8 + int(18 * radius_mix)
        base_g = 34 + int(58 * radius_mix)
        base_b = 68 + int(96 * radius_mix)

        floor_r = base_r + int(8 * tile_mix)
        floor_g = base_g + int(14 * tile_mix)
        floor_b = base_b + int(18 * tile_mix)

        vignette = 0.38 + 0.62 * radius_mix
        floor_r = int(floor_r * vignette)
        floor_g = int(floor_g * vignette)
        floor_b = int(floor_b * vignette)

        edge_dim = 0.45 + 0.55 * wall_shadow
        return (
            clamp(int(floor_r * edge_dim), 0, 255),
            clamp(int(floor_g * edge_dim), 0, 255),
            clamp(int(floor_b * edge_dim), 0, 255),
        )

    def render_frame(self):
        self._rasterize_density()

        frame = self.frame
        for sy in range(self.grid_h):
            dy = sy - self.center
            base_y = sy * SCALE
            for sx in range(self.grid_w):
                dx = sx - self.center
                base_x = sx * SCALE
                radial_sq = dx * dx + dy * dy

                if radial_sq > self.radius_sq:
                    hi = BLACK_HI
                    lo = BLACK_LO
                else:
                    density = self.density[sy * self.grid_w + sx]
                    density_l = self._sample_density(sx - 1, sy)
                    density_r = self._sample_density(sx + 1, sy)
                    density_u = self._sample_density(sx, sy - 1)
                    density_d = self._sample_density(sx, sy + 1)
                    grad_x = density_r - density_l
                    grad_y = density_d - density_u
                    slope = math.sqrt(grad_x * grad_x + grad_y * grad_y)

                    gravity_depth = ((dx * self.gravity_x) + (dy * self.gravity_y)) / max(self.radius, 1.0)
                    gravity_depth = 0.5 + gravity_depth * 0.5
                    waterness = smoothstep(0.03, 0.36, density)
                    surface_alpha = clamp(0.16 + waterness * 0.46, 0.0, 0.72)
                    edge_foam = smoothstep(0.08, 0.42, slope) * waterness
                    shimmer = 0.5 + 0.5 * math.sin(self.sim_time * 2.2 + sx * 0.22 + sy * 0.17)
                    caustic_wave = 0.5 + 0.5 * math.sin(
                        self.sim_time * 4.6
                        + sx * 0.42
                        + sy * 0.28
                        + grad_x * 8.0
                        - grad_y * 6.5
                    )
                    caustic_wave *= 0.5 + 0.5 * math.sin(
                        self.sim_time * 3.1
                        - sx * 0.19
                        + sy * 0.33
                        + density * 4.0
                    )
                    caustic_strength = (0.18 + 0.82 * caustic_wave) * (0.2 + waterness * 0.8)

                    bg_r, bg_g, bg_b = self._pool_background(sx, sy, dx, dy, radial_sq)

                    if radial_sq > self.inner_radius_sq and waterness < 0.03:
                        r = bg_r
                        g = bg_g
                        b = bg_b
                    else:
                        deep_mix = clamp(0.20 + gravity_depth * 0.80, 0.0, 1.0)
                        tint_r = int(6 + 12 * deep_mix)
                        tint_g = int(88 + 56 * deep_mix)
                        tint_b = int(150 + 80 * deep_mix)

                        refract_shift_x = grad_x * 18.0 + math.sin(self.sim_time * 2.7 + sy * 0.18) * 0.6
                        refract_shift_y = grad_y * 18.0 + math.cos(self.sim_time * 2.3 + sx * 0.16) * 0.6
                        refract_r, refract_g, refract_b = self._pool_background(
                            sx + refract_shift_x,
                            sy + refract_shift_y,
                            dx,
                            dy,
                            radial_sq,
                        )

                        floor_r = int(bg_r * (1.0 - caustic_strength * 0.25) + refract_r * (0.88 + caustic_strength * 0.12))
                        floor_g = int(bg_g * (1.0 - caustic_strength * 0.20) + refract_g * (0.90 + caustic_strength * 0.10))
                        floor_b = int(bg_b * (1.0 - caustic_strength * 0.16) + refract_b * (0.92 + caustic_strength * 0.08))

                        r = int(floor_r * (1.0 - surface_alpha) + tint_r * surface_alpha)
                        g = int(floor_g * (1.0 - surface_alpha) + tint_g * surface_alpha)
                        b = int(floor_b * (1.0 - surface_alpha) + tint_b * surface_alpha)

                        caustic_highlight = 40.0 + 105.0 * caustic_strength
                        r = clamp(int(r + caustic_highlight * 0.10), 0, 255)
                        g = clamp(int(g + caustic_highlight * 0.45), 0, 255)
                        b = clamp(int(b + caustic_highlight * 0.72), 0, 255)

                        foam_boost = edge_foam * (42.0 + 68.0 * shimmer)
                        r = clamp(int(r + foam_boost * 0.20), 0, 255)
                        g = clamp(int(g + foam_boost * 0.55), 0, 255)
                        b = clamp(int(b + foam_boost * 0.65), 0, 255)

                    color565 = pack_panel_color(r, g, b)
                    hi = (color565 >> 8) & 0xFF
                    lo = color565 & 0xFF

                for oy in range(SCALE):
                    row_write = (((base_y + oy) * LCD_WIDTH) + base_x) * 2
                    for _ in range(SCALE):
                        frame[row_write] = hi
                        frame[row_write + 1] = lo
                        row_write += 2

        return frame

    def run(self):
        self.init()

        frame_counter = 0
        accumulator = 0.0
        last_time = time.monotonic()
        next_frame = last_time
        physics_dt = 1.0 / PHYSICS_HZ

        try:
            while True:
                now = time.monotonic()
                dt = min(0.05, now - last_time)
                last_time = now
                accumulator += dt

                while accumulator >= physics_dt:
                    self.step(physics_dt)
                    accumulator -= physics_dt

                frame = self.render_frame()
                self.display.draw_frame(frame)
                frame_counter += 1

                if self.verbose and frame_counter % 30 == 0:
                    print(f"[SIM] Frame {frame_counter}")

                next_frame += self.frame_delay
                sleep_time = next_frame - time.monotonic()
                if sleep_time > 0:
                    time.sleep(sleep_time)
                else:
                    next_frame = time.monotonic()

        except KeyboardInterrupt:
            print("\nInterrupted")
        finally:
            self.display.cleanup()


def main():
    parser = argparse.ArgumentParser(description="Full-color waves and water demo for the Waveshare round display")
    parser.add_argument("--fps", type=int, default=TARGET_FPS, help="Display FPS target (default: 18)")
    parser.add_argument("--spi-bus", type=int, default=SPI_BUS, help="SPI bus number (default: 0)")
    parser.add_argument("--spi-device", type=int, default=SPI_DEVICE, help="SPI device number (default: 0)")
    parser.add_argument("--spi-hz", type=int, default=SPI_SPEED, help="SPI clock in Hz (default: 10000000)")
    parser.add_argument("--bl-active-low", action="store_true", help="Use active-low backlight polarity")
    parser.add_argument("--gpio-cs", action="store_true", help="Drive CS with GPIO instead of SPI hardware CS")
    parser.add_argument("--verbose", action="store_true", help="Print gravity changes and frame counters")

    args = parser.parse_args()

    demo = WavesAndWaterDemo(
        fps=args.fps,
        spi_bus=args.spi_bus,
        spi_device=args.spi_device,
        spi_speed_hz=args.spi_hz,
        backlight_active_high=not args.bl_active_low,
        use_gpio_cs=args.gpio_cs,
        verbose=args.verbose,
    )
    demo.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
