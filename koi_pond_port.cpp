#include "koi_pond_port.h"

#include "gpio_config.h"
#include "hal_gpio_spi.h"
#include "koi_pond_assets.h"

#if __has_include(<xtensor/xadapt.hpp>) && __has_include(<xtensor/xbuilder.hpp>) && __has_include(<xtensor/xmath.hpp>) && __has_include(<xtensor/xvectorize.hpp>)
#define KOI_POND_HAS_XTENSOR 1
#include <xtensor/xadapt.hpp>
#include <xtensor/xbuilder.hpp>
#include <xtensor/xmath.hpp>
#include <xtensor/xtensor.hpp>
#include <xtensor/xvectorize.hpp>
#else
#define KOI_POND_HAS_XTENSOR 0
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int kLcdWidth = LCD_WIDTH;
constexpr int kLcdHeight = LCD_HEIGHT;
constexpr float kTwoPi = static_cast<float>(M_PI * 2.0);
constexpr std::size_t kSinLutSize = 4096;
constexpr const char* kTouchI2CPrimary = TOUCH_I2C_BUS;
constexpr const char* kTouchI2CFallback = "/dev/i2c-3";
constexpr std::uint8_t kTouchAddr = 0x15;
constexpr std::uint32_t kSpiSpeedHz = SPI_SPEED_HZ;
constexpr std::uint8_t kGestureDoubleTap = 0x0B;
constexpr std::uint8_t kGestureLongPress = 0x0C;

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
};

struct Bounds {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct ColorRGB {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct ColorRGBA {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

struct Ripple {
    float x = 0.0F;
    float y = 0.0F;
    float radius = 0.0F;
    float alpha = 1.0F;
};

struct TouchPoint {
    Vec2 pos;
    float life = 0.0F;
};

struct FoodPellet {
    Vec2 pos;
    float radius = 2.5F;
};

float clamp_float(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

int clamp_int(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

double now_seconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

std::mt19937& global_rng() {
    static thread_local std::mt19937 rng(std::random_device{}());
    return rng;
}

template <typename Fn>
void parallel_for_rows(int height, Fn&& fn) {
    unsigned int worker_count = std::thread::hardware_concurrency();
    if (worker_count == 0U) {
        worker_count = 1U;
    }
    worker_count = std::min(worker_count, 4U);
    if (worker_count <= 1U || height < static_cast<int>(worker_count) * 24) {
        fn(0, height);
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    const int rows_per_worker = height / static_cast<int>(worker_count);
    int row_begin = 0;
    for (unsigned int worker = 0; worker < worker_count; ++worker) {
        const int row_end = (worker == worker_count - 1U) ? height : (row_begin + rows_per_worker);
        workers.emplace_back([row_begin, row_end, &fn]() {
            fn(row_begin, row_end);
        });
        row_begin = row_end;
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

const std::array<float, kSinLutSize + 1>& sin_lut() {
    static const auto lut = [] {
        std::array<float, kSinLutSize + 1> values{};
        for (std::size_t i = 0; i <= kSinLutSize; ++i) {
            const float angle = (static_cast<float>(i) / static_cast<float>(kSinLutSize)) * kTwoPi;
            values[i] = std::sin(angle);
        }
        return values;
    }();
    return lut;
}

float fast_sin(float angle) {
    float wrapped = std::fmod(angle, kTwoPi);
    if (wrapped < 0.0F) {
        wrapped += kTwoPi;
    }
    const float pos = wrapped * (static_cast<float>(kSinLutSize) / kTwoPi);
    const std::size_t idx = static_cast<std::size_t>(pos);
    const float frac = pos - static_cast<float>(idx);
    const auto& lut = sin_lut();
    const float a = lut[idx];
    const float b = lut[std::min(idx + 1, kSinLutSize)];
    return a + (b - a) * frac;
}

float random_uniform(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(global_rng());
}

int random_int(int lo, int hi) {
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(global_rng());
}

bool random_chance(float probability) {
    std::bernoulli_distribution dist(probability);
    return dist(global_rng());
}

float random_gauss(float mean, float stddev) {
    std::normal_distribution<float> dist(mean, stddev);
    return dist(global_rng());
}

template <typename T>
const T& random_choice(const std::vector<T>& items) {
    return items[static_cast<std::size_t>(random_int(0, static_cast<int>(items.size()) - 1))];
}

class RGBImage {
public:
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    RGBImage() = default;

    RGBImage(int w, int h)
        : width(w), height(h), pixels(static_cast<std::size_t>(w * h * 3), 0) {}

    RGBImage(int w, int h, const std::vector<std::uint8_t>& data)
        : width(w), height(h), pixels(data) {}

    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>((y * width + x) * 3);
    }

    ColorRGB get(int x, int y) const {
        const auto idx = index(x, y);
        return {pixels[idx], pixels[idx + 1], pixels[idx + 2]};
    }

    void set(int x, int y, const ColorRGB& color) {
        const auto idx = index(x, y);
        pixels[idx] = color.r;
        pixels[idx + 1] = color.g;
        pixels[idx + 2] = color.b;
    }
};

class RGBAImage {
public:
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    RGBAImage() = default;

    RGBAImage(int w, int h)
        : width(w), height(h), pixels(static_cast<std::size_t>(w * h * 4), 0) {}

    RGBAImage(int w, int h, const std::vector<std::uint8_t>& data)
        : width(w), height(h), pixels(data) {}

    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>((y * width + x) * 4);
    }

    ColorRGBA get(int x, int y) const {
        const auto idx = index(x, y);
        return {pixels[idx], pixels[idx + 1], pixels[idx + 2], pixels[idx + 3]};
    }

    void set(int x, int y, const ColorRGBA& color) {
        const auto idx = index(x, y);
        pixels[idx] = color.r;
        pixels[idx + 1] = color.g;
        pixels[idx + 2] = color.b;
        pixels[idx + 3] = color.a;
    }
};

ColorRGBA bilinear_sample_rgba(const RGBAImage& image, float x, float y) {
    if (x < 0.0F || y < 0.0F || x > static_cast<float>(image.width - 1) || y > static_cast<float>(image.height - 1)) {
        return {0, 0, 0, 0};
    }

    const int x0 = clamp_int(static_cast<int>(std::floor(x)), 0, image.width - 1);
    const int y0 = clamp_int(static_cast<int>(std::floor(y)), 0, image.height - 1);
    const int x1 = clamp_int(x0 + 1, 0, image.width - 1);
    const int y1 = clamp_int(y0 + 1, 0, image.height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const auto c00 = image.get(x0, y0);
    const auto c10 = image.get(x1, y0);
    const auto c01 = image.get(x0, y1);
    const auto c11 = image.get(x1, y1);

    auto interp = [&](std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) -> std::uint8_t {
        const float top = static_cast<float>(a) * (1.0F - tx) + static_cast<float>(b) * tx;
        const float bottom = static_cast<float>(c) * (1.0F - tx) + static_cast<float>(d) * tx;
        return static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(top * (1.0F - ty) + bottom * ty)), 0, 255));
    };

    return {
        interp(c00.r, c10.r, c01.r, c11.r),
        interp(c00.g, c10.g, c01.g, c11.g),
        interp(c00.b, c10.b, c01.b, c11.b),
        interp(c00.a, c10.a, c01.a, c11.a),
    };
}

RGBAImage resize_rgba(const RGBAImage& src, int new_w, int new_h) {
    RGBAImage out(new_w, new_h);
    if (new_w <= 0 || new_h <= 0) {
        return out;
    }

    const float scale_x = static_cast<float>(src.width) / static_cast<float>(new_w);
    const float scale_y = static_cast<float>(src.height) / static_cast<float>(new_h);
    for (int y = 0; y < new_h; ++y) {
        for (int x = 0; x < new_w; ++x) {
            const float src_x = (static_cast<float>(x) + 0.5F) * scale_x - 0.5F;
            const float src_y = (static_cast<float>(y) + 0.5F) * scale_y - 0.5F;
            out.set(x, y, bilinear_sample_rgba(src, src_x, src_y));
        }
    }
    return out;
}

RGBImage copy_rgb_from_asset(int w, int h, const std::uint8_t* data, std::size_t size) {
    return RGBImage(w, h, std::vector<std::uint8_t>(data, data + size));
}

RGBAImage copy_rgba_from_asset(int w, int h, const std::uint8_t* data, std::size_t size) {
    return RGBAImage(w, h, std::vector<std::uint8_t>(data, data + size));
}

RGBAImage rotate_rgba_expand(const RGBAImage& src, float degrees) {
    const float radians = degrees * static_cast<float>(M_PI) / 180.0F;
    const float cs = std::cos(radians);
    const float sn = std::sin(radians);
    const float cx = static_cast<float>(src.width) * 0.5F;
    const float cy = static_cast<float>(src.height) * 0.5F;

    const std::array<Vec2, 4> corners = {{{0.0F, 0.0F}, {static_cast<float>(src.width), 0.0F}, {0.0F, static_cast<float>(src.height)}, {static_cast<float>(src.width), static_cast<float>(src.height)}}};
    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max();
    float max_y = std::numeric_limits<float>::lowest();

    for (const auto& corner : corners) {
        const float dx = corner.x - cx;
        const float dy = corner.y - cy;
        const float rx = dx * cs - dy * sn;
        const float ry = dx * sn + dy * cs;
        min_x = std::min(min_x, rx);
        max_x = std::max(max_x, rx);
        min_y = std::min(min_y, ry);
        max_y = std::max(max_y, ry);
    }

    const int out_w = std::max(1, static_cast<int>(std::ceil(max_x - min_x)));
    const int out_h = std::max(1, static_cast<int>(std::ceil(max_y - min_y)));
    const float out_cx = static_cast<float>(out_w) * 0.5F;
    const float out_cy = static_cast<float>(out_h) * 0.5F;

    RGBAImage out(out_w, out_h);
    for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
            const float dx = static_cast<float>(x) - out_cx;
            const float dy = static_cast<float>(y) - out_cy;
            const float src_dx = dx * cs + dy * sn;
            const float src_dy = -dx * sn + dy * cs;
            const float src_x = src_dx + cx;
            const float src_y = src_dy + cy;
            out.set(x, y, bilinear_sample_rgba(src, src_x, src_y));
        }
    }
    return out;
}

void alpha_blend_pixel(RGBImage& dst, int x, int y, const ColorRGBA& src) {
    if (x < 0 || y < 0 || x >= dst.width || y >= dst.height || src.a == 0) {
        return;
    }
    const auto idx = dst.index(x, y);
    const float alpha = static_cast<float>(src.a) / 255.0F;
    dst.pixels[idx] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(dst.pixels[idx] * (1.0F - alpha) + src.r * alpha)), 0, 255));
    dst.pixels[idx + 1] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(dst.pixels[idx + 1] * (1.0F - alpha) + src.g * alpha)), 0, 255));
    dst.pixels[idx + 2] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(dst.pixels[idx + 2] * (1.0F - alpha) + src.b * alpha)), 0, 255));
}

