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

SIM_SIZE = 60
SCALE = LCD_WIDTH // SIM_SIZE
CONTAINER_CENTER = (SIM_SIZE - 1) * 0.5
# The panel is addressed as 240x240, but the visible area is an inscribed circle.
# In simulation space (60x60 at SCALE=4), that visible radius is ~30 cells.
CONTAINER_RADIUS = SIM_SIZE * 0.5
CONTAINER_RADIUS_SQ = CONTAINER_RADIUS * CONTAINER_RADIUS
INNER_RADIUS_SQ = (CONTAINER_RADIUS - 1.5) * (CONTAINER_RADIUS - 1.5)

PARTICLE_COUNT = 520
PARTICLE_REST_RADIUS = 2.15
PARTICLE_REST_RADIUS_SQ = PARTICLE_REST_RADIUS * PARTICLE_REST_RADIUS
GRID_CELL_SIZE = 3.0
GRAVITY_ACCEL = 22.0
VELOCITY_DAMPING = 0.9975
WALL_BOUNCE = 0.62
VISCOSITY = 0.010
POSITION_RELAX = 0.36
SURFACE_TENSION = 0.0032
TARGET_FPS = 60
PHYSICS_HZ = 30.0
DEFAULT_WATER_SPI_SPEED = 80000000

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
    def __init__(self, fps=TARGET_FPS, spi_bus=SPI_BUS, spi_device=SPI_DEVICE, spi_speed_hz=DEFAULT_WATER_SPI_SPEED,
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
        self.scaled_row = bytearray(LCD_WIDTH * 2)

        self.dx_values = [x - self.center for x in range(self.grid_w)]
        self.dy_values = [y - self.center for y in range(self.grid_h)]
        # Byte offsets into an RGB565 row buffer
        self.base_x_values = [x * SCALE * 2 for x in range(self.grid_w)]
        self.base_y_values = [y * SCALE for y in range(self.grid_h)]
        self.row_stride = LCD_WIDTH * 2
        self.phase_shimmer = [0.22 * x + 0.17 * y for y in range(self.grid_h) for x in range(self.grid_w)]
        self.phase_caustic = [0.31 * x + 0.37 * y for y in range(self.grid_h) for x in range(self.grid_w)]
        self.base_bg_r = [0] * (self.grid_w * self.grid_h)
        self.base_bg_g = [0] * (self.grid_w * self.grid_h)
        self.base_bg_b = [0] * (self.grid_w * self.grid_h)

        self.sim_time = 0.0
        self.gravity_timer = 0.0
        self.gravity_target_x = 0.0
        self.gravity_target_y = 1.0
        self.gravity_x = 0.0
        self.gravity_y = 1.0

        self._spawn_initial_particles()
        self._build_pool_background_map()
        self._pick_new_gravity(initial=True)

    def _log(self, message):
        if self.verbose:
            print(message)

    def _pick_new_gravity(self, initial=False):
        angle = random.random() * math.tau
        prev_gx = self.gravity_target_x
        prev_gy = self.gravity_target_y
        self.gravity_target_x = math.cos(angle)
        self.gravity_target_y = math.sin(angle)
        self.gravity_timer = random.uniform(2.0, 15.0)

        # Add a coherent impulse so the fluid sloshes as a body, not as random spray.
        if not initial:
            delta_gx = self.gravity_target_x - prev_gx
            delta_gy = self.gravity_target_y - prev_gy
            dir_impulse = 1.9
            swirl_impulse = 3.2
            for i in range(len(self.px)):
                rx = self.px[i] - self.center
                ry = self.py[i] - self.center
                lateral = (-delta_gy * rx + delta_gx * ry) / max(self.radius, 1.0)
                self.vx[i] += self.gravity_target_x * dir_impulse - self.gravity_target_y * swirl_impulse * lateral
                self.vy[i] += self.gravity_target_y * dir_impulse + self.gravity_target_x * swirl_impulse * lateral

        self._log(f"[SIM] Gravity -> {math.degrees(angle):.1f} deg for {self.gravity_timer:.1f}s")

    def _spawn_initial_particles(self):
        while len(self.px) < PARTICLE_COUNT:
            x = random.uniform(self.center - self.radius * 0.97, self.center + self.radius * 0.97)
            # About one-third fill: spawn in lower segment of the circle.
            y = random.uniform(self.center + self.radius * 0.22, self.center + self.radius * 0.98)
            dx = x - self.center
            dy = y - self.center
            if dx * dx + dy * dy > (self.radius * 0.97) * (self.radius * 0.97):
                continue
            if y < self.center + self.radius * 0.20:
                continue
            self.px.append(x)
            self.py.append(y)
            self.vx.append((random.random() - 0.5) * 0.24)
            self.vy.append((random.random() - 0.5) * 0.20)

    def init(self):
        self.display.init()
        self.display.draw_frame(bytearray([BLACK_HI, BLACK_LO] * (LCD_WIDTH * LCD_HEIGHT)))

    def step(self, dt):
        self.sim_time += dt
        self.gravity_timer -= dt
        if self.gravity_timer <= 0.0:
            self._pick_new_gravity()

        particle_count = len(self.px)

        # Smoothly rotate toward the next gravity direction.
        blend = min(1.0, dt * 1.8)
        self.gravity_x += (self.gravity_target_x - self.gravity_x) * blend
        self.gravity_y += (self.gravity_target_y - self.gravity_y) * blend
        g_len = math.hypot(self.gravity_x, self.gravity_y)
        if g_len > 1e-6:
            self.gravity_x /= g_len
            self.gravity_y /= g_len

        dt60 = dt * PHYSICS_HZ

        for i in range(particle_count):
            self.vx[i] += self.gravity_x * GRAVITY_ACCEL * dt
            self.vy[i] += self.gravity_y * GRAVITY_ACCEL * dt
            self.vx[i] *= VELOCITY_DAMPING
            self.vy[i] *= VELOCITY_DAMPING

            self.px[i] += self.vx[i] * dt60
            self.py[i] += self.vy[i] * dt60

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

        # Circular boundary constraint matching the display's visible region.
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

        particle_count = len(self.px)
        for i in range(particle_count):
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
                    weight = (1.0 - d2 * 0.25) * 2.35
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

    def _build_pool_background_map(self):
        for sy in range(self.grid_h):
            dy = self.dy_values[sy]
            world_y = dy / self.radius
            row = sy * self.grid_w
            for sx in range(self.grid_w):
                dx = self.dx_values[sx]
                radial_sq = dx * dx + dy * dy
                radius_mix = clamp(1.0 - radial_sq / max(self.radius_sq, 1.0), 0.0, 1.0)
                wall_shadow = smoothstep(self.radius_sq, self.inner_radius_sq, radial_sq)
                world_x = dx / self.radius

                tile_wave = 0.5 + 0.5 * math.sin(world_x * 11.0)
                tile_wave *= 0.5 + 0.5 * math.sin(world_y * 10.0)
                tile_wave_2 = 0.5 + 0.5 * math.sin((world_x + world_y) * 13.0)
                tile_mix = clamp(tile_wave * 0.65 + tile_wave_2 * 0.35, 0.0, 1.0)

                base_r = 4 + int(14 * radius_mix)
                base_g = 28 + int(70 * radius_mix)
                base_b = 76 + int(124 * radius_mix)

                floor_r = base_r + int(10 * tile_mix)
                floor_g = base_g + int(22 * tile_mix)
                floor_b = base_b + int(30 * tile_mix)

                vignette = 0.30 + 0.70 * radius_mix
                edge_dim = 0.38 + 0.62 * wall_shadow
                idx = row + sx

                self.base_bg_r[idx] = clamp(int(floor_r * vignette * edge_dim), 0, 255)
                self.base_bg_g[idx] = clamp(int(floor_g * vignette * edge_dim), 0, 255)
                self.base_bg_b[idx] = clamp(int(floor_b * vignette * edge_dim), 0, 255)

    def render_frame(self):
        self._rasterize_density()

        frame = self.frame
        row_buffer = self.scaled_row
        row_stride = self.row_stride
        density_map = self.density
        bg_r_map = self.base_bg_r
        bg_g_map = self.base_bg_g
        bg_b_map = self.base_bg_b
        phase_shimmer = self.phase_shimmer
        phase_caustic = self.phase_caustic
        light_x = -0.36
        light_y = -0.42
        light_z = 0.83
        t = self.sim_time
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
                    refract_x = clamp(sx + int(grad_x * 1.4), 0, self.grid_w - 1)
                    refract_y = clamp(sy + int(grad_y * 1.4), 0, self.grid_h - 1)
                    refract_idx = refract_y * self.grid_w + refract_x

                    gravity_depth = ((dx * self.gravity_x) + (dy * self.gravity_y)) / max(self.radius, 1.0)
                    gravity_depth = 0.5 + gravity_depth * 0.5
                    waterness = smoothstep(0.02, 0.50, density)
                    surface_alpha = clamp(0.24 + waterness * 0.58, 0.0, 0.90)
                    edge_foam = smoothstep(0.08, 0.42, slope) * waterness
                    shimmer = 0.5 + 0.5 * math.sin(t * 2.2 + phase_shimmer[idx])
                    wave_axis = (-self.gravity_y * dx + self.gravity_x * dy) / max(self.radius, 1.0)
                    wave_plane = (self.gravity_x * dx + self.gravity_y * dy) / max(self.radius, 1.0)
                    wave_train = 0.5 + 0.5 * math.sin(t * 5.2 + wave_axis * 18.0 + wave_plane * 6.0)
                    caustic_wave = 0.5 + 0.5 * math.sin(
                        t * 4.0 + phase_caustic[idx] + grad_x * 7.0 - grad_y * 5.5 + density * 3.5 + wave_axis * 8.0
                    )
                    caustic_strength = (0.22 + 0.78 * caustic_wave) * (0.2 + waterness * 0.8)

                    bg_r = int(bg_r_map[idx] * 0.62 + bg_r_map[refract_idx] * 0.38)
                    bg_g = int(bg_g_map[idx] * 0.62 + bg_g_map[refract_idx] * 0.38)
                    bg_b = int(bg_b_map[idx] * 0.62 + bg_b_map[refract_idx] * 0.38)

                    if radial_sq > self.inner_radius_sq and waterness < 0.03:
                        r = bg_r
                        g = bg_g
                        b = bg_b
                    else:
                        deep_mix = clamp(0.10 + gravity_depth * 0.64 + waterness * 0.26 + (wave_train - 0.5) * 0.08, 0.0, 1.0)
                        tint_r = int(3 + 10 * deep_mix)
                        tint_g = int(78 + 110 * deep_mix)
                        tint_b = int(138 + 110 * deep_mix)

                        caustic_gain = 0.78 + 0.42 * caustic_strength
                        floor_r = clamp(int(bg_r * (0.90 + 0.10 * shimmer) * caustic_gain), 0, 255)
                        floor_g = clamp(int(bg_g * (0.94 + 0.16 * shimmer) * caustic_gain), 0, 255)
                        floor_b = clamp(int(bg_b * (0.98 + 0.22 * shimmer) * caustic_gain), 0, 255)

                        r = int(floor_r * (1.0 - surface_alpha) + tint_r * surface_alpha)
                        g = int(floor_g * (1.0 - surface_alpha) + tint_g * surface_alpha)
                        b = int(floor_b * (1.0 - surface_alpha) + tint_b * surface_alpha)

                        wave_highlight = waterness * wave_train * 56.0
                        r = clamp(int(r + wave_highlight * 0.08), 0, 255)
                        g = clamp(int(g + wave_highlight * 0.34), 0, 255)
                        b = clamp(int(b + wave_highlight * 0.54), 0, 255)

                        caustic_highlight = 24.0 + 136.0 * caustic_strength
                        r = clamp(int(r + caustic_highlight * 0.10), 0, 255)
                        g = clamp(int(g + caustic_highlight * 0.45), 0, 255)
                        b = clamp(int(b + caustic_highlight * 0.72), 0, 255)

                        normal_x = -grad_x * 0.85
                        normal_y = -grad_y * 0.85
                        normal_z = 1.0
                        normal_len = math.sqrt(normal_x * normal_x + normal_y * normal_y + normal_z * normal_z)
                        inv_len = 1.0 / normal_len if normal_len > 1e-6 else 1.0
                        normal_x *= inv_len
                        normal_y *= inv_len
                        normal_z *= inv_len
                        spec = normal_x * light_x + normal_y * light_y + normal_z * light_z
                        spec = clamp(spec, 0.0, 1.0) ** 18
                        fresnel = clamp(0.16 + slope * 0.95, 0.0, 1.0)
                        r = clamp(int(r + spec * 80.0 * (0.15 + 0.45 * fresnel)), 0, 255)
                        g = clamp(int(g + spec * 150.0 * (0.35 + 0.65 * fresnel)), 0, 255)
                        b = clamp(int(b + spec * 190.0 * (0.50 + 0.75 * fresnel)), 0, 255)

                        foam_boost = edge_foam * (42.0 + 68.0 * shimmer)
                        r = clamp(int(r + foam_boost * 0.20), 0, 255)
                        g = clamp(int(g + foam_boost * 0.55), 0, 255)
                        b = clamp(int(b + foam_boost * 0.65), 0, 255)

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
                dt = min(0.05, now - last_time)
                last_time = now
                accumulator += dt

                while accumulator >= physics_dt:
                    step_start = time.perf_counter()
                    self.step(physics_dt)
                    step_accum += time.perf_counter() - step_start
                    accumulator -= physics_dt

                render_start = time.perf_counter()
                frame = self.render_frame()
                render_accum += time.perf_counter() - render_start

                draw_start = time.perf_counter()
                self.display.draw_frame(frame)
                draw_accum += time.perf_counter() - draw_start
                frame_counter += 1
                stats_frames += 1

                if self.verbose and stats_frames >= 60:
                    stats_elapsed = time.monotonic() - stats_window_start
                    if stats_elapsed > 0:
                        actual_fps = stats_frames / stats_elapsed
                        step_ms = (step_accum / stats_frames) * 1000.0
                        render_ms = (render_accum / stats_frames) * 1000.0
                        draw_ms = (draw_accum / stats_frames) * 1000.0
                        print(
                            f"[SIM] Frame {frame_counter} | {actual_fps:.1f} fps | "
                            f"step {step_ms:.1f} ms | render {render_ms:.1f} ms | push {draw_ms:.1f} ms"
                        )
                    stats_frames = 0
                    stats_window_start = time.monotonic()
                    step_accum = 0.0
                    render_accum = 0.0
                    draw_accum = 0.0

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
    parser.add_argument("--fps", type=int, default=TARGET_FPS, help="Display FPS target (default: 60)")
    parser.add_argument("--spi-bus", type=int, default=SPI_BUS, help="SPI bus number (default: 0)")
    parser.add_argument("--spi-device", type=int, default=SPI_DEVICE, help="SPI device number (default: 0)")
    parser.add_argument("--spi-hz", type=int, default=DEFAULT_WATER_SPI_SPEED, help="SPI clock in Hz (default: 80000000)")
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
