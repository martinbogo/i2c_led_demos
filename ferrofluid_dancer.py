#!/usr/bin/env python3
# Author  : Martin Bogomolni
# Date    : 2026-04-21
# License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)

"""
Ferrofluid dancer demo for the Waveshare 1.28" round GC9A01 display.

Visual concept:
- High-performance 2D simulated physics of a magnetic fluid.
- A center magnetic pulse modulated by simulated sound energy, responding dynamically.
- Dancing motion from swirl and pulse terms, while maintaining stable packing.
- Uses capacitive touch interactions via the CST816S driver to let the user manipulate the fluid center point.
- Interactive mode options:
  * `--party`: Enables an RGB color-shifting mode on the fluid surface lighting.
  * `--test`: Minimal visualizer used to debug touchscreen X/Y output.
  * `--gpio-cs`: Enables software chip-selection for SPI devices.

Hardware Note: 
- Designed originally for SPI-based GC9A01 LCD mapping and specifically hardware-accelerated SPI.
- Touch utilizes I2C communication and GPIO interrupt lines (Interrupt and Reset).
"""

import argparse
import math
import random
import time

from badapple_waveshare import DisplayDriver, LCD_HEIGHT, LCD_WIDTH, SPI_BUS, SPI_DEVICE

SIM_SIZE = 80
SCALE = LCD_WIDTH // SIM_SIZE
CONTAINER_CENTER = (SIM_SIZE - 1) * 0.5
CONTAINER_RADIUS = SIM_SIZE * 0.5
CONTAINER_RADIUS_SQ = CONTAINER_RADIUS * CONTAINER_RADIUS
INNER_RADIUS_SQ = (CONTAINER_RADIUS - 1.2) * (CONTAINER_RADIUS - 1.2)

PARTICLE_COUNT = 240
PARTICLE_REST_RADIUS = 1.48
PARTICLE_REST_RADIUS_SQ = PARTICLE_REST_RADIUS * PARTICLE_REST_RADIUS
INTERACTION_RADIUS = PARTICLE_REST_RADIUS * 2.55
INTERACTION_RADIUS_SQ = INTERACTION_RADIUS * INTERACTION_RADIUS
GRID_CELL_SIZE = 4.0

PRESSURE_PUSH = 0.20
POSITION_RELAX = 0.50
VISCOSITY = 0.055
VELOCITY_DAMPING = 0.965
WALL_BOUNCE = 0.25
SKIN_STRETCH_COHESION = 0.026
SKIN_BRIDGE_GAIN = 0.16
SKIN_BRIDGE_THRESHOLD = 0.12

SETTLING_ACCEL = 0.18
POOL_RETURN_X = 0.55
POOL_RETURN_Y = 1.35
POOL_SHAPE_SQUASH = 0.72
MAGNET_ATTACK = 30.0
MAGNET_HOLD = 10.5
MAGNET_SWIRL = 4.8
MAGNET_PATH_X_RATIO = 0.28
MAGNET_PATH_Y_RATIO = 0.17
FIELD_ATTACK_RATE = 0.48
FIELD_RELEASE_RATE = 0.12
FIELD_ORBIT_LERP = 0.20
RELEASE_COLLAPSE_FORCE = 6.2
RELEASE_COLLAPSE_DECAY = 0.86
SPIKE_FIELD_RESPONSE = 0.22
SPIKE_INSTABILITY_THRESHOLD = 0.16
SPIKE_LEAN_GAIN = 0.32
SNAP_FRONT_GAIN = 0.90
SNAP_STRETCH_GAIN = 10.5
SNAP_AXIS_PINCH = 0.85
BREAKUP_FIELD_THRESHOLD = 0.42
BREAKUP_COHESION_DROP = 0.82
BREAKUP_PUSH_GAIN = 0.22
FLING_IMPULSE_GAIN = 12.0
FLING_TANGENTIAL_GAIN = 7.5