void composite_rgba_over_rgb_at(RGBImage& dst, const RGBAImage& overlay, int ox, int oy) {
    for (int y = 0; y < overlay.height; ++y) {
        for (int x = 0; x < overlay.width; ++x) {
            alpha_blend_pixel(dst, ox + x, oy + y, overlay.get(x, y));
        }
    }
}

void paste_rgba(RGBImage& dst, const RGBAImage& sprite, int ox, int oy) {
    composite_rgba_over_rgb_at(dst, sprite, ox, oy);
}

void set_rgba_overwrite(RGBAImage& image, int x, int y, const ColorRGBA& color) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height || color.a == 0) {
        return;
    }
    image.set(x, y, color);
}

void draw_filled_ellipse(RGBAImage& image, float left, float top, float right, float bottom, const ColorRGBA& color) {
    const float cx = (left + right) * 0.5F;
    const float cy = (top + bottom) * 0.5F;
    const float rx = std::max(0.5F, (right - left) * 0.5F);
    const float ry = std::max(0.5F, (bottom - top) * 0.5F);
    const int x0 = clamp_int(static_cast<int>(std::floor(left)), 0, image.width - 1);
    const int x1 = clamp_int(static_cast<int>(std::ceil(right)), 0, image.width - 1);
    const int y0 = clamp_int(static_cast<int>(std::floor(top)), 0, image.height - 1);
    const int y1 = clamp_int(static_cast<int>(std::ceil(bottom)), 0, image.height - 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5F - cx) / rx;
            const float dy = (static_cast<float>(y) + 0.5F - cy) / ry;
            if ((dx * dx) + (dy * dy) <= 1.0F) {
                set_rgba_overwrite(image, x, y, color);
            }
        }
    }
}

bool point_in_polygon(const std::vector<Vec2>& polygon, float px, float py) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto& pi = polygon[i];
        const auto& pj = polygon[j];
        const bool intersect = ((pi.y > py) != (pj.y > py)) &&
            (px < (pj.x - pi.x) * (py - pi.y) / ((pj.y - pi.y) + 1.0e-6F) + pi.x);
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

void draw_filled_polygon(RGBAImage& image, const std::vector<Vec2>& polygon, const ColorRGBA& color) {
    if (polygon.size() < 3) {
        return;
    }
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    for (const auto& point : polygon) {
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }
    const int x0 = clamp_int(static_cast<int>(std::floor(min_x)), 0, image.width - 1);
    const int x1 = clamp_int(static_cast<int>(std::ceil(max_x)), 0, image.width - 1);
    const int y0 = clamp_int(static_cast<int>(std::floor(min_y)), 0, image.height - 1);
    const int y1 = clamp_int(static_cast<int>(std::ceil(max_y)), 0, image.height - 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (point_in_polygon(polygon, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F)) {
                set_rgba_overwrite(image, x, y, color);
            }
        }
    }
}

void draw_disc(RGBAImage& image, float cx, float cy, float radius, const ColorRGBA& color) {
    draw_filled_ellipse(image, cx - radius, cy - radius, cx + radius, cy + radius, color);
}

void draw_disc_rgb(RGBImage& image, float cx, float cy, float radius, const ColorRGBA& color) {
    const int x0 = clamp_int(static_cast<int>(std::floor(cx - radius)), 0, image.width - 1);
    const int x1 = clamp_int(static_cast<int>(std::ceil(cx + radius)), 0, image.width - 1);
    const int y0 = clamp_int(static_cast<int>(std::floor(cy - radius)), 0, image.height - 1);
    const int y1 = clamp_int(static_cast<int>(std::ceil(cy + radius)), 0, image.height - 1);
    const float r2 = radius * radius;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) + 0.5F) - cx;
            const float dy = (static_cast<float>(y) + 0.5F) - cy;
            if ((dx * dx) + (dy * dy) <= r2) {
                alpha_blend_pixel(image, x, y, color);
            }
        }
    }
}

[[maybe_unused]] void draw_line(RGBAImage& image, float x0, float y0, float x1, float y1, float width, const ColorRGBA& color) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::max(1.0F, std::sqrt(dx * dx + dy * dy));
    const int steps = std::max(1, static_cast<int>(std::ceil(length)));
    const float radius = std::max(0.5F, width * 0.5F);
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        draw_disc(image, x0 + dx * t, y0 + dy * t, radius, color);
    }
}

ColorRGBA to_rgba(const ColorRGB& color, std::uint8_t alpha = 255) {
    return {color.r, color.g, color.b, alpha};
}

class WaveshareDisplayDriver {
public:
    explicit WaveshareDisplayDriver(std::uint32_t spi_speed_hz)
        : spi_speed_hz_(spi_speed_hz) {}

    ~WaveshareDisplayDriver() {
        cleanup();
    }

    void init() {
        if (initialized_) {
            return;
        }
        if (hal_gpio_init() < 0) {
            throw std::runtime_error("Failed to initialize GPIO");
        }
        if (hal_spi_init(SPI_DEVICE, spi_speed_hz_, SPI_MODE) < 0) {
            throw std::runtime_error("Failed to initialize SPI");
        }
        hal_gpio_set_mode(LCD_CS_GPIO, GPIO_MODE_OUT);
        hal_gpio_set_mode(LCD_DC_GPIO, GPIO_MODE_OUT);
        hal_gpio_set_mode(LCD_RST_GPIO, GPIO_MODE_OUT);
        hal_gpio_set_mode(LCD_BL_GPIO, GPIO_MODE_OUT);

        gpio_set(LCD_CS_GPIO, true);
        gpio_set(LCD_DC_GPIO, false);
        set_backlight(false);

        gpio_set(LCD_RST_GPIO, false);
        hal_delay_ms(20);
        gpio_set(LCD_RST_GPIO, true);
        hal_delay_ms(120);

        write_command(0x01);
        hal_delay_ms(150);
        write_command(0x11);
        hal_delay_ms(120);
        run_init_sequence();
        write_command(0x21);
        hal_delay_ms(10);
        write_command(0x29);
        hal_delay_ms(20);
        write_black_frame();
        hal_delay_ms(20);
        set_backlight(true);
        hal_delay_ms(100);
        initialized_ = true;
    }

    void draw_frame_rgb565(const RGBImage& frame) {
        if (frame.width != kLcdWidth || frame.height != kLcdHeight) {
            throw std::runtime_error("Unexpected frame size");
        }
        set_address_window(0, 0, kLcdWidth - 1, kLcdHeight - 1);
        write_command(0x2C);
        const std::size_t pixel_count = static_cast<std::size_t>(frame.width * frame.height);
        if (frame_bytes_.size() != pixel_count * 2U) {
            frame_bytes_.resize(pixel_count * 2U);
        }
        for (std::size_t pixel = 0, src = 0, dst = 0; pixel < pixel_count; ++pixel, src += 3U, dst += 2U) {
            const std::uint16_t rgb565 = static_cast<std::uint16_t>(((frame.pixels[src] >> 3U) << 11U)
                | ((frame.pixels[src + 1U] >> 2U) << 5U)
                | (frame.pixels[src + 2U] >> 3U));
            frame_bytes_[dst] = static_cast<std::uint8_t>((rgb565 >> 8U) & 0xFFU);
            frame_bytes_[dst + 1U] = static_cast<std::uint8_t>(rgb565 & 0xFFU);
        }
        write_data(frame_bytes_.data(), frame_bytes_.size());
    }

    void cleanup() {
        if (!initialized_) {
            return;
        }
        write_black_frame();
        hal_delay_ms(20);
        set_backlight(false);
        write_command(0x28);
        hal_delay_ms(10);
        write_command(0x10);
        hal_delay_ms(120);
        hal_spi_close();
        hal_gpio_cleanup();
        initialized_ = false;
    }

private:
    std::uint32_t spi_speed_hz_ = 0;
    bool initialized_ = false;
    std::vector<std::uint8_t> frame_bytes_;

    void gpio_set(int pin, bool high) {
        hal_gpio_write(static_cast<std::uint32_t>(pin), high ? GPIO_HIGH : GPIO_LOW);
    }

    void set_backlight(bool enabled) {
        gpio_set(LCD_BL_GPIO, enabled);
    }

