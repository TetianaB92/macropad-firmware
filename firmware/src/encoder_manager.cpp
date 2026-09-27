#include "encoder_manager.h"
#include "board_config.h"
#include <Arduino.h>
#if !ENC_I2C_DISABLED
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include <driver/i2c.h>
#else
#include <Wire.h>
#endif
#endif
#include <string.h>

EncoderManager::Binding EncoderManager::bindings_[5];
bool EncoderManager::chassisEnabled_ = false;

/** MCP23017: GPA0..4 = enc A, GPA5..7+GPB0..1 = enc B, GPB2..6 = buttons */
static constexpr uint8_t MCP_IODIRA = 0x00;
static constexpr uint8_t MCP_IODIRB = 0x01;
static constexpr uint8_t MCP_GPPUA = 0x0C;
static constexpr uint8_t MCP_GPPUB = 0x0D;
static constexpr uint8_t MCP_GPIOA = 0x12;
static constexpr uint8_t MCP_GPIOB = 0x13;

static bool mcpOk = false;
static bool mcpAnnounced = false;
static uint8_t prevA[5] = {0};
static uint8_t prevBtn[5] = {1, 1, 1, 1, 1};
static uint32_t lastProbeMs = 0;

#if ENC_I2C_DISABLED
static bool mcpWriteReg(uint8_t, uint8_t) { return false; }
static uint8_t mcpReadReg(uint8_t) { return 0xFF; }