MAGNET_BASE_SPEED = 7.0
MAGNET_PULSE_SPEED = 22.0
SWIRL_SPEED = 3.6
STAR_POINTS = 8
STAR_SHARPNESS = 16.0
PEAK_SPIKE_RADIUS_RATIO = 0.50
ISO_ENTER = 0.22
ISO_EXIT = 0.16
TARGET_FPS = 36
PHYSICS_HZ = 24.0
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
        party_mode=False,
        test_mode=False,
    ):
        self.verbose = verbose
        self.party_mode = party_mode
        self.test_mode = test_mode
        self.frame_delay = 1.0 / max(1, fps)
        self.display = DisplayDriver(
            verbose=verbose,
            spi_bus=spi_bus,
            spi_device=spi_device,
            spi_speed_hz=spi_speed_hz,
            backlight_active_high=backlight_active_high,
            use_gpio_cs=use_gpio_cs,
        )

        try:
            import smbus2
            import gpiod
            from gpiod.line import Direction, Value
            
            # Use GPIO 4 for the touch interrupt pin (active low)
            # When touched, this pin goes LOW
            self.touch_int = self.display.chip.request_lines(
                config={4: gpiod.LineSettings(direction=Direction.INPUT)},
                consumer="Touch_INT"
            )
            
            # Reset the CST816S via GPIO17 to wake it
            # The reset requires pulling the line LOW for roughly 100ms
            # and then keeping it HIGH for roughly 200ms before it becomes responsive.
            # Without this sequence, the touch chip may stay in a low-power deep sleep.
            self.touch_reset = self.display.chip.request_lines(
                config={17: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE)},
                consumer="Touch_Reset"
            )
            self.touch_reset.set_value(17, Value.INACTIVE) # Pull low
            time.sleep(0.1)
            self.touch_reset.set_value(17, Value.ACTIVE)   # Pull high
            time.sleep(0.2)
            
            # NOTE ON I2C BUS:
            # Due to damaged I2C silicon on the primary Raspberry Pi testing hardware (Bus 1),
            # this demo initializes a software I2C bus via `dtoverlay=i2c-gpio` on Bus 3.
            # If your standard I2C is working, or you are on a different Pi, change this back to:
            # self.i2c_bus = smbus2.SMBus(1)
            self.i2c_bus = smbus2.SMBus(3)
            
            # Configure CST816S for continuous touch reporting
            # Stop AutoSleep
            self.i2c_bus.write_byte_data(0x15, 0xFE, 0x01)
            # Set mode: Point Mode (0x41 enables EnTouch & EnMotion)
            self.i2c_bus.write_byte_data(0x15, 0xFA, 0x41)
            # Normal scan period to 10ms
            self.i2c_bus.write_byte_data(0x15, 0xEE, 0x01)
            # Interrupt pulse width to 1.5ms
            self.i2c_bus.write_byte_data(0x15, 0xED, 0x0F)
            
            self._log("CST816S Touch successfully initialized.")
            self.has_touch = True
        except ImportError:
            self._log("smbus2 not found, touch interactions disabled.")
            self.has_touch = False
        except Exception as e:
            self._log(f"Failed to initialize I2C touch: {e}")
            self.has_touch = False

        self.touch_active = False
        self.touch_x = CONTAINER_CENTER
        self.touch_y = CONTAINER_CENTER
        self.touch_raw_x = 120
        self.touch_raw_y = 120
        self.last_touch_time = 0.0

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
        self.surface_mask = [False] * (self.grid_w * self.grid_h)
        self.frame = bytearray(LCD_WIDTH * LCD_HEIGHT * 2)
        self.row_buffers = [bytearray(LCD_WIDTH * 2) for _ in range(SCALE)]

        self.dx_values = [x - self.center for x in range(self.grid_w)]
        self.dy_values = [y - self.center for y in range(self.grid_h)]
        self.base_x_values = [x * SCALE * 2 for x in range(self.grid_w)]
        self.base_y_values = [y * SCALE for y in range(self.grid_h)]
        self.row_stride = LCD_WIDTH * 2
        self.sub_offsets = (-0.34, 0.0, 0.34)

        self.bg_r = [0] * (self.grid_w * self.grid_h)
        self.bg_g = [0] * (self.grid_w * self.grid_h)
        self.bg_b = [0] * (self.grid_w * self.grid_h)

        self.sim_time = 0.0
        self.audio_level = 0.0
        self.song_seed = random.random() * 1000.0
        self.next_silence_time = random.uniform(10.0, 18.0)
        self.silence_until = -1.0
        self.pool_x = self.center
        self.pool_y = self.center + self.radius * 0.50
        self.field_x = self.center
        self.field_y = self.center - self.radius * 0.16
        self.field_strength = 0.0
        self.spike_drive = 0.0
        self.release_collapse = 0.0
        self.field_phase = random.random() * math.tau
        self.breakup_drive = 0.0
        self.next_shake_time = random.uniform(5.0, 60.0)
        self.shake_duration = 0.0

        self._spawn_initial_particles()
        self._build_background_map()

    def _log(self, message):
        if self.verbose:
            print(message)

    def _spawn_initial_particles(self):
        spawn_radius_x = self.radius * 0.30
        spawn_radius_y = self.radius * 0.16
        while len(self.px) < PARTICLE_COUNT:
            angle = random.uniform(0.0, math.tau)
            rad = math.sqrt(random.random())
            x = self.pool_x + math.cos(angle) * spawn_radius_x * rad
            y = self.pool_y + math.sin(angle) * spawn_radius_y * rad
            self.px.append(x)
            self.py.append(y)
            self.vx.append((random.random() - 0.5) * 0.7)
            self.vy.append((random.random() - 0.5) * 0.7)

    def _update_field_state(self, dt, burst):
        if self.touch_active:
            target_strength = 1.0
            # Pull the magnet right toward the finger
            self.field_strength += (target_strength - self.field_strength) * FIELD_ATTACK_RATE
            
            # 0.4 lerp makes it zip closely to the finger while maintaining some physics smoothing
            self.field_x += (self.touch_x - self.field_x) * 0.4
            self.field_y += (self.touch_y - self.field_y) * 0.4
            
            self.spike_drive += (self.field_strength - self.spike_drive) * SPIKE_FIELD_RESPONSE
            self.release_collapse = 0.0
            return

        target_strength = clamp(self.audio_level * 1.12 + burst * 0.30 - 0.03, 0.0, 1.0)
        response = FIELD_ATTACK_RATE if target_strength > self.field_strength else FIELD_RELEASE_RATE
        self.field_strength += (target_strength - self.field_strength) * response

        if target_strength < self.field_strength and self.field_strength - target_strength > 0.05:
            self.release_collapse = max(self.release_collapse, (self.field_strength - target_strength) * RELEASE_COLLAPSE_FORCE)

        self.release_collapse *= RELEASE_COLLAPSE_DECAY
        self.spike_drive += (self.field_strength - self.spike_drive) * SPIKE_FIELD_RESPONSE

        t = self.sim_time
        phase = self.field_phase + t * (0.58 + 0.06 * math.sin(0.17 * t + self.song_seed))
        amp_x = self.radius * MAGNET_PATH_X_RATIO * (0.30 + 0.70 * self.field_strength)
        amp_y = self.radius * MAGNET_PATH_Y_RATIO * (0.25 + 0.75 * self.field_strength)
        target_x = self.center + math.sin(phase) * amp_x
        target_y = self.center - self.radius * 0.12 + math.sin(phase * 2.0 + 0.9) * amp_y

        self.field_x += (target_x - self.field_x) * FIELD_ORBIT_LERP
        self.field_y += (target_y - self.field_y) * FIELD_ORBIT_LERP

    def _sim_audio_level(self, t):
        if t >= self.next_silence_time:
            gap = random.uniform(0.85, 2.00)
            self.silence_until = t + gap
            self.next_silence_time = t + random.uniform(9.0, 20.0)

        if t < self.silence_until:
            return 0.0

        tempo_bpm = 112.0
        beat = 60.0 / tempo_bpm
        bar = beat * 4.0

        song_t = t % 64.0
        if song_t < 8.0:
            section = "intro"
        elif song_t < 24.0:
            section = "verse"
        elif song_t < 40.0:
            section = "chorus"
        elif song_t < 48.0:
            section = "bridge"
        else:
            section = "chorus2"

        bar_t = t % bar
        bar_idx = int(t / bar)

        def cyclic_dist(a, b, period):
            return abs((a - b + period * 0.5) % period - period * 0.5)

        def hit_train(hit_times, sharpness, exponent):
            level = 0.0
            for ht in hit_times:
                d = cyclic_dist(bar_t, ht, bar)
                env = max(0.0, 1.0 - d * sharpness)
                level = max(level, env ** exponent)
            return level

        if section == "intro":
            kick = hit_train([0.0, 2.0 * beat], 4.8, 2.0)
            snare = hit_train([2.0 * beat], 5.5, 1.6)
            hihat = hit_train([0.0, beat, 2.0 * beat, 3.0 * beat], 7.0, 2.0)
            section_gain = 0.70
        elif section == "verse":
            kick = hit_train([0.0, 1.5 * beat, 2.0 * beat], 5.8, 2.0)
            snare = hit_train([beat, 3.0 * beat], 7.0, 1.6)
            hihat = hit_train([i * 0.5 * beat for i in range(8)], 11.0, 2.2)
            section_gain = 0.92
        elif section == "bridge":
            kick = hit_train([0.0, 2.75 * beat], 4.4, 1.9)
            snare = hit_train([1.5 * beat, 3.0 * beat], 5.2, 1.6)
            hihat = hit_train([0.0, 2.0 * beat], 8.0, 2.0)
            section_gain = 0.78
        else:
            kick = hit_train([0.0, beat, 2.0 * beat, 2.5 * beat], 6.8, 2.2)
            snare = hit_train([beat, 3.0 * beat], 8.5, 1.7)
            hihat = hit_train([i * 0.25 * beat for i in range(16)], 14.0, 2.4)
            section_gain = 1.00

        fill = 0.0
        if bar_idx % 8 == 7 and bar_t > 3.0 * beat:
            fill = hit_train([3.0 * beat, 3.25 * beat, 3.5 * beat, 3.75 * beat], 16.0, 2.0)

        phrase = bar_idx % 4
        bass_freq = 1.6 + 0.15 * phrase
        bass = 0.5 + 0.5 * math.sin((t + self.song_seed) * bass_freq)
        bass = max(0.22, bass)

        energy = (
            0.04
            + kick * 0.43
            + snare * 0.21
            + hihat * 0.13
            + bass * 0.14
            + fill * 0.12
        ) * section_gain

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

                # Soft white background (~90% white)
                base = 210 + int(19 * vignette)
                grain = int(5 * (0.5 + 0.5 * math.sin(0.28 * sx + 0.37 * sy)))
                level = clamp(base + grain, 0, 255)

                idx = row + sx
                self.bg_r[idx] = level
                self.bg_g[idx] = level
                self.bg_b[idx] = level

    def init(self):
        self.display.init()
        self.display.draw_frame(bytearray([BLACK_HI, BLACK_LO] * (LCD_WIDTH * LCD_HEIGHT)))

    def _poll_touch(self):
        if not self.has_touch:
            return
        
        # The CST816S interrupts with a short pulse LOW on GPIO 4.
        # We can poll the line, but if we miss the pulse, checking I2C is safer.
        # Because I2C reading a sleeping CST816S causes timeouts, we catch OSError.
        # However, to avoid spamming I2C and hanging the OS, we will only poll I2C at most
        # once every 10-20ms when not touched, or if we *caught* the pulse!
        
        try:
            # 0x15 is the I2C address for CST816S
            data = self.i2c_bus.read_i2c_block_data(0x15, 0x01, 6)
            points = data[1]
            if points > 0:
                self.touch_active = True
                raw_x = ((data[2] & 0x0F) << 8) | data[3]
                raw_y = ((data[4] & 0x0F) << 8) | data[5]
                
                # Invert X axis to match screen orientation
                raw_x = (LCD_WIDTH - 1) - raw_x

                self.touch_raw_x = raw_x
                self.touch_raw_y = raw_y
                self.last_touch_time = time.time()
                # Convert raw screen coordinates (0-239) to physics simulation coordinates (0-48)
                self.touch_x = raw_x / SCALE
                self.touch_y = raw_y / SCALE
            else:
                self.touch_active = False
        except Exception:
            # The CST816S goes to sleep automatically and will throw an IOError
            # when I2C polled while asleep. Just mark inactive
            self.touch_active = False

    def _apply_magnetic_motion(self, dt):
        self._poll_touch()

        t = self.sim_time
        self.audio_level = self._sim_audio_level(t)

        beat_cycle = t % 4.0
        burst = 0.0
        if beat_cycle < 2.0:
            for hit_time in (0.0, 0.5, 1.0, 1.5):
                env = max(0.0, 1.0 - abs(beat_cycle - hit_time) * 10.0)
                burst = max(burst, env * env)

        self._update_field_state(dt, burst)

        audio_responsive_gain = 0.18 + 0.82 * (self.field_strength ** 0.7)
        pulse = 0.5 + 0.5 * math.sin(t * 10.5)
        magnet_speed = (MAGNET_BASE_SPEED + MAGNET_PULSE_SPEED * pulse) * audio_responsive_gain
        swirl_speed = SWIRL_SPEED * (0.12 + 0.88 * self.field_strength)

        breakup_gate = clamp((self.field_strength - BREAKUP_FIELD_THRESHOLD) / (1.0 - BREAKUP_FIELD_THRESHOLD), 0.0, 1.0)
        music_gate = clamp((self.audio_level - 0.18) / 0.82, 0.0, 1.0)
        burst_gate = clamp((burst - 0.12) / 0.88, 0.0, 1.0)
        self.breakup_drive = clamp(breakup_gate * (0.58 * music_gate + 0.42 * burst_gate), 0.0, 1.0)

        cx = sum(self.px) / max(1, len(self.px))
        cy = sum(self.py) / max(1, len(self.py))

        gravity_step = SETTLING_ACCEL * dt
        max_magnet_step = magnet_speed * dt
        max_swirl_step = swirl_speed * dt
        active_field = self.field_strength > 0.025
        reach_dx = self.field_x - self.pool_x
        reach_dy = self.field_y - self.pool_y
        reach_len = math.sqrt(reach_dx * reach_dx + reach_dy * reach_dy) + 1e-6
        reach_dir_x = reach_dx / reach_len
        reach_dir_y = reach_dy / reach_len

        for i in range(len(self.px)):
            field_dx = self.field_x - self.px[i]
            field_dy = self.field_y - self.py[i]
            field_dist = math.sqrt(field_dx * field_dx + field_dy * field_dy) + 1e-6
            to_field_x = field_dx / field_dist
            to_field_y = field_dy / field_dist

            pool_rel_x = self.px[i] - self.pool_x
            pool_rel_y = self.py[i] - self.pool_y
            along_reach = pool_rel_x * reach_dir_x + pool_rel_y * reach_dir_y
            cross_reach = abs(pool_rel_x * reach_dir_y - pool_rel_y * reach_dir_x)
            frontness = clamp(along_reach / reach_len, 0.0, 1.25)
            axis_lock = clamp(1.0 - cross_reach / max(self.radius * 0.24, 1.0), 0.0, 1.0)

            self.vy[i] += gravity_step

            pool_dx = self.pool_x - self.px[i]
            pool_dy = self.pool_y - self.py[i]
            settle_gain = 1.0 - self.field_strength
            self.vx[i] += pool_dx * dt * POOL_RETURN_X * settle_gain
            self.vy[i] += pool_dy * dt * POOL_RETURN_Y * settle_gain

            if self.release_collapse > 0.02:
                cdx = cx - self.px[i]
                cdy = cy - self.py[i]
                self.vx[i] += cdx * dt * self.release_collapse * 0.55
                self.vy[i] += cdy * dt * self.release_collapse * 0.55

            if active_field:
                far_gain = clamp(1.35 - field_dist / max(self.radius * 0.90, 1.0), 0.18, 1.35)
                lead_gain = 1.0 + SNAP_FRONT_GAIN * frontness * frontness
                stretch_gate = clamp((axis_lock - 0.32) / 0.68, 0.0, 1.0)
                stretch_gain = stretch_gate * clamp(frontness * 1.25, 0.0, 1.35)
                pull_step = max_magnet_step * far_gain * (0.95 + 0.75 * frontness)
                magnetic_accel = dt * self.field_strength * lead_gain * (46.0 + 18.0 * burst) / (1.0 + field_dist * 0.14)
                snap_drive = dt * self.field_strength * SNAP_STRETCH_GAIN * stretch_gain
                self.vx[i] += to_field_x * magnetic_accel
                self.vy[i] += to_field_y * magnetic_accel
                self.vx[i] += reach_dir_x * snap_drive
                self.vy[i] += reach_dir_y * snap_drive
                self.px[i] += to_field_x * pull_step
                self.py[i] += to_field_y * pull_step

                if 0.18 < frontness < 0.82 and axis_lock > 0.38:
                    neck_pull = dt * self.field_strength * SNAP_AXIS_PINCH * axis_lock * (1.0 - frontness)
                    self.px[i] += reach_dir_x * neck_pull
                    self.py[i] += reach_dir_y * neck_pull

                if self.breakup_drive > 0.02:
                    tangential_x = -to_field_y
                    tangential_y = to_field_x
                    phase = t * 7.2 + i * 0.037 + self.song_seed
                    fling_wave = 0.5 + 0.5 * math.sin(phase)
                    fling_gate = clamp((frontness - 0.12) / 0.88, 0.0, 1.0)
                    impulse = dt * self.breakup_drive * fling_gate * fling_wave
                    self.vx[i] += tangential_x * impulse * FLING_TANGENTIAL_GAIN
                    self.vy[i] += tangential_y * impulse * FLING_TANGENTIAL_GAIN

                    cdx = self.px[i] - cx
                    cdy = self.py[i] - cy
                    clen = math.sqrt(cdx * cdx + cdy * cdy) + 1e-6
                    radial_kick = dt * self.breakup_drive * (0.35 + 0.65 * fling_wave)
                    self.vx[i] += (cdx / clen) * radial_kick * FLING_IMPULSE_GAIN
                    self.vy[i] += (cdy / clen) * radial_kick * FLING_IMPULSE_GAIN

                angle = math.atan2(self.py[i] - self.field_y, self.px[i] - self.field_x)
            else:
                angle = math.atan2(self.py[i] - cy, self.px[i] - cx)

            star_phase = angle * STAR_POINTS
            sine_wave = 0.5 + 0.5 * math.cos(star_phase)
            cone_wave = 1.0 - abs((star_phase / math.pi) % 2.0 - 1.0)
            
            # The spikes are most pronounced near the magnet center where field lines project outwards
            # Extended from 0.45 to 0.85 so spikes emerge from the main body, not just the inner core
            toroidal_factor = clamp(1.0 - (field_dist / (self.radius * 0.85)), 0.0, 1.0)

            star_wave = sine_wave * (1.0 - toroidal_factor) + cone_wave * toroidal_factor
            sharpness = STAR_SHARPNESS * (1.0 - 0.4 * toroidal_factor)
            star_focus = star_wave ** sharpness

            if active_field and self.spike_drive > SPIKE_INSTABILITY_THRESHOLD:
                # Boost spike height substantially near the toroidal center to form true Rosensweig spikes
                spoke_boost = 1.0 + burst * (2.5 * star_focus - 0.28 * (1.0 - star_focus)) + (toroidal_factor * 4.0 * star_focus)
                
                # Make the outward spoke push VIOLENT to overpower inward magnetic pull and tear outwards
                # We need significant force to make them visible and accurate to ferrofluids
                # Toned down now that particle repulsion does most of the heavy lifting for 3D spikes
                spoke_push = dt * self.spike_drive * 45.0 * clamp(spoke_boost, 0.40, 6.00)
                
                # Shoot the spikes outwards fiercely where star_focus is high
                self.px[i] -= to_field_x * spoke_push * star_focus
                self.py[i] -= to_field_y * spoke_push * star_focus
                
                # Pinch the valleys inwards violently to thin the spikes and make them prominent
                valley_pinch = dt * self.spike_drive * 35.0 * (1.0 - star_wave)
                self.px[i] += to_field_x * valley_pinch
                self.py[i] += to_field_y * valley_pinch

                field_lateral_x = -to_field_y
                field_lateral_y = to_field_x
                # Push laterally to form thicker, more stable cone bases
                align_sign = math.sin(star_phase)
                steer = burst * dt * 2.2 * (1.0 - star_focus) + (dt * 1.5 * toroidal_factor * cone_wave)
                self.px[i] += field_lateral_x * steer * (-1.0 if align_sign > 0.0 else 1.0)
                self.py[i] += field_lateral_y * steer * (-1.0 if align_sign > 0.0 else 1.0)

                lean = dt * self.spike_drive * SPIKE_LEAN_GAIN * star_focus
                self.px[i] += to_field_x * lean
                self.py[i] += to_field_y * lean

            merge_gain = (1.0 - self.field_strength) * (0.42 + 0.58 * (1.0 - self.audio_level))
            cdx = self.px[i] - cx
            cdy = self.py[i] - cy
            clen = math.sqrt(cdx * cdx + cdy * cdy) + 1e-6
            self.px[i] -= (cdx / clen) * dt * 1.6 * merge_gain
            self.py[i] -= (cdy / clen) * dt * 1.1 * merge_gain

            if active_field and field_dist < self.radius * 0.22:
                twirl = 0.45 + 0.55 * math.sin(t * 3.2 + field_dist * 0.35)
                swirl = max_swirl_step * twirl
                self.vx[i] += -to_field_y * swirl * MAGNET_SWIRL * 0.12
                self.vy[i] += to_field_x * swirl * MAGNET_SWIRL * 0.12

            if not active_field:
                squash = clamp((self.py[i] - self.pool_y) / max(self.radius * 0.30, 1.0), -1.0, 1.0)
                self.vx[i] *= 0.992 - 0.010 * (1.0 - abs(squash))
                self.vy[i] *= 0.992

    def step(self, dt):
        self.sim_time += dt
        particle_count = len(self.px)

        if self.sim_time >= self.next_shake_time:
            self.next_shake_time = self.sim_time + random.uniform(5.0, 60.0)
            self.shake_duration = random.uniform(0.3, 0.9)
            self._log(f"[SIM] Shaking container for {self.shake_duration:.2f}s!")

        if self.shake_duration > 0.0:
            self.shake_duration -= dt
            shake_power = 600.0 * dt
            for i in range(particle_count):
                self.vx[i] += (random.random() - 0.5) * shake_power
                self.vy[i] += (random.random() - 0.5) * shake_power

        self._apply_magnetic_motion(dt)

        for i in range(particle_count):
            self.vx[i] *= VELOCITY_DAMPING
            self.vy[i] *= VELOCITY_DAMPING
            self.px[i] += self.vx[i] * dt
            self.py[i] += self.vy[i] * dt

        buckets = {}
        # Dynamic rest distance: when magnetized, particles repel each other fiercely in 2D,
        # forcing them into a lattice. The lowered splat radius then renders each particle
        # as a singular 3D Z-axis spike pointing right at the viewer!
        current_rest = PARTICLE_REST_RADIUS * (1.0 + 0.50 * self.field_strength)
        current_interaction = min(current_rest * 2.55, 3.99)
        current_inter_sq = current_interaction * current_interaction

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
                        if d2 <= 1e-8 or d2 >= current_inter_sq:
                            continue

                        dist = math.sqrt(d2)
                        nx = dx / dist
                        ny = dy / dist

                        q = 1.0 - (dist / current_interaction)
                        breakup_push = 1.0 + BREAKUP_PUSH_GAIN * self.breakup_drive
                        # When magnetic field is strong, repel fiercely to turn into individual Z-spikes!
                        pressure_mult = 1.0 + 8.5 * self.field_strength
                        push = q * q * PRESSURE_PUSH * breakup_push * pressure_mult
                        if dist < current_rest:
                            overlap = current_rest - dist
                            push += overlap * POSITION_RELAX * (0.8 + 0.2 * overlap / current_rest)
                        
                        # Dynamically increase the pressure bounds when magnetic fields act
                        # ALLOW huge push pushes so they force apart into a grid instead of collapsing!
                        push = min(push, 0.70 + 4.8 * self.field_strength)

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

                        if dist > current_rest * 0.90:
                            coh_q = 1.0 - (dist / current_interaction)
                            if coh_q > 0.20:
                                stretch_bias = SKIN_STRETCH_COHESION if dist > current_rest * 1.08 else 0.0
                                cohesion_scale = 1.0 - BREAKUP_COHESION_DROP * self.breakup_drive
                                cohesion = coh_q * (0.045 + stretch_bias) * cohesion_scale
                                self.px[i] += nx * cohesion
                                self.py[i] += ny * cohesion
                                self.px[j] -= nx * cohesion
                                self.py[j] -= ny * cohesion

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

        # Sharpen density drops into Z-spikes under strong magnetic field
        splat_r2 = 12.0 - 5.5 * self.field_strength
        splat_wt = 2.3 + 1.8 * self.field_strength

        for i in range(len(self.px)):
            x = self.px[i]
            y = self.py[i]
            ix = int(x)
            iy = int(y)
            for oy in (-2, -1, 0, 1, 2):
                sy = iy + oy
                if sy < 0 or sy >= self.grid_h:
                    continue
                for ox in (-2, -1, 0, 1, 2):
                    sx = ix + ox
                    if sx < 0 or sx >= self.grid_w:
                        continue
                    dx = (sx + 0.5) - x
                    dy = (sy + 0.5) - y
                    d2 = dx * dx + dy * dy
                    if d2 > splat_r2:
                        continue
                    weight = (1.0 - d2 / splat_r2) * splat_wt
                    self.density[sy * self.grid_w + sx] += weight

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
                self.density[row + x] = (up + mid * 1.6 + dn) / 3.6

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

        for y in range(self.grid_h):
            row = y * self.grid_w
            up_row = max(0, y - 1) * self.grid_w
            dn_row = min(self.grid_h - 1, y + 1) * self.grid_w
            for x in range(self.grid_w):
                left = self.density[row + max(0, x - 1)]
                right = self.density[row + min(self.grid_w - 1, x + 1)]
                up = self.density[up_row + x]
                dn = self.density[dn_row + x]
                support = max(left, right) + max(up, dn)
                if support > SKIN_BRIDGE_THRESHOLD:
                    bridge = (support - SKIN_BRIDGE_THRESHOLD) * SKIN_BRIDGE_GAIN
                    self.blur[row + x] = self.density[row + x] + bridge
                else:
                    self.blur[row + x] = self.density[row + x]

        for idx in range(cell_count):
            self.density[idx] = max(self.density[idx], self.blur[idx])

        # Surface-Only Displacement Algorithm: "Induce-on-Boundary" (IoB) Hexagonal Lattice
        # Apply a high-frequency geometric displacement over the density map when magnetic field is active
        # to form perfect, mathematically uniform Z-axis Rosensweig spikes that follow magnetic field lines!
        spike_scale = self.field_strength * 2.2
        if spike_scale > 0.05:
            # Hexagonal pattern frequency (density of the honeycomb)
            freq = 0.85
            for sy in range(self.grid_h):
                dy = (sy - self.field_y)
                row = sy * self.grid_w
                for sx in range(self.grid_w):
                    d = self.density[row + sx]
                    # Only displace the actual surface of the fluid blob, taper sharply at the edges
                    if d > ISO_EXIT * 0.8:
                        dx = (sx - self.field_x)
                        
                        # 3-axis standing wave interference pattern forming a honeycomb/hex grid
                        wave1 = math.cos(freq * dx)
                        wave2 = math.cos(freq * (0.5 * dx + 0.866 * dy))
                        wave3 = math.cos(freq * (-0.5 * dx + 0.866 * dy))
                        hex_pattern = wave1 + wave2 + wave3
                        
                        # Isolate the peaks of the interference to form sharp, distinct conical spikes
                        if hex_pattern > 1.2:
                            # Substantially increase the density/height exactly on the hexagonal vertices
                            spike = (hex_pattern - 1.2)
                            spike = spike * spike * 2.5
                            
                            # Soften the edges of the blob so spikes don't suddenly jut out in empty space
                            taper = min(1.0, (d - ISO_EXIT * 0.8) * 3.0)
                            
                            # Apply the magnetic surface displacement!
                            self.density[row + sx] += spike * spike_scale * taper

        for idx in range(cell_count):
            d = self.density[idx]
            if self.surface_mask[idx]:
                self.surface_mask[idx] = d > ISO_EXIT
            else:
                self.surface_mask[idx] = d > ISO_ENTER

    def _render_test_frame(self):
        frame = self.frame
        # Fill with white background
        frame[:] = bytearray([0xFF, 0xFF] * (LCD_WIDTH * LCD_HEIGHT))
        
        now = time.time()
        dt = now - self.last_touch_time
        if dt < 1.0:
            # Fade from black (0.0) to white (1.0)
            intens = int(min(1.0, dt) * 255)
            # 565 format for the grayscale intensity
            r = intens >> 3
            g = intens >> 2
            b = intens >> 3
            color_565 = (r << 11) | (g << 5) | b
            hi = (color_565 >> 8) & 0xFF
            lo = color_565 & 0xFF
            
            tx = int(self.touch_raw_x)
            ty = int(self.touch_raw_y)
            r_sq = 20 * 20
            
            start_y = max(0, ty - 20)
            end_y = min(LCD_HEIGHT, ty + 20)
            start_x = max(0, tx - 20)
            end_x = min(LCD_WIDTH, tx + 20)
            
            for y in range(start_y, end_y):
                dy_sq = (y - ty) * (y - ty)
                for x in range(start_x, end_x):
                    if dy_sq + (x - tx)**2 <= r_sq:
                        idx = (y * LCD_WIDTH + x) * 2
                        frame[idx] = hi
                        frame[idx+1] = lo
                        
        return frame

    def render_frame(self):
        if self.test_mode:
            return self._render_test_frame()

        self._rasterize_density()

        frame = self.frame
        row_buffers = self.row_buffers
        row_stride = self.row_stride
        density_map = self.density
        bg_r = self.bg_r
        bg_g = self.bg_g
        bg_b = self.bg_b
        t = self.sim_time
        pulse_r = 1.8 + 5.6 * self.field_strength

        if self.party_mode:
            phase = t * math.tau / 60.0
            pr = 0.5 + 0.5 * math.sin(phase)
            pg = 0.5 + 0.5 * math.sin(phase + math.tau / 3.0)
            pb = 0.5 + 0.5 * math.sin(phase + 2.0 * math.tau / 3.0)
            spec_r_mul = pr * 0.8
            spec_g_mul = pg * 0.8
            spec_b_mul = pb * 0.8
            bg_r_flat = int(pr * 255)
            bg_g_flat = int(pg * 255)
            bg_b_flat = int(pb * 255)
            surf_r_base = 5 + int(pr * 20)
            surf_g_base = 7 + int(pg * 20)
            surf_b_base = 11 + int(pb * 20)
        else:
            # White lights on a neutral dark liquid
            spec_r_mul = 1.0
            spec_g_mul = 1.0
            spec_b_mul = 1.0
            bg_r_flat = 0
            bg_g_flat = 0
            bg_b_flat = 0
            surf_r_base = 5
            surf_g_base = 5
            surf_b_base = 5

        for sy in range(self.grid_h):
            dy = self.dy_values[sy]
            base_y = self.base_y_values[sy]
            density_row = sy * self.grid_w
            density_up = max(0, sy - 1) * self.grid_w
            density_dn = min(self.grid_h - 1, sy + 1) * self.grid_w

            for oy in range(SCALE):
                row_buffers[oy][:] = b"\x00" * len(row_buffers[oy])

            for sx in range(self.grid_w):
                dx = self.dx_values[sx]
                base_x = self.base_x_values[sx]
                idx = density_row + sx
                density = density_map[idx]
                is_fluid = self.surface_mask[idx]

                sx_l = sx - 1 if sx > 0 else 0
                sx_r = sx + 1 if sx < self.grid_w - 1 else self.grid_w - 1
                density_l = density_map[density_row + sx_l]
                density_r = density_map[density_row + sx_r]
                density_u = density_map[density_up + sx]
                density_d = density_map[density_dn + sx]
                grad_x = density_r - density_l
                grad_y = density_d - density_u

                field_dx = sx - self.field_x
                field_dy = sy - self.field_y
                field_dist = math.sqrt(field_dx * field_dx + field_dy * field_dy)
                pulse = math.exp(-((field_dist - pulse_r) * (field_dist - pulse_r)) / 14.0)
                pulse *= (0.50 + 0.50 * math.sin(t * 12.0)) * self.field_strength

                base_waterness = smoothstep(0.002, 0.19, density)
                if is_fluid:
                    base_waterness = max(base_waterness, 0.42)
                else:
                    base_waterness *= 0.68

                normal_x = -grad_x * 0.7
                normal_y = -grad_y * 0.7
                normal_z = 1.0
                normal_len = math.sqrt(normal_x * normal_x + normal_y * normal_y + normal_z * normal_z)
                if normal_len > 1e-6:
                    normal_x /= normal_len
                    normal_y /= normal_len
                    normal_z /= normal_len

                # Restored original mostly-overhead directional light
                light_x, light_y, light_z = -0.18, -0.22, 0.96
                spec = max(0.0, normal_x * light_x + normal_y * light_y + normal_z * light_z)
                
                # Highlight steep slopes (the spikes) independent of the primary light angle
                rim = max(0.0, 1.0 - normal_z)
                rim = rim * rim * rim
                
                # Diffused specular + 20% rim light (approx 51 out of 255)
                spec_bright = int((spec ** 12) * 204.0 + rim * 51.0)

                if self.party_mode:
                    base_r = bg_r_flat
                    base_g = bg_g_flat
                    base_b = bg_b_flat
                else:
                    base_r = bg_r[idx]
                    base_g = bg_g[idx]
                    base_b = bg_b[idx]

                aa_needed = (0.10 < base_waterness < 0.55) or (abs(grad_x) + abs(grad_y) > 0.12)

                if not aa_needed:
                    if base_waterness > 0.28:
                        r = int(clamp(surf_r_base + spec_bright * spec_r_mul, 0, 255))
                        g = int(clamp(surf_g_base + spec_bright * spec_g_mul, 0, 255))
                        b = int(clamp(surf_b_base + spec_bright * spec_b_mul, 0, 255))
                    else:
                        surface_alpha = clamp(base_waterness * 3.5, 0.0, 0.993)
                        r = int(base_r * (1.0 - surface_alpha) + surf_r_base * surface_alpha)
                        g = int(base_g * (1.0 - surface_alpha) + surf_g_base * surface_alpha)
                        b = int(base_b * (1.0 - surface_alpha) + surf_b_base * surface_alpha)
                        pulse_boost = int(14 * pulse * (1.0 - base_waterness) * (1.0 - base_waterness))
                        r = clamp(r + pulse_boost, 0, 255)
                        g = clamp(g + pulse_boost, 0, 255)
                        b = clamp(b + pulse_boost, 0, 255)

                    color565 = pack_panel_color(r, g, b)
                    hi = (color565 >> 8) & 0xFF
                    lo = color565 & 0xFF
                    for oy in range(SCALE):
                        sub_row = row_buffers[oy]
                        for ox in range(SCALE):
                            px_write = base_x + ox * 2
                            sub_row[px_write] = hi
                            sub_row[px_write + 1] = lo
                else:
                    for oy, off_y in enumerate(self.sub_offsets):
                        sub_dy = dy + off_y
                        sub_row = row_buffers[oy]
                        for ox, off_x in enumerate(self.sub_offsets):
                            sub_dx = dx + off_x
                            sub_radial = math.sqrt(sub_dx * sub_dx + sub_dy * sub_dy)
                            sub_edge = 1.0 if sub_radial <= self.radius - 1.0 else smoothstep(self.radius + 0.6, self.radius - 1.0, sub_radial)
                            sub_density = density + grad_x * off_x + grad_y * off_y
                            waterness = smoothstep(0.002, 0.19, sub_density)
                            waterness = max(waterness, base_waterness * 0.85)

                            if waterness > 0.28:
                                r = clamp(surf_r_base + spec_bright * spec_r_mul, 0, 255)
                                g = clamp(surf_g_base + spec_bright * spec_g_mul, 0, 255)
                                b = clamp(surf_b_base + spec_bright * spec_b_mul, 0, 255)
                            else:
                                surface_alpha = clamp(waterness * 3.5, 0.0, 0.993)
                                r = int(base_r * (1.0 - surface_alpha) + surf_r_base * surface_alpha)
                                g = int(base_g * (1.0 - surface_alpha) + surf_g_base * surface_alpha)
                                b = int(base_b * (1.0 - surface_alpha) + surf_b_base * surface_alpha)
                                pulse_boost = int(14 * pulse * (1.0 - waterness) * (1.0 - waterness))
                                r = clamp(r + pulse_boost, 0, 255)
                                g = clamp(g + pulse_boost, 0, 255)
                                b = clamp(b + pulse_boost, 0, 255)

                            r = int(r * sub_edge + base_r * (1.0 - sub_edge))
                            g = int(g * sub_edge + base_g * (1.0 - sub_edge))
                            b = int(b * sub_edge + base_b * (1.0 - sub_edge))

                            color565 = pack_panel_color(r, g, b)
                            px_write = base_x + ox * 2
                            sub_row[px_write] = (color565 >> 8) & 0xFF
                            sub_row[px_write + 1] = color565 & 0xFF

            row_start = base_y * row_stride
            for oy in range(SCALE):
                dst = row_start + oy * row_stride
                frame[dst:dst + row_stride] = row_buffers[oy]

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

                    if self.verbose and stats_frames >= 30:
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
            if hasattr(self.display, "close"):
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
    parser.add_argument("--party", action="store_true", help="Smoothly cycle RGB colors")
    parser.add_argument("--test", action="store_true", help="Run touchscreen test mode")
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
        party_mode=args.party,
        test_mode=args.test,
    )
    demo.run()


if __name__ == "__main__":
    main()