    void write_command(std::uint8_t cmd) {
        gpio_set(LCD_DC_GPIO, false);
        gpio_set(LCD_CS_GPIO, false);
        hal_spi_write(&cmd, 1);
        gpio_set(LCD_CS_GPIO, true);
        hal_delay_us(1000);
    }

    void write_data(const std::uint8_t* data, std::size_t len) {
        gpio_set(LCD_DC_GPIO, true);
        gpio_set(LCD_CS_GPIO, false);
        hal_spi_write(data, len);
        gpio_set(LCD_CS_GPIO, true);
    }

    void write_black_frame() {
        const std::size_t pixel_count = static_cast<std::size_t>(kLcdWidth * kLcdHeight);
        const std::size_t byte_count = pixel_count * 2U;
        if (frame_bytes_.size() != byte_count) {
            frame_bytes_.assign(byte_count, 0);
        } else {
            std::fill(frame_bytes_.begin(), frame_bytes_.end(), 0);
        }
        set_address_window(0, 0, kLcdWidth - 1, kLcdHeight - 1);
        write_command(0x2C);
        write_data(frame_bytes_.data(), frame_bytes_.size());
    }

    void write_cmd_data(std::uint8_t cmd, const std::initializer_list<std::uint8_t>& bytes) {
        write_command(cmd);
        if (!bytes.size()) {
            return;
        }
        std::vector<std::uint8_t> tmp(bytes.begin(), bytes.end());
        write_data(tmp.data(), tmp.size());
    }

    void set_address_window(int x1, int y1, int x2, int y2) {
        write_cmd_data(0x2A, {
            static_cast<std::uint8_t>((x1 >> 8) & 0xFF), static_cast<std::uint8_t>(x1 & 0xFF),
            static_cast<std::uint8_t>((x2 >> 8) & 0xFF), static_cast<std::uint8_t>(x2 & 0xFF),
        });
        write_cmd_data(0x2B, {
            static_cast<std::uint8_t>((y1 >> 8) & 0xFF), static_cast<std::uint8_t>(y1 & 0xFF),
            static_cast<std::uint8_t>((y2 >> 8) & 0xFF), static_cast<std::uint8_t>(y2 & 0xFF),
        });
    }

    void run_init_sequence() {
        const std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> init = {
            {0xEF, {}}, {0xEB, {0x14}}, {0xFE, {}}, {0xEF, {}}, {0xEB, {0x14}},
            {0x84, {0x40}}, {0x85, {0xFF}}, {0x86, {0xFF}}, {0x87, {0xFF}},
            {0x88, {0x0A}}, {0x89, {0x21}}, {0x8A, {0x00}}, {0x8B, {0x80}},
            {0x8C, {0x01}}, {0x8D, {0x01}}, {0x8E, {0xFF}}, {0x8F, {0xFF}},
            {0xB6, {0x00, 0x00}}, {0x36, {0x08}}, {0x3A, {0x05}},
            {0x90, {0x08, 0x08, 0x08, 0x08}}, {0xBD, {0x06}}, {0xBC, {0x00}},
            {0xFF, {0x60, 0x01, 0x04}}, {0xC3, {0x13}}, {0xC4, {0x13}},
            {0xC9, {0x22}}, {0xBE, {0x11}}, {0xE1, {0x10, 0x0E}},
            {0xDF, {0x21, 0x0C, 0x02}}, {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}},
            {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}}, {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}},
            {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}}, {0xED, {0x1B, 0x0B}},
            {0xAE, {0x77}}, {0xCD, {0x63}}, {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}},
            {0xE8, {0x34}}, {0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}},
            {0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}},
            {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}},
            {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}},
            {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}},
            {0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}},
            {0x98, {0x3E, 0x07}},
        };
        for (const auto& [cmd, data] : init) {
            write_command(cmd);
            if (!data.empty()) {
                write_data(data.data(), data.size());
            }
        }
    }
};

class PondAssets {
public:
    std::vector<RGBAImage> lily_sprites;
    RGBImage bg;
    std::vector<float> grid_x;
    std::vector<float> grid_y;
    std::vector<float> radial_mask;
    std::vector<float> depth_mask;
    std::vector<float> shimmer_mask;
    std::vector<float> sun_mask;

    PondAssets()
        : bg(copy_rgb_from_asset(
            koi_pond_assets::POND_BACKGROUND_WIDTH,
            koi_pond_assets::POND_BACKGROUND_HEIGHT,
            koi_pond_assets::POND_BACKGROUND_DATA.data(),
            koi_pond_assets::POND_BACKGROUND_DATA.size())) {
        lily_sprites.push_back(copy_rgba_from_asset(
            koi_pond_assets::LILYPAD_1_WIDTH,
            koi_pond_assets::LILYPAD_1_HEIGHT,
            koi_pond_assets::LILYPAD_1_DATA.data(),
            koi_pond_assets::LILYPAD_1_DATA.size()));
        lily_sprites.push_back(copy_rgba_from_asset(
            koi_pond_assets::LILYPAD_2_WIDTH,
            koi_pond_assets::LILYPAD_2_HEIGHT,
            koi_pond_assets::LILYPAD_2_DATA.data(),
            koi_pond_assets::LILYPAD_2_DATA.size()));
        lily_sprites.push_back(copy_rgba_from_asset(
            koi_pond_assets::LILYPAD_3_WIDTH,
            koi_pond_assets::LILYPAD_3_HEIGHT,
            koi_pond_assets::LILYPAD_3_DATA.data(),
            koi_pond_assets::LILYPAD_3_DATA.size()));
        const std::size_t pixel_count = static_cast<std::size_t>(kLcdWidth * kLcdHeight);
        grid_x.resize(pixel_count);
        grid_y.resize(pixel_count);
        radial_mask.resize(pixel_count);
        depth_mask.resize(pixel_count);
        shimmer_mask.resize(pixel_count);
        sun_mask.resize(pixel_count);
        for (int y = 0; y < kLcdHeight; ++y) {
            for (int x = 0; x < kLcdWidth; ++x) {
                const auto idx = static_cast<std::size_t>(y * kLcdWidth + x);
                const float xs = static_cast<float>(x);
                const float ys = static_cast<float>(y);
                grid_x[idx] = xs;
                grid_y[idx] = ys;
                const float radial_x = (xs - kLcdWidth * 0.5F) / (kLcdWidth * 0.56F);
                const float radial_y = (ys - kLcdHeight * 0.5F) / (kLcdHeight * 0.56F);
                const float radial = clamp_float(1.0F - std::sqrt(radial_x * radial_x + radial_y * radial_y), 0.0F, 1.0F);
                const float depth = 0.35F + 0.65F * clamp_float((ys - (kLcdHeight * 0.08F)) / (kLcdHeight * 0.92F), 0.0F, 1.0F);
                radial_mask[idx] = radial;
                depth_mask[idx] = depth;
                shimmer_mask[idx] = clamp_float((radial * 0.75F) + 0.25F, 0.0F, 1.0F) * depth;
                sun_mask[idx] = (0.3F + radial * 0.7F) * (0.45F + depth * 0.55F);
            }
        }
    }
};

PondAssets* g_assets = nullptr;

class Lilypad {
public:
    Vec2 pos;
    RGBAImage sprite;

    Lilypad(float x, float y, float rot, float scale)
        : pos{x, y} {
        const auto& base = random_choice(g_assets->lily_sprites);
        const int fin_w = std::max(1, static_cast<int>(std::lround(base.width * 0.4F * scale)));
        const int fin_h = std::max(1, static_cast<int>(std::lround(base.height * 0.4F * scale)));
        sprite = resize_rgba(base, fin_w, fin_h);
        if (std::fabs(rot) > 0.01F) {
            sprite = rotate_rgba_expand(sprite, rot);
        }
    }

    Bounds bounds() const {
        const int px = static_cast<int>(std::lround(pos.x - sprite.width / 2.0F));
        const int py = static_cast<int>(std::lround(pos.y - sprite.height / 2.0F));
        return {px, py, sprite.width, sprite.height};
    }

    void draw(RGBImage& img) const {
        const auto b = bounds();
        paste_rgba(img, sprite, b.x, b.y);
    }
};

struct TextureMark {
    int segment = 0;
    float side = 0.0F;
    float along = 0.0F;
    float lateral = 0.0F;
    float radius_scale = 0.0F;
    float stretch = 0.0F;
    ColorRGB color;
    std::uint8_t alpha = 0;
};

enum class HidingState {
    Normal,
    Hiding,
    Hidden,
    Returning,
};

class Koi {
public:
    Vec2 pos;
    HidingState hiding_state = HidingState::Normal;
    double hide_timer = 0.0;
    Vec2 hide_target{};
    float scared = 0.0F;
    float swim_speed_trait = 0.0F;
    bool offscreen_replacement_checked = false;
    double offscreen_since = -1.0;
    Vec2 vel{};
    int num_chunks = 10;
    float segment_dist = 3.5F;
    std::vector<float> radii;
    ColorRGB color;
    std::vector<TextureMark> texture_marks;
    std::vector<Vec2> segments;

    static float sample_swim_speed_trait() {
        float trait = 5.0F;
        for (int i = 0; i < 12; ++i) {
            trait = random_gauss(5.0F, 1.0F);
            if (trait >= 0.0F && trait <= 9.0F) {
                return trait;
            }
        }
        return clamp_float(trait, 0.0F, 9.0F);
    }

