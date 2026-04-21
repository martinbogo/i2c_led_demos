#!/usr/bin/env python3
"""
Ferrofluid dancer demo for the Waveshare 1.28" round GC9A01 display.

Visual concept:
- Neutral gray background.
- Black ferrofluid body constrained to the circular display.
- A center magnetic pulse modulated by simulated sound energy.
- Dancing motion from swirl and pulse terms, while maintaining stable packing.
"""

import argparse
import math
import random
import time

from badapple_waveshare import DisplayDriver, LCD_HEIGHT, LCD_WIDTH, SPI_BUS, SPI_DEVICE

SIM_SIZE = 60
SCALE = LCD_WIDTH // SIM_SIZE
CONTAINER_CENTER = (SIM_SIZE - 1) * 0.5
CONTAINER_RADIUS = SIM_SIZE * 0.5
CONTAINER_RADIUS_SQ = CONTAINER_RADIUS * CONTAINER_RADIUS
INNER_RADIUS_SQ = (CONTAINER_RADIUS - 1.2) * (CONTAINER_RADIUS - 1.2)

PARTICLE_COUNT = 280
PARTICLE_REST_RADIUS = 1.15
PARTICLE_REST_RADIUS_SQ = PARTICLE_REST_RADIUS * PARTICLE_REST_RADIUS
INTERACTION_RADIUS = PARTICLE_REST_RADIUS * 2.7
INTERACTION_RADIUS_SQ = INTERACTION_RADIUS * INTERACTION_RADIUS
GRID_CELL_SIZE = 3.2

PRESSURE_PUSH = 0.24
POSITION_RELAX = 0.50
VISCOSITY = 0.020
VELOCITY_DAMPING = 0.935
WALL_BOUNCE = 0.25

MAGNET_BASE_SPEED = 1.0
MAGNET_PULSE_SPEED = 7.2
SWIRL_SPEED = 1.8
TARGET_FPS = 60
PHYSICS_HZ = 30.0
SPI_SPEED_HZ = 80000000

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
    # Panel is in BGR mode from the GC9A01 init path.
    return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3)


