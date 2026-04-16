#include <Arduino.h>
#include <Arduino_RouterBridge.h>
#include <Wire.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace {
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint8_t WIDTH = 128;
constexpr uint8_t HEIGHT = 64;
constexpr uint8_t PAGES = HEIGHT / 8;
constexpr size_t FRAME_BYTES = WIDTH * PAGES;
constexpr size_t I2C_CHUNK = 31;
constexpr unsigned long FRAME_INTERVAL_MS = 33;
constexpr unsigned long SYNC_INTERVAL_MS = 500;

uint8_t frameBuffer[FRAME_BYTES] = {0};

struct WatchState {
  uint32_t syncedDaySeconds = 0;
  uint32_t syncMillis = 0;
  uint16_t steps = 5500;
  uint8_t battery = 87;
  bool hasSync = false;
};

WatchState watchState;

bool sendCommandBytes(const uint8_t *bytes, size_t count) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00);
  for (size_t i = 0; i < count; ++i) {
    Wire.write(bytes[i]);
  }
  return Wire.endTransmission() == 0;
}

bool sendDataBytes(const uint8_t *bytes, size_t count) {
  while (count > 0) {
    const size_t chunk = count > I2C_CHUNK ? I2C_CHUNK : count;
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x40);
    for (size_t i = 0; i < chunk; ++i) {
      Wire.write(bytes[i]);
    }
    if (Wire.endTransmission() != 0) {
      return false;
    }
    bytes += chunk;
    count -= chunk;
  }
  return true;
}

bool setWindow(uint8_t colStart, uint8_t colEnd, uint8_t pageStart, uint8_t pageEnd) {
  const uint8_t cmds[] = {0x21, colStart, colEnd, 0x22, pageStart, pageEnd};
  return sendCommandBytes(cmds, sizeof(cmds));
}

bool clearDisplay() {
  static uint8_t zeroPage[WIDTH] = {0};
  for (uint8_t page = 0; page < PAGES; ++page) {
    if (!setWindow(0, WIDTH - 1, page, page)) {
      return false;
    }
    if (!sendDataBytes(zeroPage, sizeof(zeroPage))) {
      return false;
    }
  }
  return true;
}

bool initDisplay() {
  const uint8_t init[] = {
      0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
      0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
      0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
      0xAF,
  };
  return sendCommandBytes(init, sizeof(init));
}

bool flushFrame() {
  return setWindow(0, WIDTH - 1, 0, PAGES - 1) && sendDataBytes(frameBuffer, sizeof(frameBuffer));
}

void px(int x, int y) {
  if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
    frameBuffer[x + (y / 8) * WIDTH] |= 1u << (y & 7);
  }
}

void clearPx(int x, int y) {
  if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
    frameBuffer[x + (y / 8) * WIDTH] &= static_cast<uint8_t>(~(1u << (y & 7)));
  }
}

void fillRect(int x0, int y0, int x1, int y1) {
  if (x0 > x1) {
    int t = x0;
    x0 = x1;
    x1 = t;
  }
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      px(x, y);
    }
  }
}

void clearRect(int x0, int y0, int x1, int y1) {
  if (x0 > x1) {
    int t = x0;
    x0 = x1;
    x1 = t;
  }
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      clearPx(x, y);
    }
  }
}

void fillCircle(int cx, int cy, int r) {
  for (int y = -r; y <= r; ++y) {
    for (int x = -r; x <= r; ++x) {
      if (x * x + y * y <= r * r) {
        px(cx + x, cy + y);
      }
    }
  }
}

const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},
    {0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},
    {0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},
    {0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},
    {0x08,0x08,0x2A,0x1C,0x08},
};

void drawChar(int x, int y, char c) {
  if (c < 32 || c > 126) {
    c = '?';
  }
  const uint8_t *glyph = font5x7[c - 32];
  for (int col = 0; col < 5; ++col) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 7; ++row) {
      if (bits & (1u << row)) {
        px(x + col, y + row);
      }
    }
  }
}

void drawStr(int x, int y, const char *s) {
  while (*s) {
    drawChar(x, y, *s++);
    x += 6;
  }
}

void drawStrInv(int x, int y, const char *s) {
  while (*s) {
    char c = *s++;
    if (c < 32 || c > 126) {
      c = '?';
    }
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; ++col) {
      uint8_t bits = glyph[col];
      for (int row = 0; row < 7; ++row) {
        if (bits & (1u << row)) {
          clearPx(x + col, y + row);
        } else {
          px(x + col, y + row);
        }
      }
    }
    for (int row = 0; row < 7; ++row) {
      px(x + 5, y + row);
    }
    x += 6;
  }
}

void drawLargeChar(int x, int y, char c, int scale) {
  if (c < 32 || c > 126) {
    c = '?';
  }
  const uint8_t *glyph = font5x7[c - 32];
  for (int col = 0; col < 5; ++col) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 7; ++row) {
      if (bits & (1u << row)) {
        fillRect(x + col * scale,
                 y + row * scale,
                 x + col * scale + scale - 1,
                 y + row * scale + scale - 1);
      }
    }
  }
}