    Koi(float x, float y)
        : pos{x, y}, scared(random_uniform(0.0F, 9.0F)), swim_speed_trait(sample_swim_speed_trait()) {
        radii = {7.0F, 8.0F, 7.5F, 6.0F, 5.0F, 4.0F, 3.0F, 2.0F, 1.0F, 0.5F};
        const std::vector<ColorRGB> colors = {
            {255, 90, 40}, {220, 220, 220}, {255, 170, 50}, {240, 100, 40},
        };
        color = random_choice(colors);
        vel = {random_uniform(-1.0F, 1.0F), random_uniform(-1.0F, 1.0F)};
        normalize(vel);
        vel.x *= 2.0F;
        vel.y *= 2.0F;
        texture_marks = generate_texture_marks();
        segments.assign(static_cast<std::size_t>(num_chunks), {x, y});
    }

    static float normalize(Vec2& v) {
        const float mag = std::sqrt(v.x * v.x + v.y * v.y);
        if (mag > 0.0F) {
            v.x /= mag;
            v.y /= mag;
        }
        return mag;
    }

    float scale_from_trait(float slow_value, float baseline_value, float fast_value) const {
        const float trait = clamp_float(swim_speed_trait, 0.0F, 9.0F);
        if (trait <= 5.0F) {
            const float blend = trait / 5.0F;
            return slow_value + (baseline_value - slow_value) * blend;
        }
        const float blend = (trait - 5.0F) / 4.0F;
        return baseline_value + (fast_value - baseline_value) * blend;
    }

    float cruise_speed() const { return scale_from_trait(1.0F, 2.5F, 3.8F); }
    float cruise_turn_span() const { return scale_from_trait(0.10F, 0.40F, 0.72F); }
    float hide_speed() const { return scale_from_trait(0.28F, 0.60F, 1.00F); }
    float hide_turn_span() const { return scale_from_trait(0.18F, 0.60F, 0.95F); }
    float return_speed() const { return scale_from_trait(0.55F, 1.00F, 1.55F); }
    float return_turn_span() const { return scale_from_trait(0.18F, 0.50F, 0.80F); }
    float panic_speed_multiplier() const { return scale_from_trait(0.82F, 1.00F, 1.18F); }
    float panic_turn_response() const { return scale_from_trait(0.14F, 0.20F, 0.28F); }
    float calmness() const { return 1.0F - clamp_float(scared / 9.0F, 0.0F, 1.0F); }

    Vec2 free_swim_boundary_acceleration(float force_scale = 0.06F, float slack = 36.0F) const {
        Vec2 accel{};
        if (pos.x < -slack) {
            accel.x += (-slack - pos.x) * force_scale;
        } else if (pos.x > kLcdWidth + slack) {
            accel.x -= (pos.x - (kLcdWidth + slack)) * force_scale;
        }
        if (pos.y < -slack) {
            accel.y += (-slack - pos.y) * force_scale;
        } else if (pos.y > kLcdHeight + slack) {
            accel.y -= (pos.y - (kLcdHeight + slack)) * force_scale;
        }
        return accel;
    }

    std::vector<TextureMark> generate_texture_marks() {
        const std::vector<ColorRGB> palette = {
            {248, 244, 232}, {255, 188, 82}, {255, 132, 70}, {234, 84, 48},
            {76, 56, 48}, {34, 30, 28}, {232, 210, 120},
        };
        std::vector<TextureMark> marks;
        const int mark_count = random_int(0, 3);
        const int body_start = 2;
        const int body_end = std::max(body_start, num_chunks - 4);
        for (int i = 0; i < mark_count; ++i) {
            TextureMark mark;
            mark.segment = random_int(body_start, body_end);
            mark.side = random_chance(0.5F) ? -1.0F : 1.0F;
            mark.along = random_uniform(-0.28F, 0.15F);
            mark.lateral = random_uniform(0.10F, 0.42F);
            mark.radius_scale = std::max(0.16F, random_uniform(0.22F, 0.48F) * (1.0F - mark.segment / static_cast<float>(num_chunks + 2)));
            mark.stretch = random_uniform(0.8F, 1.35F);
            mark.color = random_choice(palette);
            mark.alpha = static_cast<std::uint8_t>(random_int(115, 205));
            marks.push_back(mark);
        }
        return marks;
    }

