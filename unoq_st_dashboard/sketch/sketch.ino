#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>
#include <string.h>

namespace {
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint8_t WIDTH = 128;
constexpr uint8_t HEIGHT = 64;
constexpr uint8_t PAGES = HEIGHT / 8;
constexpr size_t FRAME_BYTES = WIDTH * PAGES;
constexpr size_t I2C_CHUNK = 31;
constexpr unsigned long SERIAL_BAUD = 460800;
constexpr unsigned long FRAME_READ_GAP_TIMEOUT_MS = 180;
constexpr char PROTOCOL_MAGIC[] = {'O', 'L', 'E', 'D'};

uint8_t frameBuffer[FRAME_BYTES] = {0};
uint32_t displayedFrames = 0;

void sendSerialLine(const char *message) {
  Serial1.println(message);
}

void sendSerialValue(const char *prefix, uint32_t value) {
  Serial1.print(prefix);
  Serial1.println(value);
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

size_t readExact(uint8_t *buffer, size_t count) {
  size_t total = 0;
  unsigned long deadline = millis() + FRAME_READ_GAP_TIMEOUT_MS;

  while (total < count) {
    const int available = Serial1.available();
    if (available > 0) {
      int remaining = available;
      while (remaining-- > 0 && total < count) {
        const int incoming = Serial1.read();
        if (incoming < 0) {
          break;
        }
        buffer[total++] = static_cast<uint8_t>(incoming);
      }
      deadline = millis() + FRAME_READ_GAP_TIMEOUT_MS;
      continue;
    }

    if (static_cast<long>(millis() - deadline) >= 0) {
      break;
    }
    delay(1);
  }

  return total;
}

bool waitForMagicAndCommand(char &commandOut) {
  static size_t matched = 0;
  static bool waitingForCommand = false;

  if (waitingForCommand) {
    if (Serial1.available() == 0) {
      return false;
    }
    commandOut = static_cast<char>(Serial1.read());
    waitingForCommand = false;
    return true;
  }

  while (Serial1.available() > 0) {
    const int incoming = Serial1.read();
    if (incoming < 0) {
      break;
    }
    if (incoming == PROTOCOL_MAGIC[matched]) {
      ++matched;
      if (matched == sizeof(PROTOCOL_MAGIC)) {
        matched = 0;
        if (Serial1.available() == 0) {
          waitingForCommand = true;
          return false;
        }
        commandOut = static_cast<char>(Serial1.read());
        return true;
      }
    } else {
      matched = (incoming == PROTOCOL_MAGIC[0]) ? 1 : 0;
    }
  }
  return false;
}

void handleFrame() {
  const size_t got = readExact(frameBuffer, sizeof(frameBuffer));
  if (got != sizeof(frameBuffer)) {
    Serial1.print("ERR SHORT ");
    Serial1.println(static_cast<unsigned long>(got));
    return;
  }
  if (!flushFrame()) {
    sendSerialLine("ERR DISPLAY");
    return;
  }
  ++displayedFrames;
  sendSerialValue("ACK ", displayedFrames);
}

void handleCommand(char command) {
  switch (command) {
    case 'Q':
      sendSerialLine("READY UNOQ_OLED_SINK");
      break;
    case 'F':
      handleFrame();
      break;
    case 'E':
      sendSerialValue("DONE ", displayedFrames);
      break;
    default:
      sendSerialLine("ERR CMD");
      break;
  }
}
}  // namespace

void setup() {
  Serial1.begin(SERIAL_BAUD);
  Serial1.setTimeout(1500);

  Wire.begin();
  Wire.setClock(400000);

  delay(50);
  if (!initDisplay()) {
    sendSerialLine("ERR INIT");
    return;
  }
  memset(frameBuffer, 0, sizeof(frameBuffer));
  clearDisplay();
  flushFrame();
  sendSerialLine("BOOT UNOQ_OLED_SINK");
}

void loop() {
  char command = 0;
  if (!waitForMagicAndCommand(command)) {
    delay(1);
    return;
  }
  handleCommand(command);
}