void drawLargeStr(int x, int y, const char *s, int scale) {
  while (*s) {
    drawLargeChar(x, y, *s++, scale);
    x += (5 + 1) * scale;
  }
}

uint32_t currentDaySeconds() {
  if (!watchState.hasSync) {
    return (millis() / 1000UL) % 86400UL;
  }
  const uint32_t elapsedSeconds = (millis() - watchState.syncMillis) / 1000UL;
  return (watchState.syncedDaySeconds + elapsedSeconds) % 86400UL;
}

void drawLcarsFrame(int scene) {
  fillRect(0, 0, WIDTH - 6, 14);
  for (int r = 0; r <= 7; ++r) {
    for (int dy = -r; dy <= r; ++dy) {
      for (int dx = -r; dx <= r; ++dx) {
        if (dx * dx + dy * dy <= r * r) {
          px(WIDTH - 7 + dx, 7 + dy);
        }
      }
    }
  }
  clearRect(0, 0, WIDTH - 1, 1);
  clearRect(0, 15, WIDTH - 1, 15);

  char buf[24];
  snprintf(buf, sizeof(buf), "LCARS 47-%02d", scene);
  const int textWidth = 11 * 6;
  const int textX = WIDTH - 8 - textWidth;
  const int textY = 5;
  fillRect(textX, textY, textX + textWidth, textY + 7);
  drawStrInv(textX, textY, buf);

  fillRect(0, 17, 16, HEIGHT - 1);
  clearRect(17, 17, 30, 30);
  for (int y = -8; y <= 8; ++y) {
    for (int x = -8; x <= 8; ++x) {
      if (x * x + y * y > 64 && x < 0 && y < 0) {
        px(25 + x, 25 + y);
      }
    }
  }

  clearRect(0, 30, 16, 32);
  clearRect(0, 48, 16, 50);

  char id2[5];
  snprintf(id2, sizeof(id2), "%02d", 24 + scene);
  fillRect(0, 38, 16, 45);
  drawStrInv(2, 38, id2);
}

void drawBootScreen() {
  drawLcarsFrame(0);
  drawStr(24, 24, "ROUTER LINK");
  drawStr(24, 36, "WAITING...");
}

int interpolateInt(float phase, float start, float end, int from, int to) {
  if (phase <= start) {
    return from;
  }
  if (phase >= end) {
    return to;
  }
  const float t = (phase - start) / (end - start);
  return from + static_cast<int>((to - from) * t);
}

int ecgOffset(float phase) {
  if (phase < 0.50f) {
    return 0;
  }
  if (phase < 0.58f) {
    return interpolateInt(phase, 0.50f, 0.58f, 0, -3);
  }
  if (phase < 0.62f) {
    return interpolateInt(phase, 0.58f, 0.62f, -3, 0);
  }
  if (phase < 0.66f) {
    return interpolateInt(phase, 0.62f, 0.66f, 0, -13);
  }
  if (phase < 0.70f) {
    return interpolateInt(phase, 0.66f, 0.70f, -13, 6);
  }
  if (phase < 0.78f) {
    return interpolateInt(phase, 0.70f, 0.78f, 6, 0);
  }
  return 0;
}