    Bounds render_bounds() const {
        float min_x = std::numeric_limits<float>::max();
        float min_y = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        float max_y = std::numeric_limits<float>::lowest();
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const auto& seg = segments[i];
            const float radius = radii[i];
            min_x = std::min(min_x, seg.x - radius);
            min_y = std::min(min_y, seg.y - radius);
            max_x = std::max(max_x, seg.x + radius);
            max_y = std::max(max_y, seg.y + radius);
        }
        const float margin = 24.0F;
        const int x0 = static_cast<int>(std::floor(min_x - margin));
        const int y0 = static_cast<int>(std::floor(min_y - margin));
        const int x1 = static_cast<int>(std::ceil(max_x + margin));
        const int y1 = static_cast<int>(std::ceil(max_y + margin));
        return {x0, y0, std::max(1, x1 - x0 + 1), std::max(1, y1 - y0 + 1)};
    }

    void draw_texture(RGBAImage& layer, const Vec2& origin) const {
        if (segments.size() < 2) {
            return;
        }
        for (const auto& mark : texture_marks) {
            const int seg_idx = std::min(mark.segment, num_chunks - 2);
            const auto& seg = segments[static_cast<std::size_t>(seg_idx)];
            const auto& next = segments[static_cast<std::size_t>(seg_idx + 1)];
            const float dx = next.x - seg.x;
            const float dy = next.y - seg.y;
            const float mag = std::sqrt(dx * dx + dy * dy);
            float tangent_x = 0.0F;
            float tangent_y = 0.0F;
            if (mag < 0.001F) {
                const float angle = std::atan2(vel.y, vel.x);
                tangent_x = std::cos(angle);
                tangent_y = std::sin(angle);
            } else {
                tangent_x = dx / mag;
                tangent_y = dy / mag;
            }
            const float normal_x = -tangent_y;
            const float normal_y = tangent_x;
            const float body_r = radii[static_cast<std::size_t>(seg_idx)];
            const float center_x = seg.x + origin.x + tangent_x * body_r * mark.along + normal_x * body_r * mark.lateral * mark.side;
            const float center_y = seg.y + origin.y + tangent_y * body_r * mark.along + normal_y * body_r * mark.lateral * mark.side;
            const float rx = std::max(1.2F, body_r * mark.radius_scale * mark.stretch);
            const float ry = std::max(1.0F, body_r * mark.radius_scale);
            draw_filled_ellipse(layer, center_x - rx, center_y - ry, center_x + rx, center_y + ry, {mark.color.r, mark.color.g, mark.color.b, mark.alpha});
        }
    }

    void update(const std::vector<Vec2>& flee_points, const std::vector<Lilypad>& lilypads, const std::vector<FoodPellet>* pellets, double now) {
        float speed = cruise_speed();
        float ax = 0.0F;
        float ay = 0.0F;

        if (!flee_points.empty() && (hiding_state == HidingState::Normal || hiding_state == HidingState::Returning)) {
            hiding_state = HidingState::Hiding;
            const auto touch = flee_points.front();
            if (scared <= 1.0F) {
                hide_target = {touch.x + random_uniform(-10.0F, 10.0F), touch.y + random_uniform(-10.0F, 10.0F)};
            } else if (scared < 5.0F) {
                const float angle = std::atan2(pos.y - touch.y, pos.x - touch.x) + random_uniform(-0.5F, 0.5F);
                const float dist = 60.0F + scared * 10.0F;
                hide_target.x = clamp_float(pos.x + std::cos(angle) * dist, 20.0F, static_cast<float>(kLcdWidth - 20));
                hide_target.y = clamp_float(pos.y + std::sin(angle) * dist, 20.0F, static_cast<float>(kLcdHeight - 20));
            } else {
                if (!lilypads.empty() && random_uniform(0.0F, 1.0F) > 0.3F) {
                    const auto& pad = lilypads[static_cast<std::size_t>(random_int(0, static_cast<int>(lilypads.size()) - 1))];
                    hide_target = {pad.pos.x + random_uniform(-10.0F, 10.0F), pad.pos.y + random_uniform(-10.0F, 10.0F)};
                } else {
                    const float angle = std::atan2(pos.y - touch.y, pos.x - touch.x) + random_uniform(-0.5F, 0.5F);
                    const float dist = (kLcdWidth / 2.0F) + (scared / 9.0F) * static_cast<float>(kLcdWidth);
                    hide_target = {pos.x + std::cos(angle) * dist, pos.y + std::sin(angle) * dist};
                }
            }
        }

        const FoodPellet* pellet_target = nullptr;
        float pellet_dist = std::numeric_limits<float>::max();
        if (pellets != nullptr && !pellets->empty() && (hiding_state == HidingState::Normal || hiding_state == HidingState::Returning)) {
            for (const auto& pellet : *pellets) {
                const float dx = pellet.pos.x - pos.x;
                const float dy = pellet.pos.y - pos.y;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist < pellet_dist) {
                    pellet_dist = dist;
                    pellet_target = &pellet;
                }
            }
        }

        if (hiding_state == HidingState::Normal) {
            if (pellet_target != nullptr && pellet_dist > 0.001F) {
                const float calm = calmness();
                const float dx = pellet_target->pos.x - pos.x;
                const float dy = pellet_target->pos.y - pos.y;
                const float current_angle = std::atan2(vel.y, vel.x);
                const float target_angle = std::atan2(dy, dx);
                const float diff = std::fmod((target_angle - current_angle + static_cast<float>(M_PI) * 3.0F), static_cast<float>(M_PI) * 2.0F) - static_cast<float>(M_PI);
                const float jitter_span = 0.03F + (1.0F - calm) * 0.10F;
                const float new_angle = current_angle + diff * (0.10F + calm * 0.22F) + random_uniform(-jitter_span, jitter_span);
                speed = cruise_speed() * (0.72F + calm * 0.98F);
                const Vec2 boundary_accel = free_swim_boundary_acceleration(0.055F, 42.0F);
                ax += boundary_accel.x * 0.45F;
                ay += boundary_accel.y * 0.45F;
                vel = {std::cos(new_angle) * speed + ax, std::sin(new_angle) * speed + ay};
            } else {
                const float wander_angle = random_uniform(-cruise_turn_span(), cruise_turn_span());
                const float current_angle = std::atan2(vel.y, vel.x);
                const float new_angle = current_angle + wander_angle;
                const Vec2 boundary_accel = free_swim_boundary_acceleration(0.065F, 38.0F);
                ax += boundary_accel.x;
                ay += boundary_accel.y;
                vel = {std::cos(new_angle) * speed + ax, std::sin(new_angle) * speed + ay};
            }
        } else if (hiding_state == HidingState::Hiding) {
            speed = (3.0F + (scared / 9.0F) * 5.0F) * panic_speed_multiplier();
            const float dx = hide_target.x - pos.x;
            const float dy = hide_target.y - pos.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < 20.0F) {
                hiding_state = HidingState::Hidden;
                hide_timer = now + (scared / 9.0F) * 10.0;
                speed = hide_speed();
            } else {
                const float tx = dx / dist;
                const float ty = dy / dist;
                const float current_angle = std::atan2(vel.y, vel.x);
                const float target_angle = std::atan2(ty, tx);
                const float diff = std::fmod((target_angle - current_angle + static_cast<float>(M_PI) * 3.0F), static_cast<float>(M_PI) * 2.0F) - static_cast<float>(M_PI);
                const float new_angle = current_angle + diff * panic_turn_response();
                vel = {std::cos(new_angle) * speed, std::sin(new_angle) * speed};
            }
        } else if (hiding_state == HidingState::Hidden) {
            speed = hide_speed();
            const float wander_angle = random_uniform(-hide_turn_span(), hide_turn_span());
            const float current_angle = std::atan2(vel.y, vel.x);
            const float new_angle = current_angle + wander_angle;
            const float dx = hide_target.x - pos.x;
            const float dy = hide_target.y - pos.y;
            const float dist_to_target = std::sqrt(dx * dx + dy * dy);
            if (dist_to_target > 15.0F) {
                ax += (dx / std::max(1.0F, dist_to_target)) * 0.1F;
                ay += (dy / std::max(1.0F, dist_to_target)) * 0.1F;
            }
            vel = {std::cos(new_angle) * speed + ax, std::sin(new_angle) * speed + ay};
            if (now > hide_timer) {
                hiding_state = HidingState::Returning;
                speed = return_speed();
            }
        } else {
            if (pellet_target != nullptr && pellet_dist > 0.001F) {
                const float calm = calmness();
                const float dx = pellet_target->pos.x - pos.x;
                const float dy = pellet_target->pos.y - pos.y;
                const float current_angle = std::atan2(vel.y, vel.x);
                const float target_angle = std::atan2(dy, dx);
                const float diff = std::fmod((target_angle - current_angle + static_cast<float>(M_PI) * 3.0F), static_cast<float>(M_PI) * 2.0F) - static_cast<float>(M_PI);
                const float jitter_span = 0.02F + (1.0F - calm) * 0.08F;
                const float new_angle = current_angle + diff * (0.12F + calm * 0.20F) + random_uniform(-jitter_span, jitter_span);
                speed = std::max(return_speed(), cruise_speed() * (0.64F + calm * 0.82F));
                const Vec2 boundary_accel = free_swim_boundary_acceleration(0.09F, 30.0F);
                ax += boundary_accel.x * 0.55F;
                ay += boundary_accel.y * 0.55F;
                vel = {std::cos(new_angle) * speed + ax, std::sin(new_angle) * speed + ay};
                if (pos.x >= 0.0F && pos.x <= kLcdWidth && pos.y >= 0.0F && pos.y <= kLcdHeight) {
                    hiding_state = HidingState::Normal;
                }
            } else {
                speed = return_speed();
                const float wander_angle = random_uniform(-return_turn_span(), return_turn_span());
                const float current_angle = std::atan2(vel.y, vel.x);
                const float new_angle = current_angle + wander_angle;
                const float cx = kLcdWidth * 0.5F;
                const float cy = kLcdHeight * 0.5F;
                const float dx = cx - pos.x;
                const float dy = cy - pos.y;
                const float dist_center = std::sqrt(dx * dx + dy * dy);
                const Vec2 boundary_accel = free_swim_boundary_acceleration(0.11F, 28.0F);
                ax += boundary_accel.x + dx * 0.01F;
                ay += boundary_accel.y + dy * 0.01F;
                if (pos.x >= 0.0F && pos.x <= kLcdWidth && pos.y >= 0.0F && pos.y <= kLcdHeight && dist_center < kLcdWidth / 2.0F - 50.0F) {
                    hiding_state = HidingState::Normal;
                }
                vel = {std::cos(new_angle) * speed + ax, std::sin(new_angle) * speed + ay};
            }
        }

        if (speed > 0.0F) {
            normalize(vel);
            vel.x *= speed;
            vel.y *= speed;
            pos.x += vel.x;
            pos.y += vel.y;
            segments[0] = pos;
            for (std::size_t i = 1; i < segments.size(); ++i) {
                const float sdx = segments[i - 1].x - segments[i].x;
                const float sdy = segments[i - 1].y - segments[i].y;
                const float dist = std::sqrt(sdx * sdx + sdy * sdy);
                if (dist > segment_dist) {
                    segments[i].x += (sdx / dist) * (dist - segment_dist);
                    segments[i].y += (sdy / dist) * (dist - segment_dist);
                }
            }
        }
    }

    void draw_shape(
        RGBAImage& layer,
        const ColorRGBA& fill_color,
        const Vec2& offset,
        float body_scale,
        float tail_scale,
        float fin_scale,
        bool draw_eyes,
        const ColorRGBA& eye_color) const {
        const auto& last = segments.back();
        const auto& prev = segments[segments.size() - 2];
        const float ang = std::atan2(last.y - prev.y, last.x - prev.x);
        const float t_len = 14.0F * tail_scale;
        const float t_spread = 0.5F;
        const Vec2 last_pt{last.x + offset.x, last.y + offset.y};
        std::vector<Vec2> tail = {
            last_pt,
            {last_pt.x + std::cos(ang - t_spread) * t_len, last_pt.y + std::sin(ang - t_spread) * t_len},
            {last_pt.x + std::cos(ang) * (t_len * 0.3F), last_pt.y + std::sin(ang) * (t_len * 0.3F)},
            {last_pt.x + std::cos(ang + t_spread) * t_len, last_pt.y + std::sin(ang + t_spread) * t_len},
        };
        draw_filled_polygon(layer, tail, fill_color);

        const auto& f_seg = segments[2];
        const auto& f_prev = segments[1];
        const float f_ang = std::atan2(f_prev.y - f_seg.y, f_prev.x - f_seg.x);
        const float flen = 10.0F * fin_scale;
        const float f_width = 5.0F * fin_scale;
        const Vec2 f_seg_pt{f_seg.x + offset.x, f_seg.y + offset.y};
        const float base_radius = radii[2] * body_scale;

        std::vector<Vec2> fin1 = {
            f_seg_pt,
            {f_seg_pt.x + std::cos(f_ang - static_cast<float>(M_PI) / 2.0F - 0.4F) * (base_radius + flen),
             f_seg_pt.y + std::sin(f_ang - static_cast<float>(M_PI) / 2.0F - 0.4F) * (base_radius + flen)},
            {f_seg_pt.x + std::cos(f_ang) * f_width, f_seg_pt.y + std::sin(f_ang) * f_width},
        };
        draw_filled_polygon(layer, fin1, fill_color);

        std::vector<Vec2> fin2 = {
            f_seg_pt,
            {f_seg_pt.x + std::cos(f_ang + static_cast<float>(M_PI) / 2.0F + 0.4F) * (base_radius + flen),
             f_seg_pt.y + std::sin(f_ang + static_cast<float>(M_PI) / 2.0F + 0.4F) * (base_radius + flen)},
            {f_seg_pt.x + std::cos(f_ang) * f_width, f_seg_pt.y + std::sin(f_ang) * f_width},
        };
        draw_filled_polygon(layer, fin2, fill_color);

        for (int i = num_chunks - 1; i >= 0; --i) {
            const auto& seg = segments[static_cast<std::size_t>(i)];
            const float r = radii[static_cast<std::size_t>(i)] * body_scale;
            const float sx = seg.x + offset.x;
            const float sy = seg.y + offset.y;
            draw_filled_ellipse(layer, sx - r, sy - r, sx + r, sy + r, fill_color);
            if (i == 0 && draw_eyes) {
                const float h_ang = std::atan2(vel.y, vel.x);
                const float ex1 = sx + std::cos(h_ang - static_cast<float>(M_PI) / 2.5F) * (r * 0.7F);
                const float ey1 = sy + std::sin(h_ang - static_cast<float>(M_PI) / 2.5F) * (r * 0.7F);
                const float ex2 = sx + std::cos(h_ang + static_cast<float>(M_PI) / 2.5F) * (r * 0.7F);
                const float ey2 = sy + std::sin(h_ang + static_cast<float>(M_PI) / 2.5F) * (r * 0.7F);
                draw_filled_ellipse(layer, ex1 - 1.5F, ey1 - 1.5F, ex1 + 1.5F, ey1 + 1.5F, eye_color);
                draw_filled_ellipse(layer, ex2 - 1.5F, ey2 - 1.5F, ex2 + 1.5F, ey2 + 1.5F, eye_color);
            }
        }
    }

    void draw(RGBImage& img_bg) const {
        const auto bounds = render_bounds();
        const Vec2 origin{-static_cast<float>(bounds.x), -static_cast<float>(bounds.y)};

        RGBAImage shadow(bounds.w, bounds.h);
        draw_shape(shadow, {10, 28, 22, 68}, {origin.x + 4.0F, origin.y + 5.0F}, 1.08F, 0.96F, 0.88F, false, {0, 0, 0, 0});
        composite_rgba_over_rgb_at(img_bg, shadow, bounds.x, bounds.y);

        RGBAImage body(bounds.w, bounds.h);
        draw_shape(body, to_rgba(color), origin, 1.0F, 1.0F, 1.0F, true, {0, 0, 0, 255});
        draw_texture(body, origin);
        composite_rgba_over_rgb_at(img_bg, body, bounds.x, bounds.y);
    }
};

