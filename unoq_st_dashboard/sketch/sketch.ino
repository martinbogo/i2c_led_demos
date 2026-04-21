/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-21
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * sketch.ino - ST Dashboard App Lab renderer for Arduino Uno Q (STM32 side)
 * Renders LCARS Star Trek dashboard locally. Updates telemetry via Router Bridge RPC calls.
 *
 * Build:     ./build.sh unoq (or arduino-cli compile)
 */
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
constexpr unsigned long SYNC_INTERVAL_MS = 1000;
constexpr unsigned long ALERT_HOLD_MS = 180000UL;
constexpr int SCENE_COUNT = 6;
constexpr int SCENE_SECONDS = 7;
constexpr int MAX_CORES = 4;
constexpr int HISTORY_LEN = 64;

constexpr uint8_t ALERT_UV = 1u << 0;
constexpr uint8_t ALERT_THROTTLE = 1u << 1;
constexpr uint8_t ALERT_TEMP = 1u << 2;
constexpr uint8_t ALERT_NET = 1u << 3;
constexpr uint8_t ALERT_DISK = 1u << 4;
constexpr uint8_t ALERT_MEM = 1u << 5;
constexpr uint8_t ALERT_SERVICE = 1u << 6;
constexpr uint8_t ALERT_FAN = 1u << 7;

struct DashboardState {
  int tempC = 0;
  int fanValue = -1;
  bool fanIsRPM = false;
  int armFreqMHz = 0;
  int armMaxMHz = 0;
  int coreCount = 0;
  uint16_t totalCpu10 = 0;
  uint16_t perCore10[MAX_CORES] = {0, 0, 0, 0};
  uint16_t coreFreqMHz[MAX_CORES] = {0, 0, 0, 0};
  uint16_t iowait10 = 0;
  uint16_t load1x10 = 0;
  uint16_t memUsed10 = 0;
  uint16_t memAvailMB = 0;
  uint16_t memCachedMB = 0;
  uint16_t swapUsedMB = 0;
  uint16_t zramUsedMB = 0;
  uint16_t memPsi100 = 0;
  uint16_t diskUsed10 = 0;
  uint32_t diskReadKBps = 0;
  uint32_t diskWriteKBps = 0;
  uint8_t rootTagCode = 0;
  int nvmeTempC = -1;
  uint16_t throttledMask = 0;
  uint16_t failedUnits = 0;
  bool ethUp = false;
  bool wlanUp = false;
  int ethSpeedMbps = -1;
  int wifiRssiDbm = -1000;
  uint32_t netRxKBps = 0;
  uint32_t netTxKBps = 0;
  int gatewayLatency10 = -1;
  uint16_t uptimeMinutes = 0;
  uint16_t procs = 0;
  int headroomC = 85;
  uint8_t alertCurrent = 0;
  uint8_t alertRecent = 0;
};

uint8_t frameBuffer[FRAME_BYTES] = {0};
DashboardState currentState;
DashboardState pendingState;
bool haveState = false;
bool pendingDirty = false;

uint8_t tempHist[HISTORY_LEN] = {0};
uint8_t cpuHist[HISTORY_LEN] = {0};
uint8_t memHist[HISTORY_LEN] = {0};
uint8_t ioHist[HISTORY_LEN] = {0};
uint8_t netHist[HISTORY_LEN] = {0};
uint32_t alertSeenMs[8] = {0, 0, 0, 0, 0, 0, 0, 0};
bool alertWasSeen[8] = {false, false, false, false, false, false, false, false};

int clampInt(int value, int lo, int hi) {
  if (value < lo) {
    return lo;
  }
  if (value > hi) {
    return hi;
  }
  return value;
}

void historyPush(uint8_t *hist, uint8_t value) {
  memmove(hist, hist + 1, HISTORY_LEN - 1);
  hist[HISTORY_LEN - 1] = value;
}

uint8_t rateToPct(uint32_t rateKBps, uint32_t fullScaleKBps) {
  if (fullScaleKBps == 0U) {
    return 0;
  }
  uint32_t pct = (rateKBps * 100U) / fullScaleKBps;
  if (pct > 100U) {
    pct = 100U;
  }
  return static_cast<uint8_t>(pct);
}

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