static void mcpInit() {
  if (!mcpAnnounced) {
    Serial.println(
        "EncoderManager: MCP23017 I2C disabled on P4 (legacy touch I2C vs "
        "driver_ng abort). UART enc+/enc-/encp still work. Planned pins: SDA=47 SCL=48.");
    mcpAnnounced = true;
  }
  mcpOk = false;
}
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
static bool busReady = false;
static bool mcpWriteReg(uint8_t reg, uint8_t val) {
  const uint8_t bytes[] = {reg, val};
  return i2c_master_write_to_device(I2C_NUM_1, ENC_I2C_ADDR, bytes, 2,
                                  pdMS_TO_TICKS(10)) == ESP_OK;
}
static bool mcpReadPair(uint8_t* bytes) {
  const uint8_t reg = MCP_GPIOA;
  return i2c_master_write_read_device(I2C_NUM_1, ENC_I2C_ADDR, &reg, 1,
                                    bytes, 2, pdMS_TO_TICKS(10)) == ESP_OK;
}
static void mcpInit() {
  if (!busReady) {
    i2c_config_t cfg{};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = ENC_I2C_SDA; cfg.scl_io_num = ENC_I2C_SCL;
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE; cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = 100000;
    busReady = i2c_param_config(I2C_NUM_1, &cfg) == ESP_OK &&
               i2c_driver_install(I2C_NUM_1, cfg.mode, 0, 0, 0) == ESP_OK;
    if (!busReady) { Serial.println("Encoder I2C1 init failed"); return; }
    pinMode(ENC_THIRD_B_PIN, INPUT_PULLUP);
  }
  mcpOk = mcpWriteReg(MCP_IODIRA, 0xFF) && mcpWriteReg(MCP_IODIRB, 0xFF) &&
          mcpWriteReg(MCP_GPPUA, 0x7F) && mcpWriteReg(MCP_GPPUB, 0x7F);
  Serial.printf("EncoderManager: MCP23017 %s (SDA=47 SCL=48 addr=0x20)\n",
                mcpOk ? "online" : "not found");
}
#else
static bool mcpWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ENC_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t mcpReadReg(uint8_t reg) {
  Wire.beginTransmission(ENC_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)ENC_I2C_ADDR, 1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

static void mcpInit() {
  Wire.begin(ENC_I2C_SDA, ENC_I2C_SCL);
  Wire.beginTransmission(ENC_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    mcpOk = false;
    Serial.println("EncoderManager: MCP23017 not found (OK until PCB)");
    return;
  }
  const bool found = mcpWriteReg(MCP_IODIRA, 0xFF) && mcpWriteReg(MCP_IODIRB, 0xFF) &&
                     mcpWriteReg(MCP_GPPUA, 0xFF) && mcpWriteReg(MCP_GPPUB, 0xFF);
  if (found != mcpOk || !mcpAnnounced) {
    Serial.printf("EncoderManager: MCP23017 %s (SDA=%d SCL=%d addr=0x%02X)\n",
                  found ? "online" : "not found", ENC_I2C_SDA, ENC_I2C_SCL, ENC_I2C_ADDR);
    mcpAnnounced = true;
  }
  mcpOk = found;
}
#endif

/** Quadrature decode: returns -1,0,+1 */
static int quadDelta(uint8_t prev, uint8_t now) {
  static const int8_t table[16] = {
      0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  return table[(prev << 2) | now];
}

void EncoderManager::init() {
  for (int i = 0; i < 5; i++) {
    bindings_[i].enabled = false;
    strcpy(bindings_[i].rotate, "none");
    strcpy(bindings_[i].press, "none");
  }
  chassisEnabled_ = true;
  bindings_[0].enabled = bindings_[1].enabled = true;
  strcpy(bindings_[0].rotate, "system_volume");
  strcpy(bindings_[0].press, "system_mute");
  strcpy(bindings_[1].rotate, "monitor_brightness");
  mcpInit();
  Serial.println("EncoderManager: init");
}

void EncoderManager::applyFromLayout(JsonDocument& layout) {
  chassisEnabled_ = layout["encodersEnabled"] | false;
  JsonArray arr = layout["encoders"].as<JsonArray>();
  for (int i = 0; i < 5; i++) {
    bindings_[i].enabled = false;
    strcpy(bindings_[i].rotate, "none");
    strcpy(bindings_[i].press, "none");
  }
  if (!chassisEnabled_ || arr.isNull() || arr.size() == 0) {
    // Bring-up defaults requested for testing before configuring the web layout.
    chassisEnabled_ = true;
    bindings_[0].enabled = bindings_[1].enabled = true;
    strcpy(bindings_[0].rotate, "system_volume");
    strcpy(bindings_[0].press, "system_mute");
    strcpy(bindings_[1].rotate, "monitor_brightness");
    return;
  }

  for (JsonObject enc : arr) {
    int idx = enc["index"] | -1;
    if (idx < 0 || idx >= 5) continue;
    bindings_[idx].enabled = true;
    strncpy(bindings_[idx].rotate, enc["rotate"] | "none", 31);
    strncpy(bindings_[idx].press, enc["press"] | "none", 31);
    bindings_[idx].rotate[31] = 0;
    bindings_[idx].press[31] = 0;
  }
  Serial.printf("Encoders enabled=%d\n", (int)chassisEnabled_);
}

void EncoderManager::emitRotate(QueueHandle_t q, int idx, int dir) {
  if (idx < 0 || idx >= 5) return;
  auto& b = bindings_[idx];
  if (!b.enabled || strcmp(b.rotate, "none") == 0) return;
  char action[64];
  snprintf(action, sizeof(action), "ENC_ROTATE:%d:%s:%+d", idx, b.rotate,
           dir >= 0 ? 1 : -1);
  xQueueSend(q, &action, 0);
}

void EncoderManager::emitPress(QueueHandle_t q, int idx) {
  if (idx < 0 || idx >= 5) return;
  auto& b = bindings_[idx];
  if (!b.enabled || strcmp(b.press, "none") == 0) return;
  char action[64];
  snprintf(action, sizeof(action), "ENC_PRESS:%d:%s", idx, b.press);
  xQueueSend(q, &action, 0);
}

void EncoderManager::handleSimLine(QueueHandle_t q, const char* text) {
  String line(text);
  line.trim();
  if (line.startsWith("enc+") || line.startsWith("enc-")) {
    int dir = line.startsWith("enc+") ? 1 : -1;
    emitRotate(q, line.substring(4).toInt(), dir);
  } else if (line.startsWith("encp")) {
    emitPress(q, line.substring(4).toInt());
  }
}

static void pollMcp(QueueHandle_t q) {
  if (!mcpOk) {
    if (millis() - lastProbeMs > 5000) {
      lastProbeMs = millis();
      mcpInit();
    }
    return;
  }
  static bool seeded = false;
#if defined(CONFIG_IDF_TARGET_ESP32P4) && !ENC_I2C_DISABLED
  uint8_t bytes[2];
  if (!mcpReadPair(bytes)) { mcpOk = false; seeded = false; lastProbeMs = millis(); return; }
  uint8_t ga = bytes[0], gb = bytes[1];
#else
  uint8_t ga = mcpReadReg(MCP_GPIOA);
  uint8_t gb = mcpReadReg(MCP_GPIOB);
#endif
  static int8_t movement[5] = {};
  static uint32_t buttonAt[5] = {};

  // Enc i A = GPA bit i (0..4)
  // Enc i B = for i<3: GPA bit(5+i); for i>=3: GPB bit(i-3)
  // Enc i BTN = GPB bit(2+i)
  for (int i = 0; i < 5; i++) {
    uint8_t a = (ga >> i) & 1;
    uint8_t b =
        (i < 3) ? ((ga >> (5 + i)) & 1) : ((gb >> (i - 3)) & 1);
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (i == 2) b = digitalRead(ENC_THIRD_B_PIN) ? 1 : 0;
#endif
    uint8_t now = (a) | (b << 1);
    const uint8_t btn = (gb >> (2 + i)) & 1;
    if (!seeded) { prevA[i] = now; prevBtn[i] = btn; movement[i] = 0; continue; }
    int d = quadDelta(prevA[i] & 0x3, now);
    prevA[i] = now;
    movement[i] += d;
    if (movement[i] >= 4 || movement[i] <= -4) {
      EncoderManager::emitRotate(q, i, movement[i] > 0 ? 1 : -1);
      movement[i] = 0;
    }
    if (prevBtn[i] == 1 && btn == 0 && millis() - buttonAt[i] >= 40) {
      buttonAt[i] = millis(); EncoderManager::emitPress(q, i);
    }
    prevBtn[i] = btn;
  }
  seeded = true;
}

void EncoderManager::loop(QueueHandle_t actionQueue) {
  if (!chassisEnabled_) return;
#if ENC_I2C_DISABLED
  (void)actionQueue;
#else
  pollMcp(actionQueue);
#endif
}