class Pond {
public:
    std::vector<Lilypad> lilypads;
    std::vector<Koi> fish;
    std::vector<Ripple> ripples;
    std::vector<TouchPoint> touch_points;
    std::vector<FoodPellet> pellets;
    double feed_cooldown_until = 0.0;
    double start_time = now_seconds();

    Pond() {
        const int lilypad_count = random_int(3, 6);
        constexpr float margin = 28.0F;
        constexpr float min_distance = 52.0F;
        constexpr int max_attempts = 24;
        lilypads.reserve(static_cast<std::size_t>(lilypad_count));

        for (int i = 0; i < lilypad_count; ++i) {
            float chosen_x = random_uniform(margin, kLcdWidth - margin);
            float chosen_y = random_uniform(margin, kLcdHeight - margin);

            for (int attempt = 0; attempt < max_attempts; ++attempt) {
                const float candidate_x = random_uniform(margin, kLcdWidth - margin);
                const float candidate_y = random_uniform(margin, kLcdHeight - margin);
                bool overlaps = false;
                for (const auto& pad : lilypads) {
                    const float dx = candidate_x - pad.pos.x;
                    const float dy = candidate_y - pad.pos.y;
                    if (std::sqrt(dx * dx + dy * dy) < min_distance) {
                        overlaps = true;
                        break;
                    }
                }
                if (!overlaps) {
                    chosen_x = candidate_x;
                    chosen_y = candidate_y;
                    break;
                }
            }

            lilypads.emplace_back(
                chosen_x,
                chosen_y,
                random_uniform(0.0F, 360.0F),
                random_uniform(0.78F, 1.25F));
        }

        for (int i = 0; i < 6; ++i) {
            fish.emplace_back(kLcdWidth * 0.5F + random_uniform(-50.0F, 50.0F), kLcdHeight * 0.5F + random_uniform(-50.0F, 50.0F));
        }
    }

    Koi spawn_koi_from_offscreen() const {
        const float margin = random_uniform(24.0F, 42.0F);
        const int edge = random_int(0, 3);
        float x = 0.0F;
        float y = 0.0F;
        if (edge == 0) {
            x = -margin;
            y = random_uniform(15.0F, kLcdHeight - 15.0F);
        } else if (edge == 1) {
            x = kLcdWidth + margin;
            y = random_uniform(15.0F, kLcdHeight - 15.0F);
        } else if (edge == 2) {
            x = random_uniform(15.0F, kLcdWidth - 15.0F);
            y = -margin;
        } else {
            x = random_uniform(15.0F, kLcdWidth - 15.0F);
            y = kLcdHeight + margin;
        }
        Koi koi(x, y);
        const float target_x = kLcdWidth * 0.5F + random_uniform(-40.0F, 40.0F);
        const float target_y = kLcdHeight * 0.5F + random_uniform(-40.0F, 40.0F);
        const float dx = target_x - x;
        const float dy = target_y - y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > 0.001F) {
            const float spawn_speed = koi.scale_from_trait(1.1F, 2.1F, 3.1F);
            koi.vel = {(dx / dist) * spawn_speed, (dy / dist) * spawn_speed};
        }
        koi.pos = {x, y};
        koi.segments.assign(static_cast<std::size_t>(koi.num_chunks), {x, y});
        return koi;
    }

    bool should_replace_offscreen_koi(Koi& koi, double now) const {
        constexpr float margin = 8.0F;
        const bool outside = koi.pos.x < -margin || koi.pos.x > kLcdWidth + margin || koi.pos.y < -margin || koi.pos.y > kLcdHeight + margin;
        if (!outside) {
            koi.offscreen_since = -1.0;
            koi.offscreen_replacement_checked = false;
            return false;
        }
        if (koi.offscreen_since < 0.0) {
            koi.offscreen_since = now;
            return false;
        }
        if (now - koi.offscreen_since < 0.8) {
            return false;
        }
        if (koi.offscreen_replacement_checked) {
            return false;
        }
        koi.offscreen_replacement_checked = true;
        return random_uniform(0.0F, 1.0F) < 0.1F;
    }

    void add_ripple(float x, float y) {
        ripples.push_back({x, y, 0.0F, 1.0F});
        touch_points.push_back({{x, y}, 0.5F});
    }

    void add_visual_ripple(float x, float y) {
        ripples.push_back({x, y, 0.0F, 1.0F});
    }

    void spawn_fish_from_gesture() {
        fish.push_back(spawn_koi_from_offscreen());
    }

    bool feed_fish(float x, float y, double now) {
        if (now < feed_cooldown_until) {
            return false;
        }

        pellets.clear();
        const int pellet_count = random_int(3, 10);
        pellets.reserve(static_cast<std::size_t>(pellet_count));
        for (int i = 0; i < pellet_count; ++i) {
            const float angle = random_uniform(0.0F, kTwoPi);
            const float dist = random_uniform(0.0F, 14.0F);
            const float px = clamp_float(x + std::cos(angle) * dist + random_uniform(-1.5F, 1.5F), 6.0F, kLcdWidth - 6.0F);
            const float py = clamp_float(y + std::sin(angle) * dist + random_uniform(-1.5F, 1.5F), 6.0F, kLcdHeight - 6.0F);
            pellets.push_back({{px, py}, random_uniform(2.0F, 3.6F)});
        }

        feed_cooldown_until = now + 15.0;
        touch_points.clear();
        add_visual_ripple(x, y);
        return true;
    }

    void draw_pellets(RGBImage& image) const {
        for (const auto& pellet : pellets) {
            draw_disc_rgb(image, pellet.pos.x + 1.2F, pellet.pos.y + 1.4F, pellet.radius + 0.6F, {18, 28, 18, 96});
            draw_disc_rgb(image, pellet.pos.x, pellet.pos.y, pellet.radius, {206, 142, 74, 220});
            draw_disc_rgb(image, pellet.pos.x - 0.7F, pellet.pos.y - 0.8F, std::max(0.8F, pellet.radius * 0.42F), {250, 226, 168, 170});
        }
    }

    void update(double now) {
        std::vector<Vec2> active_touches;
        active_touches.reserve(touch_points.size());
        for (const auto& touch : touch_points) {
            active_touches.push_back(touch.pos);
        }

        std::vector<Koi> updated;
        updated.reserve(fish.size());
        for (auto& koi : fish) {
            koi.update(active_touches, lilypads, &pellets, now);

            int eaten_idx = -1;
            float eaten_dist = std::numeric_limits<float>::max();
            for (std::size_t idx = 0; idx < pellets.size(); ++idx) {
                const float dx = pellets[idx].pos.x - koi.pos.x;
                const float dy = pellets[idx].pos.y - koi.pos.y;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist <= pellets[idx].radius + 8.0F && dist < eaten_dist) {
                    eaten_idx = static_cast<int>(idx);
                    eaten_dist = dist;
                }
            }
            if (eaten_idx >= 0) {
                const auto pellet_pos = pellets[static_cast<std::size_t>(eaten_idx)].pos;
                pellets.erase(pellets.begin() + eaten_idx);
                koi.scared = std::max(0.0F, koi.scared - 0.5F);
                add_visual_ripple(pellet_pos.x, pellet_pos.y);
            }

            if (should_replace_offscreen_koi(koi, now)) {
                updated.push_back(spawn_koi_from_offscreen());
            } else {
                updated.push_back(koi);
            }
        }
        fish = std::move(updated);

        for (auto& ripple : ripples) {
            ripple.radius += 2.0F;
            ripple.alpha -= 0.02F;
        }
        ripples.erase(std::remove_if(ripples.begin(), ripples.end(), [](const Ripple& ripple) {
            return ripple.alpha <= 0.0F;
        }), ripples.end());

        for (auto& touch : touch_points) {
            touch.life -= 0.05F;
        }
        touch_points.erase(std::remove_if(touch_points.begin(), touch_points.end(), [](const TouchPoint& touch) {
            return touch.life <= 0.0F;
        }), touch_points.end());
    }

    void apply_floor_shimmer(RGBImage& image, double now) const {
        auto* pixels = image.pixels.data();
        const float now_f = static_cast<float>(now);
#if KOI_POND_HAS_XTENSOR
        static const auto sin_vec = xt::vectorize([](float angle) {
            return fast_sin(angle);
        });
        const auto xs = xt::adapt(g_assets->grid_x, {static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        const auto ys = xt::adapt(g_assets->grid_y, {static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        const auto shimmer_mask = xt::adapt(g_assets->shimmer_mask, {static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        const auto sun_mask = xt::adapt(g_assets->sun_mask, {static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        const auto drift_x = xt::eval(xs + sin_vec((ys * 0.026F) - now_f * 0.95F) * 11.0F);
        const auto drift_y = xt::eval(ys + sin_vec((xs * 0.021F) + now_f * 0.75F) * 8.0F);
        const auto wave1 = xt::eval(sin_vec(drift_x * 0.102F + now_f * 3.0F + sin_vec(drift_y * 0.054F - now_f * 1.2F) * 1.9F));
        const auto wave2 = xt::eval(sin_vec((drift_x + drift_y) * 0.064F - now_f * 3.5F));
        const auto wave3 = xt::eval(sin_vec(xt::sqrt(xt::square(drift_x - kLcdWidth * 0.58F) + xt::square(drift_y - kLcdHeight * 0.22F)) * 0.12F - now_f * 2.4F));
        const auto wave4 = xt::eval(sin_vec((drift_x * 0.036F) - (drift_y * 0.071F) + now_f * 1.6F));
        auto caustic = xt::eval(xt::maximum(0.0F, wave1 + wave2 * 0.82F + wave3 * 0.72F + wave4 * 0.45F - 1.05F));
        caustic = xt::eval(xt::pow(caustic, 1.48F) * shimmer_mask * 56.0F);
        auto sun_glow = xt::eval(xt::maximum(0.0F,
            sin_vec((xs * 0.031F) - (ys * 0.043F) + now_f * 1.8F) +
            sin_vec((xs * 0.018F) + (ys * 0.026F) - now_f * 1.15F) - 0.1F));
        sun_glow = xt::eval(xt::pow(sun_glow, 1.65F) * sun_mask * 16.0F);

        const auto* caustic_data = caustic.data();
        const auto* sun_glow_data = sun_glow.data();
        const std::size_t pixel_count = static_cast<std::size_t>(kLcdWidth * kLcdHeight);
        for (std::size_t idx = 0, pixel_idx = 0; idx < pixel_count; ++idx, pixel_idx += 3U) {
            const float caustic_value = caustic_data[idx];
            const float sun_glow_value = sun_glow_data[idx];
            pixels[pixel_idx] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(pixels[pixel_idx] + caustic_value * 0.44F + sun_glow_value * 0.32F)), 0, 255));
            pixels[pixel_idx + 1U] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(pixels[pixel_idx + 1U] + caustic_value * 0.92F + sun_glow_value * 0.58F)), 0, 255));
            pixels[pixel_idx + 2U] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(pixels[pixel_idx + 2U] + caustic_value * 0.66F + sun_glow_value * 0.40F)), 0, 255));
        }