void outlineRect(int x0, int y0, int x1, int y1) {
  fillRect(x0, y0, x1, y0);
  fillRect(x0, y1, x1, y1);
  fillRect(x0, y0, x0, y1);
  fillRect(x1, y0, x1, y1);
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

void drawHBarPct(int x, int y, int w, int h, uint8_t pct) {
  outlineRect(x, y, x + w - 1, y + h - 1);
  if (w <= 2 || h <= 2) {
    return;
  }
  const int fillWidth = ((w - 2) * clampInt(pct, 0, 100)) / 100;
  if (fillWidth > 0) {
    fillRect(x + 1, y + 1, x + fillWidth, y + h - 2);
  }
}

void drawVBarPct(int x, int y, int w, int h, uint8_t pct) {
  outlineRect(x, y, x + w - 1, y + h - 1);
  if (w <= 2 || h <= 2) {
    return;
  }
  const int fillHeight = ((h - 2) * clampInt(pct, 0, 100)) / 100;
  if (fillHeight > 0) {
    fillRect(x + 1, y + h - 1 - fillHeight, x + w - 2, y + h - 2);
  }
}

void drawSparkline(int x, int y, int w, int h, const uint8_t *hist, uint8_t maxValue) {
  outlineRect(x, y, x + w - 1, y + h - 1);
  if (w <= 2 || h <= 2 || maxValue == 0U) {
    return;
  }
  const int innerWidth = w - 2;
  int start = HISTORY_LEN - innerWidth;
  if (start < 0) {
    start = 0;
  }
  for (int i = 0; i < innerWidth; ++i) {
    const int idx = start + i;
    if (idx >= HISTORY_LEN) {
      break;
    }
    const uint8_t value = hist[idx] > maxValue ? maxValue : hist[idx];
    const int barHeight = ((h - 2) * value) / maxValue;
    for (int dy = 0; dy < barHeight; ++dy) {
      px(x + 1 + i, y + h - 2 - dy);
    }
  }
}

void drawAlertPips(int x, int y, uint8_t current, uint8_t recent) {
  const uint8_t bits[] = {
      ALERT_UV, ALERT_THROTTLE, ALERT_TEMP, ALERT_NET,
      ALERT_DISK, ALERT_MEM, ALERT_SERVICE, ALERT_FAN,
  };
  for (int i = 0; i < 8; ++i) {
    const int x0 = x + i * 5;
    if (recent & bits[i]) {
      outlineRect(x0, y, x0 + 3, y + 3);
    }
    if (current & bits[i]) {
      fillRect(x0 + 1, y + 1, x0 + 2, y + 2);
    }
  }
}

const char *rootTagLabel(uint8_t code) {
  switch (code) {
    case 1: return "NVME";
    case 2: return "SD";
    case 3: return "USB";
    default: return "ROOT";
  }
}

void appendText(char *out, size_t outSize, const char *text) {
  if (outSize == 0U) {
    return;
  }
  const size_t outLen = strlen(out);
  if (outLen >= outSize - 1U) {
    out[outSize - 1U] = '\0';
    return;
  }
  size_t textLen = strlen(text);
  if (textLen > outSize - outLen - 1U) {
    textLen = outSize - outLen - 1U;
  }
  memcpy(out + outLen, text, textLen);
  out[outLen + textLen] = '\0';
}

void buildAlertSummary(uint8_t recent, char *out, size_t outSize) {
  out[0] = '\0';
  const struct { uint8_t bit; const char *tag; } tags[] = {
      {ALERT_UV, "UV"}, {ALERT_THROTTLE, "THR"}, {ALERT_TEMP, "TMP"}, {ALERT_NET, "NET"},
      {ALERT_DISK, "DSK"}, {ALERT_MEM, "MEM"}, {ALERT_SERVICE, "SVC"}, {ALERT_FAN, "FAN"},
  };
  for (int i = 0; i < 8; ++i) {
    if (recent & tags[i].bit) {
      if (out[0] != '\0') {
        appendText(out, outSize, " ");
      }
      appendText(out, outSize, tags[i].tag);
    }
  }
  if (out[0] == '\0') {
    snprintf(out, outSize, "NOMINAL");
  }
}

void formatLoad1(uint16_t load1x10, char *out, size_t outSize) {
  snprintf(out, outSize, "L%u.%u", load1x10 / 10U, load1x10 % 10U);
}

void formatCompactMB(uint32_t valueMB, char *out, size_t outSize) {
  if (valueMB >= 1024U) {
    snprintf(out, outSize, "%luG", static_cast<unsigned long>(valueMB / 1024U));
  } else {
    snprintf(out, outSize, "%luM", static_cast<unsigned long>(valueMB));
  }
}

void formatCompactKBps(uint32_t valueKBps, char *out, size_t outSize) {
  if (valueKBps >= 1024U * 1024U) {
    snprintf(out, outSize, "%luG", static_cast<unsigned long>(valueKBps / (1024U * 1024U)));
  } else if (valueKBps >= 1024U) {
    snprintf(out, outSize, "%luM", static_cast<unsigned long>(valueKBps / 1024U));
  } else {
    snprintf(out, outSize, "%luK", static_cast<unsigned long>(valueKBps));
  }
}

void touchAlertMask(unsigned long nowMs, uint8_t mask) {
  const uint8_t bits[] = {
      ALERT_UV, ALERT_THROTTLE, ALERT_TEMP, ALERT_NET,
      ALERT_DISK, ALERT_MEM, ALERT_SERVICE, ALERT_FAN,
  };
  for (int i = 0; i < 8; ++i) {
    if (mask & bits[i]) {
      alertSeenMs[i] = nowMs;
      alertWasSeen[i] = true;
    }
  }
}

uint8_t recentAlertMask(unsigned long nowMs) {
  const uint8_t bits[] = {
      ALERT_UV, ALERT_THROTTLE, ALERT_TEMP, ALERT_NET,
      ALERT_DISK, ALERT_MEM, ALERT_SERVICE, ALERT_FAN,
  };
  uint8_t mask = 0;
  for (int i = 0; i < 8; ++i) {
    if (alertWasSeen[i] && nowMs - alertSeenMs[i] <= ALERT_HOLD_MS) {
      mask |= bits[i];
    }
  }
  return mask;
}

void refreshAlertState(unsigned long nowMs) {
  uint8_t current = 0;
  if (currentState.throttledMask & 0x1u) {
    current |= ALERT_UV;
  }
  if (currentState.throttledMask & ((1u << 1) | (1u << 2) | (1u << 3))) {
    current |= ALERT_THROTTLE;
  }
  if (currentState.tempC >= 80 || currentState.headroomC <= 5) {
    current |= ALERT_TEMP;
  }
  if (!currentState.ethUp && !currentState.wlanUp) {
    current |= ALERT_NET;
  }
  if (currentState.diskUsed10 > 900U) {
    current |= ALERT_DISK;
  }
  if (currentState.memUsed10 > 920U || currentState.memPsi100 > 50U) {
    current |= ALERT_MEM;
  }
  if (currentState.failedUnits > 0U) {
    current |= ALERT_SERVICE;
  }
  if (currentState.fanValue < 0 && currentState.tempC >= 65) {
    current |= ALERT_FAN;
  }

  touchAlertMask(nowMs, current);
  currentState.alertCurrent = current;
  currentState.alertRecent = recentAlertMask(nowMs);
}

void commitPendingState() {
  if (!pendingDirty) {
    return;
  }
  currentState = pendingState;
  currentState.headroomC = 85 - currentState.tempC;
  if (currentState.headroomC < -99) {
    currentState.headroomC = -99;
  }

  const unsigned long nowMs = millis();
  refreshAlertState(nowMs);

  historyPush(tempHist, static_cast<uint8_t>(clampInt(currentState.tempC, 0, 100)));
  historyPush(cpuHist, static_cast<uint8_t>(clampInt(currentState.totalCpu10 / 10U, 0, 100)));
  historyPush(memHist, static_cast<uint8_t>(clampInt(currentState.memUsed10 / 10U, 0, 100)));
  historyPush(ioHist, rateToPct(currentState.diskReadKBps + currentState.diskWriteKBps, 100U * 1024U));
  historyPush(netHist, rateToPct(currentState.netRxKBps + currentState.netTxKBps, 125U * 1024U));

  haveState = true;
  pendingDirty = false;
}

void drawLcarsFrame(unsigned long nowMs, int scene, const char *title) {
  fillRect(0, 0, WIDTH - 12, 0);
  fillRect(0, 14, WIDTH - 12, 14);
  fillRect(0, 0, 0, 14);
  for (float a = -1.5707963f; a <= 1.5707963f; a += 0.05f) {
    px(WIDTH - 12 + static_cast<int>(7.0f * cosf(a)), 7 + static_cast<int>(7.0f * sinf(a)));
  }

  static const int titleOffsets[] = {-3, -2, 2, 3};
  static int titleJitter = -2;
  static int lastScene = -1;
  if (scene != lastScene) {
    int nextJitter = titleJitter;
    while (nextJitter == titleJitter) {
      nextJitter = titleOffsets[random(4)];
    }
    titleJitter = nextJitter;
    lastScene = scene;
  }

  const int textWidth = strlen(title) * 6;
  int textX = WIDTH - 20 - textWidth + titleJitter;
  if (textX < 50) {
    textX = 50;
  }
  drawAlertPips(4, 5, currentState.alertCurrent, currentState.alertRecent);
  drawStr(textX, 5, title);

  fillRect(0, 17, 22, 17);
  fillRect(0, 30, 14, 30);
  fillRect(0, 17, 0, 30);
  for (int y = 14; y <= 30; ++y) {
    const float t = static_cast<float>(y - 14) / 16.0f;
    const int x = 14 + static_cast<int>(8.0f * cosf(t * 1.5707963f));
    px(x, y);
  }

  outlineRect(0, 33, 14, 46);
  outlineRect(0, 49, 14, 62);

  int cpuH = (11 * clampInt(currentState.totalCpu10 / 10U, 0, 100)) / 100;
  for (int y = 29; y >= 29 - cpuH; --y) {
    fillRect(2, y, 12, y);
  }
  int ramH = (11 * clampInt(currentState.memUsed10 / 10U, 0, 100)) / 100;
  for (int y = 45; y >= 45 - ramH; --y) {
    fillRect(2, y, 12, y);
  }
  int activityPct = ioHist[HISTORY_LEN - 1];
  if (netHist[HISTORY_LEN - 1] > activityPct) {
    activityPct = netHist[HISTORY_LEN - 1];
  }
  if (currentState.alertCurrent) {
    activityPct = 100;
  }
  int actH = (11 * clampInt(activityPct, 0, 100)) / 100;
  for (int y = 61; y >= 61 - actH; --y) {
    fillRect(2, y, 12, y);
  }
  if (((nowMs / 166UL) % 2UL) == 0UL) {
    px(7, 31);
  }
}

void drawBootScreen() {
  drawLcarsFrame(millis(), 0, "WAIT LINK");
  drawStr(26, 24, "APP LAB");
  drawStr(26, 36, "WAITING");
}

void drawSceneThermalPower(unsigned long nowMs, int x, int y) {
  char buf[32];
  const int statsX = 78;
  snprintf(buf, sizeof(buf), "%dC", currentState.tempC);
  drawLargeStr(x, y, buf, 2);
  drawHBarPct(x, y + 16, 34, 6, static_cast<uint8_t>(clampInt((currentState.tempC * 100) / 85, 0, 100)));
  snprintf(buf, sizeof(buf), "HD %dC", currentState.headroomC);
  drawStr(x, y + 24, buf);
  if (currentState.fanValue < 0) {
    drawStr(x, y + 32, "FAN OFF");
  } else if (currentState.fanIsRPM) {
    snprintf(buf, sizeof(buf), "RPM %d", currentState.fanValue);
    drawStr(x, y + 32, buf);
  } else {
    snprintf(buf, sizeof(buf), "FAN %d", currentState.fanValue);
    drawStr(x, y + 32, buf);
  }
  if (currentState.alertCurrent & ALERT_UV) {
    drawStr(x, y + 40, "UV ALERT");
  } else if (currentState.alertCurrent & ALERT_THROTTLE) {
    drawStr(x, y + 40, "THROTL");
  } else {
    drawStr(x, y + 40, "PWR OK");
  }

  snprintf(buf, sizeof(buf), "A%dM", currentState.armFreqMHz);
  drawStr(statsX, y, buf);
  snprintf(buf, sizeof(buf), "M%dM", currentState.armMaxMHz);
  drawStr(statsX, y + 8, buf);
  drawStr(statsX, y + 16, "UNOQ");
  drawSparkline(statsX, y + 24, 28, 16, tempHist, 85);
  if (((nowMs / 83UL) % 2UL) == 0UL) {
    px(statsX + 24, y + 42);
  }
}

void drawSceneCpuTopology(int x, int y) {
  char buf[24];
  const int statsX = 78;
  const int barY = y + 10;
  const int barH = 20;
  const int barX0 = x + 1;

  for (int i = 0; i < currentState.coreCount && i < MAX_CORES; ++i) {
    const int bx = barX0 + i * 11;
    drawChar(bx + 1, y, static_cast<char>('0' + i));
    drawVBarPct(bx, barY, 7, barH, static_cast<uint8_t>(clampInt(currentState.perCore10[i] / 10U, 0, 100)));
    if (currentState.armMaxMHz > 0 && currentState.coreFreqMHz[i] > 0) {
      const int tickY = barY + 1 + ((barH - 3) * (currentState.armMaxMHz - currentState.coreFreqMHz[i])) / currentState.armMaxMHz;
      fillRect(bx + 1, tickY, bx + 5, tickY);
    }
  }

  snprintf(buf, sizeof(buf), "A%d", currentState.armFreqMHz);
  drawStr(statsX, y, buf);
  formatLoad1(currentState.load1x10, buf, sizeof(buf));
  drawStr(statsX, y + 8, buf);
  snprintf(buf, sizeof(buf), "I%u%%", currentState.iowait10 / 10U);
  drawStr(statsX, y + 16, buf);
  snprintf(buf, sizeof(buf), "P%u", currentState.procs);
  drawStr(statsX, y + 24, buf);
  drawSparkline(x, y + 34, 78, 12, cpuHist, 100);
}

void drawSceneMemoryPressure(int x, int y) {
  char buf[24];
  char availBuf[12];
  char usedBuf[12];
  char cachedBuf[12];
  char swapBuf[12];
  char zramBuf[12];
  const int statsX = 78;

  const uint32_t usedMB = currentState.memAvailMB == 0U && currentState.memUsed10 == 0U ? 0U : 0U;
  (void)usedMB;

  formatCompactMB(currentState.memAvailMB, availBuf, sizeof(availBuf));
  formatCompactMB((currentState.memUsed10 * (currentState.memAvailMB + 1U)) / 1000U, usedBuf, sizeof(usedBuf));
  formatCompactMB(currentState.memCachedMB, cachedBuf, sizeof(cachedBuf));
  formatCompactMB(currentState.swapUsedMB, swapBuf, sizeof(swapBuf));
  formatCompactMB(currentState.zramUsedMB, zramBuf, sizeof(zramBuf));

  drawStr(x, y, "AVAIL");
  drawLargeStr(x, y + 8, availBuf, 2);
  drawHBarPct(x, y + 26, 34, 6, static_cast<uint8_t>(clampInt(currentState.memUsed10 / 10U, 0, 100)));
  snprintf(buf, sizeof(buf), "PSI %u.%02u", currentState.memPsi100 / 100U, currentState.memPsi100 % 100U);
  drawStr(x, y + 36, buf);

  snprintf(buf, sizeof(buf), "U %u%%", currentState.memUsed10 / 10U);
  drawStr(statsX, y, buf);
  snprintf(buf, sizeof(buf), "C %s", cachedBuf);
  drawStr(statsX, y + 8, buf);
  snprintf(buf, sizeof(buf), "S %s", swapBuf);
  drawStr(statsX, y + 16, buf);
  snprintf(buf, sizeof(buf), "Z %s", zramBuf);
  drawStr(statsX, y + 24, buf);
  drawSparkline(statsX, y + 32, 28, 14, memHist, 100);
}

void drawSceneStoragePcie(int x, int y) {
  char buf[32];
  char readBuf[12];
  char writeBuf[12];
  char bwBuf[12];
  const int statsX = 78;
  const uint32_t totalBW = currentState.diskReadKBps + currentState.diskWriteKBps;

  drawLargeStr(x, y, rootTagLabel(currentState.rootTagCode), 2);
  drawHBarPct(x, y + 16, 50, 6, static_cast<uint8_t>(clampInt(currentState.diskUsed10 / 10U, 0, 100)));
  snprintf(buf, sizeof(buf), "U %u%%", currentState.diskUsed10 / 10U);
  drawStr(x, y + 24, buf);
  formatCompactKBps(currentState.diskReadKBps, readBuf, sizeof(readBuf));
  snprintf(buf, sizeof(buf), "R %s", readBuf);
  drawStr(x, y + 32, buf);
  formatCompactKBps(currentState.diskWriteKBps, writeBuf, sizeof(writeBuf));
  snprintf(buf, sizeof(buf), "W %s", writeBuf);
  drawStr(x, y + 40, buf);

  snprintf(buf, sizeof(buf), "IOW %u%%", currentState.iowait10 / 10U);
  drawStr(statsX, y, buf);
  if (currentState.nvmeTempC >= 0) {
    snprintf(buf, sizeof(buf), "NV %dC", currentState.nvmeTempC);
  } else {
    snprintf(buf, sizeof(buf), "NV --");
  }
  drawStr(statsX, y + 8, buf);
  drawStr(statsX, y + 16, rootTagLabel(currentState.rootTagCode));
  formatCompactKBps(totalBW, bwBuf, sizeof(bwBuf));
  snprintf(buf, sizeof(buf), "BW %s", bwBuf);
  drawStr(statsX, y + 24, buf);
  drawSparkline(statsX, y + 32, 28, 10, ioHist, 100);
}

void drawSceneNetworkLink(int x, int y) {
  char buf[32];
  char rxBuf[12];
  char txBuf[12];
  formatCompactKBps(currentState.netRxKBps, rxBuf, sizeof(rxBuf));
  formatCompactKBps(currentState.netTxKBps, txBuf, sizeof(txBuf));

  snprintf(buf, sizeof(buf), "E:%s", currentState.ethUp ? "UP" : "--");
  drawStr(x, y, buf);
  snprintf(buf, sizeof(buf), "W:%s", currentState.wlanUp ? "UP" : "--");
  drawStr(x, y + 8, buf);
  if (currentState.ethSpeedMbps >= 0 && currentState.gatewayLatency10 >= 0) {
    snprintf(buf, sizeof(buf), "GW %d.%d LK %d", currentState.gatewayLatency10 / 10, currentState.gatewayLatency10 % 10, currentState.ethSpeedMbps);
  } else if (currentState.ethSpeedMbps >= 0) {
    snprintf(buf, sizeof(buf), "GW -- LK %d", currentState.ethSpeedMbps);
  } else {
    snprintf(buf, sizeof(buf), "GW -- LK --");
  }
  drawStr(x, y + 16, buf);
  if (currentState.wifiRssiDbm > -999) {
    snprintf(buf, sizeof(buf), "RS %d RX %s", currentState.wifiRssiDbm, rxBuf);
  } else {
    snprintf(buf, sizeof(buf), "RS -- RX %s", rxBuf);
  }
  drawStr(x, y + 24, buf);
  snprintf(buf, sizeof(buf), "TX %s", txBuf);
  drawStr(x, y + 32, buf);
  drawSparkline(x, y + 40, 78, 12, netHist, 100);
}

void drawSceneSystemAlerts(int x, int y) {
  char buf[32];
  char alerts[32];
  char shortAlerts[7];
  const int statsX = 78;
  snprintf(buf, sizeof(buf), "UP %uh%02u", currentState.uptimeMinutes / 60U, currentState.uptimeMinutes % 60U);
  drawStr(x, y, buf);
  snprintf(buf, sizeof(buf), "P %u", currentState.procs);
  drawStr(x, y + 8, buf);
  drawStr(x, y + 16, "APP LAB");
  drawStr(x, y + 24, "UNO Q");
  snprintf(buf, sizeof(buf), "F %u", currentState.failedUnits);
  drawStr(x, y + 32, buf);

  buildAlertSummary(currentState.alertRecent, alerts, sizeof(alerts));
  snprintf(shortAlerts, sizeof(shortAlerts), "%.6s", alerts);
  snprintf(buf, sizeof(buf), "R:%s", shortAlerts);
  drawStr(statsX, y, buf);
  drawAlertPips(statsX, y + 10, currentState.alertCurrent, currentState.alertRecent);
  snprintf(buf, sizeof(buf), "NET %s", (currentState.ethUp || currentState.wlanUp) ? "UP" : "DN");
  drawStr(statsX, y + 24, buf);
  drawStr(statsX, y + 32, currentState.alertCurrent ? "ALERT" : "NOM");
}

void drawStatsScene(unsigned long nowMs, int scene) {
  const int x = 26;
  const int y = 18;
  switch (scene) {
    case 0: drawSceneThermalPower(nowMs, x, y); break;
    case 1: drawSceneCpuTopology(x, y); break;
    case 2: drawSceneMemoryPressure(x, y); break;
    case 3: drawSceneStoragePcie(x, y); break;
    case 4: drawSceneNetworkLink(x, y); break;
    case 5: drawSceneSystemAlerts(x, y); break;
  }
}

void dashTest(String o1, String o2) {
  Monitor.print("dashTest invoked! o1=");
  Monitor.print(o1);
  Monitor.print(" o2=");
  Monitor.println(o2);
}

void dashBegin(String uptimeMinutes_str, String procs_str, String tempC_str, String fanValue_str, String fanIsRPM_str) {
  uint32_t uptimeMinutes = uptimeMinutes_str.toInt();
  uint32_t procs = procs_str.toInt();
  int32_t tempC = tempC_str.toInt();
  int32_t fanValue = fanValue_str.toInt();
  uint32_t fanIsRPM = fanIsRPM_str.toInt();
  Monitor.println("dashBegin invoked!");
  pendingState = currentState;
  pendingState.uptimeMinutes = static_cast<uint16_t>(uptimeMinutes > 65535U ? 65535U : uptimeMinutes);
  pendingState.procs = static_cast<uint16_t>(procs > 65535U ? 65535U : procs);
  pendingState.tempC = tempC;
  pendingState.fanValue = fanValue;
  pendingState.fanIsRPM = fanIsRPM != 0U;
  pendingDirty = true;
}

void dashSystem(String armFreqMHz_str, String armMaxMHz_str, String totalCpu10_str, String iowait10_str, String load1x10_str) {
  uint32_t armFreqMHz = armFreqMHz_str.toInt();
  uint32_t armMaxMHz = armMaxMHz_str.toInt();
  uint32_t totalCpu10 = totalCpu10_str.toInt();
  uint32_t iowait10 = iowait10_str.toInt();
  uint32_t load1x10 = load1x10_str.toInt();
  pendingState.armFreqMHz = static_cast<int>(armFreqMHz);
  pendingState.armMaxMHz = static_cast<int>(armMaxMHz);
  pendingState.totalCpu10 = static_cast<uint16_t>(totalCpu10 > 1000U ? 1000U : totalCpu10);
  pendingState.iowait10 = static_cast<uint16_t>(iowait10 > 1000U ? 1000U : iowait10);
  pendingState.load1x10 = static_cast<uint16_t>(load1x10 > 999U ? 999U : load1x10);
}

void dashCores(String coreCount_str, String cpu0_str, String cpu1_str, String cpu2_str, String cpu3_str) {
  uint32_t coreCount = coreCount_str.toInt();
  uint32_t cpu0 = cpu0_str.toInt();
  uint32_t cpu1 = cpu1_str.toInt();
  uint32_t cpu2 = cpu2_str.toInt();
  uint32_t cpu3 = cpu3_str.toInt();
  pendingState.coreCount = static_cast<int>(coreCount > MAX_CORES ? MAX_CORES : coreCount);
  pendingState.perCore10[0] = static_cast<uint16_t>(cpu0 > 1000U ? 1000U : cpu0);
  pendingState.perCore10[1] = static_cast<uint16_t>(cpu1 > 1000U ? 1000U : cpu1);
  pendingState.perCore10[2] = static_cast<uint16_t>(cpu2 > 1000U ? 1000U : cpu2);
  pendingState.perCore10[3] = static_cast<uint16_t>(cpu3 > 1000U ? 1000U : cpu3);
}

void dashCoreFreqs(String freq0_str, String freq1_str, String freq2_str, String freq3_str) {
  uint32_t freq0 = freq0_str.toInt();
  uint32_t freq1 = freq1_str.toInt();
  uint32_t freq2 = freq2_str.toInt();
  uint32_t freq3 = freq3_str.toInt();
  pendingState.coreFreqMHz[0] = static_cast<uint16_t>(freq0 > 65535U ? 65535U : freq0);
  pendingState.coreFreqMHz[1] = static_cast<uint16_t>(freq1 > 65535U ? 65535U : freq1);
  pendingState.coreFreqMHz[2] = static_cast<uint16_t>(freq2 > 65535U ? 65535U : freq2);
  pendingState.coreFreqMHz[3] = static_cast<uint16_t>(freq3 > 65535U ? 65535U : freq3);
}

void dashMemory(String memUsed10_str, String memAvailMB_str, String memCachedMB_str, String swapUsedMB_str, String zramUsedMB_str, String memPsi100_str) {
  uint32_t memUsed10 = memUsed10_str.toInt();
  uint32_t memAvailMB = memAvailMB_str.toInt();
  uint32_t memCachedMB = memCachedMB_str.toInt();
  uint32_t swapUsedMB = swapUsedMB_str.toInt();
  uint32_t zramUsedMB = zramUsedMB_str.toInt();
  uint32_t memPsi100 = memPsi100_str.toInt();
  pendingState.memUsed10 = static_cast<uint16_t>(memUsed10 > 1000U ? 1000U : memUsed10);
  pendingState.memAvailMB = static_cast<uint16_t>(memAvailMB > 65535U ? 65535U : memAvailMB);
  pendingState.memCachedMB = static_cast<uint16_t>(memCachedMB > 65535U ? 65535U : memCachedMB);
  pendingState.swapUsedMB = static_cast<uint16_t>(swapUsedMB > 65535U ? 65535U : swapUsedMB);
  pendingState.zramUsedMB = static_cast<uint16_t>(zramUsedMB > 65535U ? 65535U : zramUsedMB);
  pendingState.memPsi100 = static_cast<uint16_t>(memPsi100 > 9999U ? 9999U : memPsi100);
}

void dashStorage(String rootTagCode_str, String diskUsed10_str, String diskReadKBps_str, String diskWriteKBps_str, String nvmeTempC_str, String throttledMask_str, String failedUnits_str) {
  uint32_t rootTagCode = rootTagCode_str.toInt();
  uint32_t diskUsed10 = diskUsed10_str.toInt();
  uint32_t diskReadKBps = diskReadKBps_str.toInt();
  uint32_t diskWriteKBps = diskWriteKBps_str.toInt();
  int32_t nvmeTempC = nvmeTempC_str.toInt();
  uint32_t throttledMask = throttledMask_str.toInt();
  uint32_t failedUnits = failedUnits_str.toInt();
  pendingState.rootTagCode = static_cast<uint8_t>(rootTagCode > 3U ? 0U : rootTagCode);
  pendingState.diskUsed10 = static_cast<uint16_t>(diskUsed10 > 1000U ? 1000U : diskUsed10);
  pendingState.diskReadKBps = diskReadKBps;
  pendingState.diskWriteKBps = diskWriteKBps;
  pendingState.nvmeTempC = nvmeTempC;
  pendingState.throttledMask = static_cast<uint16_t>(throttledMask > 65535U ? 65535U : throttledMask);
  pendingState.failedUnits = static_cast<uint16_t>(failedUnits > 65535U ? 65535U : failedUnits);
}

void dashNetwork(String ethUp_str, String wlanUp_str, String ethSpeedMbps_str, String wifiRssiDbm_str, String netRxKBps_str, String netTxKBps_str, String gatewayLatency10_str) {
  uint32_t ethUp = ethUp_str.toInt();
  uint32_t wlanUp = wlanUp_str.toInt();
  int32_t ethSpeedMbps = ethSpeedMbps_str.toInt();
  int32_t wifiRssiDbm = wifiRssiDbm_str.toInt();
  uint32_t netRxKBps = netRxKBps_str.toInt();
  uint32_t netTxKBps = netTxKBps_str.toInt();
  int32_t gatewayLatency10 = gatewayLatency10_str.toInt();
  pendingState.ethUp = ethUp != 0U;
  pendingState.wlanUp = wlanUp != 0U;
  pendingState.ethSpeedMbps = ethSpeedMbps;
  pendingState.wifiRssiDbm = wifiRssiDbm;
  pendingState.netRxKBps = netRxKBps;
  pendingState.netTxKBps = netTxKBps;
  pendingState.gatewayLatency10 = gatewayLatency10;
}

void dashCommit() {
  Monitor.println("dashCommit invoked");
  commitPendingState();
}

int dashPing() {
  return 1;
}

void pullSnapshot(unsigned long nowMs) {
  static unsigned long lastSyncMs = 0;
  static bool firstSync = true;
  if (!firstSync && (nowMs - lastSyncMs) < SYNC_INTERVAL_MS) {
    return;
  }
  firstSync = false;
  lastSyncMs = nowMs;

  int32_t syncOk = 0;
  const bool callOk = Bridge.call("dash_sync").result(syncOk);
  if (!callOk || syncOk == 0) {
    Monitor.println("dashboard pull failed");
  }
}
}  // namespace

