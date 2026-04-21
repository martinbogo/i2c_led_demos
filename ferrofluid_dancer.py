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

MAGNET_BASE_SPEED = 7.0
MAGNET_PULSE_SPEED = 22.0
SWIRL_SPEED = 3.6
STAR_POINTS = 12
STAR_SHARPNESS = 16.0
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
    # Panel MADCTL=0x08 has bit 1 (BGR) = 0, so it is in RGB mode, not BGR.
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


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
        # Simulate realistic rock/electronic music with kick, snare, hi-hat, bass.
        # Pattern repeats every 4 seconds (musical phrase: 2s music + 2s silence/sparse).
        beat_cycle = t % 4.0
        
        # Only play dense drums for first 2 seconds of cycle, sparse for next 2 seconds.
        if beat_cycle < 2.0:
            # Kick drum: strong at 0.0, 0.5, 1.0, 1.5 seconds
            kick = 0.0
            for kick_time in [0.0, 0.5, 1.0, 1.5]:
                kick_envelope = max(0.0, 1.0 - abs(beat_cycle - kick_time) * 6.0)
                kick = max(kick, kick_envelope ** 2.0)
            
            # Snare: hits at 0.5, 1.5 (on the 2nd and 4th beat)
            snare = 0.0
            for snare_time in [0.5, 1.5]:
                snare_envelope = max(0.0, 1.0 - abs(beat_cycle - snare_time) * 8.0)
                snare = max(snare, snare_envelope ** 1.5)
            
            # Hi-hat: continuous sixteenth-note pattern (every 0.25s)
            hihat = 0.0
            for hihat_time in [0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75]:
                hihat_envelope = max(0.0, 1.0 - abs(beat_cycle - hihat_time) * 16.0)
                hihat = max(hihat, hihat_envelope ** 2.5)
            
            # Bass line: low-frequency swell
            bass = 0.5 + 0.5 * math.sin(beat_cycle * math.pi)
            bass = max(0.3, bass)
            
            # Mix everything: kick is most energetic, snare adds punch, hihat adds shimmer.
            energy = 0.08 + kick * 0.40 + snare * 0.18 + hihat * 0.12 + bass * 0.12
        else:
            # Sparse or silence: minimal energy, occasional hi-hat only
            sparse_cycle = beat_cycle - 2.0
            hihat = 0.0
            for hihat_time in [0.3, 1.0, 1.7]:  # Sparse hi-hat hits
                hihat_envelope = max(0.0, 1.0 - abs(sparse_cycle - hihat_time) * 8.0)
                hihat = max(hihat, hihat_envelope ** 2.5)
            energy = 0.02 + hihat * 0.06
        
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

        # Ferrofluid is EXTREMELY attracted to the magnet: it dances, not pools.
        # Magnetic attraction stays strong unless in dead silence.
        audio_responsive_gain = 0.4 + 0.6 * (self.audio_level ** 0.5)  # Min 0.4, max 1.0
        
        pulse = 0.5 + 0.5 * math.sin(t * 10.5)
        magnet_speed = (MAGNET_BASE_SPEED + MAGNET_PULSE_SPEED * pulse) * audio_responsive_gain
        swirl_speed = SWIRL_SPEED * (0.3 + 0.7 * self.audio_level)

        # Beat burst envelope for visible spike ejection and metaball-style split/recombine.
        beat_cycle = t % 4.0
        burst = 0.0
        if beat_cycle < 2.0:
            for hit_time in (0.0, 0.5, 1.0, 1.5):
                env = max(0.0, 1.0 - abs(beat_cycle - hit_time) * 10.0)
                burst = max(burst, env * env)

        # Compute centroid so quiet phases strongly re-merge droplets.
        cx = sum(self.px) / max(1, len(self.px))
        cy = sum(self.py) / max(1, len(self.py))

        # Light gravity: ferrofluid is heavy, but the magnet is much stronger.
        gravity_speed = 1.2  # Reduced from 6.5; magnet overwhelms this
        
        # Bounded drift keeps motion stable and avoids solver tunneling.
        max_magnet_step = magnet_speed * dt
        max_swirl_step = swirl_speed * dt
        gravity_step = gravity_speed * dt

        for i in range(len(self.px)):
            dx = self.px[i] - self.center
            dy = self.py[i] - self.center
            dist = math.sqrt(dx * dx + dy * dy) + 1e-6
            nx = dx / dist
            ny = dy / dist

            # Gravity: always present but weak (magnet dominates).
            self.py[i] += gravity_step

            # Magnetic pull to center: EXTREMELY strong, only weakens in true silence.
            far_gain = clamp(dist / max(self.radius * 0.7, 1.0), 0.2, 1.3)
            pull_step = max_magnet_step * far_gain
            self.px[i] -= nx * pull_step
            self.py[i] -= ny * pull_step

            # Spike formation during beats: forceful outward ejection at surface.
            spike_strength = (self.audio_level ** 1.05) * 5.8
            if dist > self.radius * 0.42:  # Broader range: from 42% radius outward
                spike_push = spike_strength * dt * (7.0 + 12.0 * burst)
                self.px[i] += nx * spike_push
                self.py[i] += ny * spike_push

            # 12-point star pull like clock marks: lock fluid into sharp spokes.
            angle = math.atan2(dy, dx)
            star_phase = angle * STAR_POINTS
            star_wave = 0.5 + 0.5 * math.cos(star_phase)
            star_focus = star_wave ** STAR_SHARPNESS

            # Radial extension on spoke directions during bursts.
            spoke_push = burst * dt * 18.0 * star_focus
            if dist > self.radius * 0.28:
                self.px[i] += nx * spoke_push
                self.py[i] += ny * spoke_push

            # Off-spoke suppression keeps valleys between spikes clean and sharp.
            valley_pull = burst * dt * 7.5 * (1.0 - star_focus)
            self.px[i] -= nx * valley_pull
            self.py[i] -= ny * valley_pull

            # Tangential steering snaps particles toward nearest spoke angle.
            tx = -ny
            ty = nx
            align_sign = math.sin(star_phase)
            steer = burst * dt * 6.0 * (1.0 - star_focus)
            if align_sign > 0.0:
                self.px[i] -= tx * steer
                self.py[i] -= ty * steer
            else:
                self.px[i] += tx * steer
                self.py[i] += ty * steer

            # Quiet phase cohesion: metaball-like recombination toward centroid.
            merge_gain = (1.0 - burst) * (0.35 + 0.65 * (1.0 - self.audio_level))
            cdx = self.px[i] - cx
            cdy = self.py[i] - cy
            clen = math.sqrt(cdx * cdx + cdy * cdy) + 1e-6
            self.px[i] -= (cdx / clen) * dt * 2.8 * merge_gain
            self.py[i] -= (cdy / clen) * dt * 2.8 * merge_gain

            # Add a dance swirl around center (always active when there's audio).
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
                    if d2 > 3.2:
                        continue
                    weight = (1.0 - d2 / 3.2) * 2.0
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
                self.density[row + x] = (up + mid * 1.2 + dn) / 3.2

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

                # Compute distance to circle for antialiasing edge pixels.
                radial = math.sqrt(radial_sq) if radial_sq > 1e-6 else 0.0
                edge_blend = 1.0 if radial <= self.radius - 2.0 else smoothstep(self.radius + 1.5, self.radius - 2.0, radial)

                if edge_blend < 0.01:
                    # Fully outside: black.
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

                    # Magnetic pulse glow.
                    dist_center = math.sqrt(dx * dx + dy * dy)
                    pulse = math.exp(-((dist_center - pulse_r) * (dist_center - pulse_r)) / 16.0)
                    pulse *= 0.55 + 0.45 * math.sin(t * 12.0)

                    waterness = smoothstep(0.02, 0.40, density)

                    # Dense fluid: pure black base with bright specular highlights.
                    if waterness > 0.4:
                        # Deep ferrofluid: render as dark with bright spec.
                        r = 8
                        g = 8
                        b = 10

                        # Normal vector from density gradient.
                        normal_x = -grad_x * 0.7
                        normal_y = -grad_y * 0.7
                        normal_z = 1.0
                        normal_len = math.sqrt(normal_x * normal_x + normal_y * normal_y + normal_z * normal_z)
                        if normal_len > 1e-6:
                            normal_x /= normal_len
                            normal_y /= normal_len
                            normal_z /= normal_len

                        # Bright specular highlight: light from top-left-front.
                        light_x, light_y, light_z = -0.40, -0.45, 0.80
                        spec = max(0.0, normal_x * light_x + normal_y * light_y + normal_z * light_z)
                        spec = spec ** 16
                        spec_bright = int(spec * 255.0)
                        r = clamp(r + spec_bright * 0.25, 0, 255)
                        g = clamp(g + spec_bright * 0.50, 0, 255)
                        b = clamp(b + spec_bright * 0.75, 0, 255)
                    else:
                        # Near-fluid or background: blend normally.
                        base_r = bg_r[idx]
                        base_g = bg_g[idx]
                        base_b = bg_b[idx]

                        surface_alpha = clamp(0.25 + waterness * 0.50, 0.0, 0.95)
                        fluid_r = 6
                        fluid_g = 6
                        fluid_b = 8

                        r = int(base_r * (1.0 - surface_alpha) + fluid_r * surface_alpha)
                        g = int(base_g * (1.0 - surface_alpha) + fluid_g * surface_alpha)
                        b = int(base_b * (1.0 - surface_alpha) + fluid_b * surface_alpha)

                        # Pulse mostly at background.
                        pulse_boost = int(14 * pulse * (1.0 - waterness) * (1.0 - waterness))
                        r = clamp(r + pulse_boost, 0, 255)
                        g = clamp(g + pulse_boost, 0, 255)
                        b = clamp(b + pulse_boost, 0, 255)

                    # Apply edge blending for antialiasing.
                    bg_r_edge = bg_r[idx]
                    bg_g_edge = bg_g[idx]
                    bg_b_edge = bg_b[idx]
                    r = int(r * edge_blend + bg_r_edge * (1.0 - edge_blend))
                    g = int(g * edge_blend + bg_g_edge * (1.0 - edge_blend))
                    b = int(b * edge_blend + bg_b_edge * (1.0 - edge_blend))

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