#else
        parallel_for_rows(kLcdHeight, [&](int row_begin, int row_end) {
            for (int y = row_begin; y < row_end; ++y) {
                for (int x = 0; x < kLcdWidth; ++x) {
                    const auto idx = static_cast<std::size_t>(y * kLcdWidth + x);
                    const auto pixel_idx = idx * 3U;
                    const float xs = g_assets->grid_x[idx];
                    const float ys = g_assets->grid_y[idx];
                    const float drift_x = xs + fast_sin((ys * 0.026F) - now_f * 0.95F) * 11.0F;
                    const float drift_y = ys + fast_sin((xs * 0.021F) + now_f * 0.75F) * 8.0F;
                    const float wave1 = fast_sin(drift_x * 0.102F + now_f * 3.0F + fast_sin(drift_y * 0.054F - now_f * 1.2F) * 1.9F);
                    const float wave2 = fast_sin((drift_x + drift_y) * 0.064F - now_f * 3.5F);
                    const float wave3 = fast_sin(std::sqrt((drift_x - kLcdWidth * 0.58F) * (drift_x - kLcdWidth * 0.58F) + (drift_y - kLcdHeight * 0.22F) * (drift_y - kLcdHeight * 0.22F)) * 0.12F - now_f * 2.4F);
                    const float wave4 = fast_sin((drift_x * 0.036F) - (drift_y * 0.071F) + now_f * 1.6F);
                    float caustic = std::max(0.0F, wave1 + wave2 * 0.82F + wave3 * 0.72F + wave4 * 0.45F - 1.05F);
                    caustic = std::pow(caustic, 1.48F) * g_assets->shimmer_mask[idx] * 56.0F;
                    float sun_glow = std::max(0.0F,
                        fast_sin((xs * 0.031F) - (ys * 0.043F) + now_f * 1.8F) +
                        fast_sin((xs * 0.018F) + (ys * 0.026F) - now_f * 1.15F) - 0.1F);
                    sun_glow = std::pow(sun_glow, 1.65F) * g_assets->sun_mask[idx] * 16.0F;
                    pixels[pixel_idx] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(pixels[pixel_idx] + caustic * 0.44F + sun_glow * 0.32F)), 0, 255));
                    pixels[pixel_idx + 1U] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(pixels[pixel_idx + 1U] + caustic * 0.92F + sun_glow * 0.58F)), 0, 255));
                    pixels[pixel_idx + 2U] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(pixels[pixel_idx + 2U] + caustic * 0.66F + sun_glow * 0.40F)), 0, 255));
                }
            }
        });
#endif
    }

    RGBImage apply_ripple_distortion(const RGBImage& input, double now) const {
        if (ripples.empty()) {
            return input;
        }
        RGBImage out(kLcdWidth, kLcdHeight);
        const auto* src = input.pixels.data();
        auto* dst = out.pixels.data();
        const float now_f = static_cast<float>(now);
#if KOI_POND_HAS_XTENSOR
        static const auto sin_vec = xt::vectorize([](float angle) {
            return fast_sin(angle);
        });
        const auto xs = xt::adapt(g_assets->grid_x, {static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        const auto ys = xt::adapt(g_assets->grid_y, {static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        xt::xtensor<float, 2> offset_x = xt::zeros<float>({static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        xt::xtensor<float, 2> offset_y = xt::zeros<float>({static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        xt::xtensor<float, 2> highlight = xt::zeros<float>({static_cast<std::size_t>(kLcdHeight), static_cast<std::size_t>(kLcdWidth)});
        for (const auto& ripple : ripples) {
            const auto dx = xt::eval(xs - ripple.x);
            const auto dy = xt::eval(ys - ripple.y);
            const auto dist = xt::eval(xt::sqrt(dx * dx + dy * dy));
            const auto safe_dist = xt::eval(xt::maximum(dist, 1.0F));
            const float ring_width = 5.5F + ripple.radius * 0.02F;
            const float denom = 2.0F * ring_width * ring_width;
            const auto ring = xt::eval(xt::exp(-(xt::square(dist - ripple.radius)) / denom));
            const auto wave = xt::eval(sin_vec((dist - ripple.radius) * 0.85F - now_f * 10.0F));
            const auto strength = xt::eval(ring * wave * ripple.alpha * (3.5F + ripple.radius * 0.015F));
            offset_x += (dx / safe_dist) * strength;
            offset_y += (dy / safe_dist) * strength * 0.7F;
            highlight += ring * xt::clip(wave, 0.0F, 1.0F) * ripple.alpha * 18.0F;
        }

        const auto* offset_x_data = offset_x.data();
        const auto* offset_y_data = offset_y.data();
        const auto* highlight_data = highlight.data();
        for (int y = 0; y < kLcdHeight; ++y) {
            for (int x = 0; x < kLcdWidth; ++x) {
                const auto idx = static_cast<std::size_t>(y * kLcdWidth + x);
                const int sx = clamp_int(static_cast<int>(std::lround(static_cast<float>(x) - offset_x_data[idx])), 0, kLcdWidth - 1);
                const int sy = clamp_int(static_cast<int>(std::lround(static_cast<float>(y) - offset_y_data[idx])), 0, kLcdHeight - 1);
                const auto sample_idx = static_cast<std::size_t>((sy * kLcdWidth + sx) * 3);
                const auto dst_idx = idx * 3U;
                const float highlight_value = highlight_data[idx];
                dst[dst_idx] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(src[sample_idx] + highlight_value * 0.30F)), 0, 255));
                dst[dst_idx + 1U] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(src[sample_idx + 1U] + highlight_value * 0.65F)), 0, 255));
                dst[dst_idx + 2U] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(src[sample_idx + 2U] + highlight_value * 0.90F)), 0, 255));
            }
        }
#else
        parallel_for_rows(kLcdHeight, [&](int row_begin, int row_end) {
            for (int y = row_begin; y < row_end; ++y) {
                for (int x = 0; x < kLcdWidth; ++x) {
                    const auto idx = static_cast<std::size_t>(y * kLcdWidth + x);
                    const float xs = g_assets->grid_x[idx];
                    const float ys = g_assets->grid_y[idx];
                    float offset_x = 0.0F;
                    float offset_y = 0.0F;
                    float highlight = 0.0F;
                    for (const auto& ripple : ripples) {
                        const float dx = xs - ripple.x;
                        const float dy = ys - ripple.y;
                        const float dist = std::sqrt(dx * dx + dy * dy);
                        const float safe_dist = std::max(dist, 1.0F);
                        const float ring_width = 5.5F + ripple.radius * 0.02F;
                        const float ring = std::exp(-((dist - ripple.radius) * (dist - ripple.radius)) / (2.0F * ring_width * ring_width));
                        const float wave = fast_sin((dist - ripple.radius) * 0.85F - now_f * 10.0F);
                        const float strength = ring * wave * ripple.alpha * (3.5F + ripple.radius * 0.015F);
                        offset_x += (dx / safe_dist) * strength;
                        offset_y += (dy / safe_dist) * strength * 0.7F;
                        highlight += ring * clamp_float(wave, 0.0F, 1.0F) * ripple.alpha * 18.0F;
                    }
                    const int sx = clamp_int(static_cast<int>(std::lround(xs - offset_x)), 0, kLcdWidth - 1);
                    const int sy = clamp_int(static_cast<int>(std::lround(ys - offset_y)), 0, kLcdHeight - 1);
                    const auto sample_idx = static_cast<std::size_t>((sy * kLcdWidth + sx) * 3);
                    const auto dst_idx = idx * 3U;
                    dst[dst_idx] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(src[sample_idx] + highlight * 0.30F)), 0, 255));
                    dst[dst_idx + 1U] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(src[sample_idx + 1U] + highlight * 0.65F)), 0, 255));
                    dst[dst_idx + 2U] = static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(src[sample_idx + 2U] + highlight * 0.90F)), 0, 255));
                }
            }
        });