// Initialize the board, LCD, and Router Bridge communication
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
  Bridge.provide("dash_test", dashTest);
  Bridge.provide("dash_begin", dashBegin);
  Bridge.provide("dash_system", dashSystem);
  Bridge.provide("dash_cores", dashCores);
  Bridge.provide("dash_core_freqs", dashCoreFreqs);
  Bridge.provide("dash_memory", dashMemory);
  Bridge.provide("dash_storage", dashStorage);
  Bridge.provide("dash_network", dashNetwork);
  Bridge.provide("dash_commit", dashCommit);
  Bridge.provide("dash_ping", dashPing);

  randomSeed(micros());
  Monitor.println("dashboard bridge ready");
}

// Main Dashboard loop: Ping the Python host for new state, render LCARS scenes
void loop() {
  static unsigned long lastFrameMillis = 0;
  const unsigned long nowMs = millis();

  pullSnapshot(nowMs);

  if (nowMs - lastFrameMillis < FRAME_INTERVAL_MS) {
    delay(1);
    return;
  }
  lastFrameMillis = nowMs;

  memset(frameBuffer, 0, sizeof(frameBuffer));
  if (!haveState) {
    drawBootScreen();
    flushFrame();
    return;
  }

  const int scene = static_cast<int>((nowMs / (SCENE_SECONDS * 1000UL)) % SCENE_COUNT);
  const char *titles[SCENE_COUNT] = {
      "THERM/PWR", "CPU TOPO", "MEM/PRESS",
      "PCIe/STOR", "NET/LINK", "SYS/ALERT",
  };

  drawLcarsFrame(nowMs, scene, titles[scene]);
  drawStatsScene(nowMs, scene);
  flushFrame();
}