void drawScenes(uint32_t daySeconds, uint16_t steps, uint8_t battery, float simT, float ecgPhase, int scene) {
  const int playX = 24;
  const int playY = 18;
  const int hour = (daySeconds / 3600UL) % 24UL;
  const int minute = (daySeconds / 60UL) % 60UL;
  const int second = daySeconds % 60UL;
  char buf[32];

  switch (scene) {
    case 0:
      snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
      drawLargeStr(playX, playY + 6, buf, 3);

      snprintf(buf, sizeof(buf), "%02d", second);
      drawLargeStr(WIDTH - 24, playY + 6, buf, 1);

      drawStr(playX, playY + 34, "MAIN TIME");
      fillRect(playX, playY + 42, playX + (battery * 70) / 100, playY + 44);
      break;

    case 1: {
      drawStr(playX, playY, "BIO-MONITOR");
      const int bpm = 72 + static_cast<int>(sinf(simT) * 5.0f);
      snprintf(buf, sizeof(buf), "%d", bpm);
      drawLargeStr(playX, playY + 12, buf, 2);
      drawStr(playX + 28, playY + 20, "BPM");

      for (int x = 0; x < 60; ++x) {
        float wrapped = ecgPhase + static_cast<float>(x) / 60.0f;
        while (wrapped >= 1.0f) {
          wrapped -= 1.0f;
        }
        const int cy = playY + 22 + ecgOffset(wrapped);
        px(playX + 44 + x, cy);
        px(playX + 44 + x, cy + 1);
      }

      drawStr(playX, playY + 36, "VITALS: NOMINAL");
      break;
    }

    case 2: {
      drawStr(playX, playY, "ENGINEERING");

      drawStr(playX, playY + 12, "STP");
      int stepWidth = (static_cast<int>(steps) * 60) / 10000;
      if (stepWidth > 60) {
        stepWidth = 60;
      }
      fillRect(playX + 25, playY + 12, playX + 25 + stepWidth, playY + 18);
      for (int x = 0; x <= 60; x += 8) {
        clearRect(playX + 25 + x, playY + 12, playX + 25 + x, playY + 18);
      }

      drawStr(playX, playY + 24, "PWR");
      const int cells = (battery * 10) / 100;
      for (int i = 0; i < 10; ++i) {
        if (i < cells) {
          fillRect(playX + 25 + i * 6, playY + 24, playX + 25 + i * 6 + 4, playY + 30);
        } else {
          px(playX + 25 + i * 6, playY + 30);
          px(playX + 25 + i * 6 + 4, playY + 30);
        }
      }

      snprintf(buf, sizeof(buf), "T %02d:%02d:%02d", hour, minute, second);
      drawStr(playX, playY + 36, buf);
      break;
    }

    case 3: {
      drawStr(playX, playY, "DIAGNOSTIC");
      for (int r = 0; r < 4; ++r) {
        snprintf(buf, sizeof(buf), "0x%04X %04X", random(0x10000), random(0x10000));
        drawStr(playX, playY + 10 + r * 8, buf);
      }

      const float angle = simT * 5.0f;
      const int radarCx = WIDTH - 16;
      const int radarCy = playY + 20;
      fillCircle(radarCx, radarCy, 1);
      px(radarCx + static_cast<int>(12 * cosf(angle)), radarCy + static_cast<int>(12 * sinf(angle)));
      px(radarCx + static_cast<int>(10 * cosf(angle - 0.2f)), radarCy + static_cast<int>(10 * sinf(angle - 0.2f)));
      px(radarCx + static_cast<int>(8 * cosf(angle - 0.4f)), radarCy + static_cast<int>(8 * sinf(angle - 0.4f)));
      break;
    }
  }
}

void watchSync(uint32_t daySeconds, uint32_t steps, uint32_t battery) {
  watchState.syncedDaySeconds = daySeconds % 86400UL;
  watchState.syncMillis = millis();
  watchState.steps = steps > 65535UL ? 65535u : static_cast<uint16_t>(steps);
  if (battery > 100UL) {
    battery = 100UL;
  }
  watchState.battery = static_cast<uint8_t>(battery);
  watchState.hasSync = true;
}

int watchPing() {
  return 1;
}

int watchSyncRpc() {
  return 1;
}

void watchStateNotify(String daySeconds_str, String steps_str, String battery_str) {
  uint32_t daySeconds = daySeconds_str.toInt();
  uint32_t steps = steps_str.toInt();
  uint32_t battery = battery_str.toInt();
  watchSync(daySeconds, steps, battery);
}

void pullWatchSync(unsigned long nowMs) {
  static unsigned long lastSyncMs = 0;
  static bool firstSync = true;
  if (!firstSync && (nowMs - lastSyncMs) < SYNC_INTERVAL_MS) {
    return;
  }
  firstSync = false;
  lastSyncMs = nowMs;

  int syncOk = 0;
  const bool callOk = Bridge.call("watch_sync").result(syncOk);
  if (!callOk) {
    Monitor.println("watch pull failed");
  }
}
}  // namespace

void setup() {
  Wire.begin();
  Wire.setClock(400000);

  delay(50);
  initDisplay();
  memset(frameBuffer, 0, sizeof(frameBuffer));
  clearDisplay();
  flushFrame();

  Bridge.begin();
  Monitor.begin();
  Bridge.provide("watch_sync", watchSyncRpc);
  Bridge.provide("watch_state", watchStateNotify);
  Bridge.provide("watch_ping", watchPing);

  randomSeed(micros());
  Monitor.println("watch bridge ready");
}

void loop() {
  static unsigned long lastFrameMillis = 0;
  static unsigned long previousMillis = millis();
  static float simT = 0.0f;
  static float ecgPhase = 0.0f;

  const unsigned long nowMillis = millis();

  pullWatchSync(nowMillis);

  if (nowMillis - lastFrameMillis < FRAME_INTERVAL_MS) {
    delay(1);
    return;
  }
  lastFrameMillis = nowMillis;

  float dt = static_cast<float>(nowMillis - previousMillis) / 1000.0f;
  previousMillis = nowMillis;
  if (dt > 1.0f) {
    dt = 1.0f;
  }

  simT += dt;
  ecgPhase += dt / (60.0f / 72.0f);
  if (ecgPhase >= 1.0f) {
    ecgPhase -= 1.0f;
  }

  memset(frameBuffer, 0, sizeof(frameBuffer));
  if (!watchState.hasSync) {
    drawBootScreen();
    flushFrame();
    return;
  }

  const uint32_t daySeconds = currentDaySeconds();
  const int scene = static_cast<int>((daySeconds / 15UL) % 4UL);
  drawLcarsFrame(scene);
  drawScenes(daySeconds, watchState.steps, watchState.battery, simT, ecgPhase, scene);
  flushFrame();
}