#endif
        return out;
    }

    void apply_lilypad_ripple_reflection(RGBImage& image, double now) const {
        if (ripples.empty()) {
            return;
        }
        for (const auto& pad : lilypads) {
            const auto b = pad.bounds();
            if (b.x >= kLcdWidth || b.y >= kLcdHeight || b.x + b.w <= 0 || b.y + b.h <= 0) {
                continue;
            }
            RGBAImage overlay(b.w, b.h);
            bool any = false;
            for (int y = 0; y < b.h; ++y) {
                for (int x = 0; x < b.w; ++x) {
                    const auto sprite = pad.sprite.get(x, y);
                    const float alpha = sprite.a / 255.0F;
                    if (alpha <= 0.05F) {
                        continue;
                    }
                    const float global_x = static_cast<float>(b.x + x);
                    const float global_y = static_cast<float>(b.y + y);
                    float response = 0.0F;
                    for (const auto& ripple : ripples) {
                        const float dx = global_x - ripple.x;
                        const float dy = global_y - ripple.y;
                        const float dist = std::sqrt(dx * dx + dy * dy);
                        const float ring_width = 7.5F + ripple.radius * 0.035F;
                        const float ring = std::exp(-((dist - ripple.radius) * (dist - ripple.radius)) / (2.0F * ring_width * ring_width));
                        const float wave = fast_sin((dist - ripple.radius) * 0.62F - static_cast<float>(now) * 7.5F);
                        response += ring * wave * ripple.alpha;
                    }
                    const float rim_mask = clamp_float(alpha * (1.0F - alpha) * 5.0F, 0.0F, 1.0F);
                    const float pad_mask = clamp_float(alpha * 0.38F + rim_mask * 0.62F, 0.0F, 1.0F);
                    const float positive = clamp_float(response, 0.0F, 1.0F) * pad_mask;
                    const float negative = clamp_float(-response, 0.0F, 1.0F) * pad_mask;
                    const ColorRGBA color{
                        static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(positive * 150.0F)), 0, 255)),
                        static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(positive * 185.0F)), 0, 255)),
                        static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(positive * 175.0F)), 0, 255)),
                        static_cast<std::uint8_t>(clamp_int(static_cast<int>(std::lround(positive * 34.0F + negative * 18.0F)), 0, 255)),
                    };
                    if (color.a > 0) {
                        any = true;
                    }
                    overlay.set(x, y, color);
                }
            }
            if (any) {
                composite_rgba_over_rgb_at(image, overlay, b.x, b.y);
            }
        }
    }

    RGBImage render() const {
        const double rel_now = now_seconds() - start_time;
        RGBImage img = g_assets->bg;
        apply_floor_shimmer(img, rel_now);
        for (const auto& koi : fish) {
            koi.draw(img);
        }
        img = apply_ripple_distortion(img, rel_now);
        draw_pellets(img);
        for (const auto& pad : lilypads) {
            pad.draw(img);
        }
        apply_lilypad_ripple_reflection(img, rel_now);
        return img;
    }
};

std::atomic<int> g_touch_x{-1};
std::atomic<int> g_touch_y{-1};
std::atomic<int> g_gesture_x{-1};
std::atomic<int> g_gesture_y{-1};
std::atomic<int> g_gesture_code{0};
std::atomic<std::uint64_t> g_gesture_sequence{0};
std::atomic<bool> g_is_touched{false};
std::atomic<bool> g_stop_touch{false};
std::atomic<bool> g_should_exit{false};

void handle_exit_signal(int /*signum*/) {
    g_should_exit.store(true);
}

void touch_thread_func() {
    try {
        hal_gpio_set_mode(TOUCH_RST_GPIO, GPIO_MODE_OUT);
        hal_gpio_write(TOUCH_RST_GPIO, GPIO_LOW);
        hal_delay_ms(10);
        hal_gpio_write(TOUCH_RST_GPIO, GPIO_HIGH);
        hal_delay_ms(50);

        bool i2c_ok = hal_i2c_init(kTouchI2CPrimary) == 0;
        if (!i2c_ok) {
            i2c_ok = hal_i2c_init(kTouchI2CFallback) == 0;
        }
        if (!i2c_ok) {
            return;
        }
        hal_i2c_write_byte(kTouchAddr, 0xFE, 0x01);
        hal_i2c_write_byte(kTouchAddr, 0xFA, 0x41);

        while (!g_stop_touch.load()) {
            std::uint8_t data[6] = {0, 0, 0, 0, 0, 0};
            if (hal_i2c_read_bytes(kTouchAddr, 0x01, data, 6) == 0) {
                const int gesture = data[0];
                const int x = ((data[2] & 0x0F) << 8) | data[3];
                const int y = ((data[4] & 0x0F) << 8) | data[5];
                const int mapped_x = (kLcdWidth - 1) - x;
                if (data[1] > 0 || gesture == kGestureDoubleTap || gesture == kGestureLongPress) {
                    g_touch_x.store(mapped_x);
                    g_touch_y.store(y);
                    g_is_touched.store(data[1] > 0);

                    const double now = now_seconds();
                    static double last_double_tap_emit = -10.0;
                    static double last_long_press_emit = -10.0;
                    if (gesture == kGestureDoubleTap && now - last_double_tap_emit > 0.35) {
                        g_gesture_x.store(mapped_x);
                        g_gesture_y.store(y);
                        g_gesture_code.store(gesture);
                        g_gesture_sequence.fetch_add(1);
                        last_double_tap_emit = now;
                    } else if (gesture == kGestureLongPress && now - last_long_press_emit > 1.10) {
                        g_gesture_x.store(mapped_x);
                        g_gesture_y.store(y);
                        g_gesture_code.store(gesture);
                        g_gesture_sequence.fetch_add(1);
                        last_long_press_emit = now;
                    }
                } else {
                    g_is_touched.store(false);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        hal_i2c_close();
    } catch (...) {
        g_is_touched.store(false);
    }
}

}  // namespace

int koi_pond_run() {
    auto previous_sigint = std::signal(SIGINT, handle_exit_signal);
#ifdef SIGHUP
    auto previous_sighup = std::signal(SIGHUP, handle_exit_signal);
#endif

    std::thread touch_thread;
    try {
        std::cout << "Loading assets..." << std::endl;
        PondAssets assets;
        g_assets = &assets;

        std::cout << "Initializing display..." << std::endl;
        WaveshareDisplayDriver driver(kSpiSpeedHz);
        driver.init();

        std::cout << "Starting touch thread..." << std::endl;
        g_stop_touch.store(false);
        g_should_exit.store(false);
        touch_thread = std::thread(touch_thread_func);

        Pond pond;
        std::cout << "Running Koi pond..." << std::endl;

        bool last_touch = false;
        std::uint64_t last_gesture_sequence = 0;
        while (!g_should_exit.load()) {
            const auto frame_start = std::chrono::steady_clock::now();
            const auto gesture_sequence = g_gesture_sequence.load();
            if (gesture_sequence != last_gesture_sequence) {
                last_gesture_sequence = gesture_sequence;
                const int gesture = g_gesture_code.load();
                if (gesture == kGestureDoubleTap) {
                    pond.spawn_fish_from_gesture();
                } else if (gesture == kGestureLongPress) {
                    pond.feed_fish(static_cast<float>(g_gesture_x.load()), static_cast<float>(g_gesture_y.load()), now_seconds());
                }
            }

            const bool is_touched = g_is_touched.load();
            if (is_touched && !last_touch) {
                pond.add_ripple(static_cast<float>(g_touch_x.load()), static_cast<float>(g_touch_y.load()));
            }
            last_touch = is_touched;
            pond.update(now_seconds());
            const auto image = pond.render();
            driver.draw_frame_rgb565(image);
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - frame_start).count();
            if (elapsed < 0.033) {
                std::this_thread::sleep_for(std::chrono::duration<double>(0.033 - elapsed));
            }
        }

        g_stop_touch.store(true);
        if (touch_thread.joinable()) {
            touch_thread.join();
        }
        driver.cleanup();

        std::signal(SIGINT, previous_sigint);
#ifdef SIGHUP
        std::signal(SIGHUP, previous_sighup);
#endif
        return 0;
    } catch (const std::exception& ex) {
        g_stop_touch.store(true);
        if (touch_thread.joinable()) {
            touch_thread.join();
        }
        std::signal(SIGINT, previous_sigint);
#ifdef SIGHUP
        std::signal(SIGHUP, previous_sighup);
#endif
        std::ofstream crash_log("/tmp/koi_crash_trace.log");
        crash_log << ex.what() << '\n';
        std::cerr << ex.what() << std::endl;
        return 1;
    } catch (...) {
        g_stop_touch.store(true);
        if (touch_thread.joinable()) {
            touch_thread.join();
        }
        std::signal(SIGINT, previous_sigint);
#ifdef SIGHUP
        std::signal(SIGHUP, previous_sighup);
#endif
        std::ofstream crash_log("/tmp/koi_crash_trace.log");
        crash_log << "Unknown exception" << '\n';
        std::cerr << "Unknown exception" << std::endl;
        return 1;
    }
}