class FerrofluidDancerDemo:
    def __init__(
        self,
        fps=TARGET_FPS,
        spi_bus=SPI_BUS,
        spi_device=SPI_DEVICE,
        spi_speed_hz=SPI_SPEED_HZ,
        backlight_active_high=True,
        use_gpio_cs=False,
        verbose=False,
    ):
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
        self.scaled_row = bytearray(LCD_WIDTH * 2)

        self.dx_values = [x - self.center for x in range(self.grid_w)]
        self.dy_values = [y - self.center for y in range(self.grid_h)]
        self.base_x_values = [x * SCALE * 2 for x in range(self.grid_w)]
        self.base_y_values = [y * SCALE for y in range(self.grid_h)]
        self.row_stride = LCD_WIDTH * 2

        self.bg_r = [0] * (self.grid_w * self.grid_h)
        self.bg_g = [0] * (self.grid_w * self.grid_h)
        self.bg_b = [0] * (self.grid_w * self.grid_h)

        self.sim_time = 0.0
        self.audio_level = 0.0

        self._spawn_initial_particles()
        self._build_background_map()

    def _log(self, message):
        if self.verbose:
            print(message)

    def _spawn_initial_particles(self):
        spawn_radius = self.radius * 0.36
        while len(self.px) < PARTICLE_COUNT:
            angle = random.uniform(0.0, math.tau)
            rad = spawn_radius * math.sqrt(random.random())
            x = self.center + math.cos(angle) * rad
            y = self.center + math.sin(angle) * rad
            self.px.append(x)
            self.py.append(y)
            self.vx.append((random.random() - 0.5) * 0.7)
            self.vy.append((random.random() - 0.5) * 0.7)

    def _sim_audio_level(self, t):
        bass = 0.5 + 0.5 * math.sin(t * 2.2)
        mid = 0.5 + 0.5 * math.sin(t * 4.9 + 1.3)
        beat = max(0.0, math.sin(t * 9.2)) ** 4
        triplet = 0.5 + 0.5 * math.sin(t * 13.1 + 0.7)
        energy = 0.14 + bass * 0.28 + mid * 0.24 + triplet * 0.10 + beat * 0.44
        return clamp(energy, 0.0, 1.0)

    def _build_background_map(self):
        for sy in range(self.grid_h):
            dy = self.dy_values[sy]
            row = sy * self.grid_w
            for sx in range(self.grid_w):
                dx = self.dx_values[sx]
                radial_sq = dx * dx + dy * dy
                radial = math.sqrt(radial_sq) / max(self.radius, 1.0)
                vignette = clamp(1.0 - radial * 0.62, 0.30, 1.0)

                # Cool neutral gray palette.
                base = 134 + int(30 * vignette)
                grain = int(7 * (0.5 + 0.5 * math.sin(0.28 * sx + 0.37 * sy)))
                level = clamp(base + grain, 0, 255)

                idx = row + sx
                self.bg_r[idx] = level
                self.bg_g[idx] = level
                self.bg_b[idx] = level

    def init(self):
        self.display.init()
        self.display.draw_frame(bytearray([BLACK_HI, BLACK_LO] * (LCD_WIDTH * LCD_HEIGHT)))

    def _apply_magnetic_motion(self, dt):
        t = self.sim_time
        self.audio_level = self._sim_audio_level(t)

        pulse = 0.5 + 0.5 * math.sin(t * 8.5)
        magnet_speed = MAGNET_BASE_SPEED + MAGNET_PULSE_SPEED * (0.25 + 0.75 * self.audio_level) * pulse
        swirl_speed = SWIRL_SPEED * (0.2 + 0.8 * self.audio_level)

        # Bounded drift keeps motion stable and avoids solver tunneling.
        max_magnet_step = magnet_speed * dt
        max_swirl_step = swirl_speed * dt

        for i in range(len(self.px)):
            dx = self.px[i] - self.center
            dy = self.py[i] - self.center
            dist = math.sqrt(dx * dx + dy * dy) + 1e-6
            nx = dx / dist
            ny = dy / dist

            # Pull to center, stronger for farther particles.
            far_gain = clamp(dist / max(self.radius * 0.7, 1.0), 0.2, 1.3)
            pull_step = max_magnet_step * far_gain
            self.px[i] -= nx * pull_step
            self.py[i] -= ny * pull_step

            # Add a dance swirl around center.
            twirl = 0.55 + 0.45 * math.sin(t * 3.2 + dist * 0.24)
            swirl = max_swirl_step * twirl
            self.px[i] += -ny * swirl
            self.py[i] += nx * swirl

    def step(self, dt):
        self.sim_time += dt
        particle_count = len(self.px)

        self._apply_magnetic_motion(dt)

        # Velocity path only carries short transient motion.
        for i in range(particle_count):
            self.vx[i] *= VELOCITY_DAMPING
            self.vy[i] *= VELOCITY_DAMPING
            self.px[i] += self.vx[i] * dt
            self.py[i] += self.vy[i] * dt

        # Spatial hash for local interactions.
        buckets = {}
        for i in range(particle_count):
            gx = int(self.px[i] / GRID_CELL_SIZE)
            gy = int(self.py[i] / GRID_CELL_SIZE)
            buckets.setdefault((gx, gy), []).append(i)

        for i in range(particle_count):
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
                        if d2 <= 1e-8 or d2 >= INTERACTION_RADIUS_SQ:
                            continue

                        dist = math.sqrt(d2)
                        nx = dx / dist
                        ny = dy / dist

                        q = 1.0 - (dist / INTERACTION_RADIUS)
                        push = q * q * PRESSURE_PUSH
                        if dist < PARTICLE_REST_RADIUS:
                            overlap = PARTICLE_REST_RADIUS - dist
                            push += overlap * POSITION_RELAX * (0.8 + 0.2 * overlap / PARTICLE_REST_RADIUS)
                        push = min(push, 0.70)

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

        # Circular boundary constraint.
        for i in range(particle_count):
            dx = self.px[i] - self.center
            dy = self.py[i] - self.center
            radial_sq = dx * dx + dy * dy
            if radial_sq > self.inner_radius_sq:
                radial = math.sqrt(radial_sq) if radial_sq > 1e-12 else 1.0
                nx = dx / radial
                ny = dy / radial

                self.px[i] = self.center + nx * (self.radius - 1.05)
                self.py[i] = self.center + ny * (self.radius - 1.05)

                vn = self.vx[i] * nx + self.vy[i] * ny
                if vn > 0.0:
                    self.vx[i] -= (1.0 + WALL_BOUNCE) * vn * nx
                    self.vy[i] -= (1.0 + WALL_BOUNCE) * vn * ny

    def _rasterize_density(self):
        cell_count = self.grid_w * self.grid_h
        for i in range(cell_count):
            self.density[i] = 0.0
            self.blur[i] = 0.0

        for i in range(len(self.px)):
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
                    if d2 > 4.2:
                        continue
                    weight = (1.0 - d2 / 4.2) * 2.40
                    self.density[sy * self.grid_w + sx] += weight

        # Two blur passes.
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

    def render_frame(self):
        self._rasterize_density()

        frame = self.frame
        row_buffer = self.scaled_row
        row_stride = self.row_stride
        density_map = self.density
        bg_r = self.bg_r
        bg_g = self.bg_g
        bg_b = self.bg_b
        t = self.sim_time

        pulse_r = 2.2 + 6.5 * self.audio_level

        for sy in range(self.grid_h):
            dy = self.dy_values[sy]
            base_y = self.base_y_values[sy]
            density_row = sy * self.grid_w
            density_up = max(0, sy - 1) * self.grid_w
            density_dn = min(self.grid_h - 1, sy + 1) * self.grid_w

            for sx in range(self.grid_w):
                dx = self.dx_values[sx]
                base_x = self.base_x_values[sx]
                radial_sq = dx * dx + dy * dy

                if radial_sq > self.radius_sq:
                    hi = BLACK_HI
                    lo = BLACK_LO
                else:
                    idx = density_row + sx
                    density = density_map[idx]

                    sx_l = sx - 1 if sx > 0 else 0
                    sx_r = sx + 1 if sx < self.grid_w - 1 else self.grid_w - 1
                    density_l = density_map[density_row + sx_l]
                    density_r = density_map[density_row + sx_r]
                    density_u = density_map[density_up + sx]
                    density_d = density_map[density_dn + sx]
                    grad_x = density_r - density_l
                    grad_y = density_d - density_u
                    slope = abs(grad_x) + abs(grad_y)

                    # Simulated magnetic pulse glow around center (subtle, gray).
                    dist_center = math.sqrt(dx * dx + dy * dy)
                    pulse = math.exp(-((dist_center - pulse_r) * (dist_center - pulse_r)) / 16.0)
                    pulse *= 0.55 + 0.45 * math.sin(t * 12.0)

                    waterness = smoothstep(0.04, 0.52, density)
                    surface_alpha = clamp(0.20 + waterness * 0.78, 0.0, 0.97)

                    base_r = bg_r[idx]
                    base_g = bg_g[idx]
                    base_b = bg_b[idx]

                    # Dark ferrofluid body color with slight metallic highlights.
                    fluid_r = clamp(int(20 - waterness * 13), 5, 24)
                    fluid_g = clamp(int(21 - waterness * 13), 5, 24)
                    fluid_b = clamp(int(22 - waterness * 13), 5, 24)

                    r = int(base_r * (1.0 - surface_alpha) + fluid_r * surface_alpha)
                    g = int(base_g * (1.0 - surface_alpha) + fluid_g * surface_alpha)
                    b = int(base_b * (1.0 - surface_alpha) + fluid_b * surface_alpha)

                    # Rim highlight from density gradient.
                    rim = clamp(slope * 26.0, 0.0, 26.0)
                    r = clamp(int(r + rim * 0.35), 0, 255)
                    g = clamp(int(g + rim * 0.35), 0, 255)
                    b = clamp(int(b + rim * 0.35), 0, 255)

                    # Magnetic pulse contribution to nearby background.
                    pulse_boost = int(18 * pulse * (1.0 - waterness * 0.5))
                    r = clamp(r + pulse_boost, 0, 255)
                    g = clamp(g + pulse_boost, 0, 255)
                    b = clamp(b + pulse_boost, 0, 255)

                    color565 = pack_panel_color(r, g, b)
                    hi = (color565 >> 8) & 0xFF
                    lo = color565 & 0xFF

                px_write = base_x
                for _ in range(SCALE):
                    row_buffer[px_write] = hi
                    row_buffer[px_write + 1] = lo
                    px_write += 2

            row_start = base_y * row_stride
            for oy in range(SCALE):
                dst = row_start + oy * row_stride
                frame[dst:dst + row_stride] = row_buffer

        return frame

    def run(self):
        self.init()

        frame_counter = 0
        accumulator = 0.0
        last_time = time.monotonic()
        next_frame = last_time
        physics_dt = 1.0 / PHYSICS_HZ

        stats_frames = 0
        stats_window_start = last_time
        step_accum = 0.0
        render_accum = 0.0
        draw_accum = 0.0

        try:
            while True:
                now = time.monotonic()
                elapsed = now - last_time
                last_time = now
                accumulator = min(0.25, accumulator + elapsed)

                while accumulator >= physics_dt:
                    t0 = time.monotonic()
                    self.step(physics_dt)
                    t1 = time.monotonic()
                    step_accum += t1 - t0
                    accumulator -= physics_dt

                if now >= next_frame:
                    t0 = time.monotonic()
                    frame = self.render_frame()
                    t1 = time.monotonic()
                    self.display.draw_frame(frame)
                    t2 = time.monotonic()

                    render_accum += t1 - t0
                    draw_accum += t2 - t1
                    frame_counter += 1
                    stats_frames += 1

                    next_frame = now + self.frame_delay

                    if self.verbose and stats_frames >= 60:
                        span = max(1e-6, now - stats_window_start)
                        fps = stats_frames / span
                        avg_step = (step_accum / stats_frames) * 1000.0
                        avg_render = (render_accum / stats_frames) * 1000.0
                        avg_draw = (draw_accum / stats_frames) * 1000.0
                        self._log(
                            f"[SIM] Frame {frame_counter} | {fps:.1f} fps | "
                            f"step {avg_step:.1f} ms | render {avg_render:.1f} ms | push {avg_draw:.1f} ms | "
                            f"audio {self.audio_level:.2f}"
                        )
                        stats_frames = 0
                        stats_window_start = now
                        step_accum = 0.0
                        render_accum = 0.0
                        draw_accum = 0.0
                else:
                    sleep_time = next_frame - now
                    if sleep_time > 0.001:
                        time.sleep(sleep_time)
        except KeyboardInterrupt:
            pass
        finally:
            self.display.close()


def parse_args():
    parser = argparse.ArgumentParser(description="Ferrofluid dancer demo for 240x240 round GC9A01 display")
    parser.add_argument("--fps", type=int, default=TARGET_FPS, help="Target frame rate")
    parser.add_argument("--spi-bus", type=int, default=SPI_BUS, help="SPI bus")
    parser.add_argument("--spi-device", type=int, default=SPI_DEVICE, help="SPI device")
    parser.add_argument("--spi-speed-hz", type=int, default=SPI_SPEED_HZ, help="SPI speed")
    parser.add_argument("--gpio-cs", action="store_true", help="Use GPIO software chip-select")
    parser.add_argument("--backlight-active-low", action="store_true", help="Set if panel backlight is active low")
    parser.add_argument("--verbose", action="store_true", help="Verbose logs")
    return parser.parse_args()


def main():
    args = parse_args()
    demo = FerrofluidDancerDemo(
        fps=args.fps,
        spi_bus=args.spi_bus,
        spi_device=args.spi_device,
        spi_speed_hz=args.spi_speed_hz,
        backlight_active_high=not args.backlight_active_low,
        use_gpio_cs=args.gpio_cs,
        verbose=args.verbose,
    )
    demo.run()


if __name__ == "__main__":
    main()
