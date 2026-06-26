#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <driver/twai.h>

#if __has_include("config.h")
#include "config.h"
#elif __has_include("config.h.example")
#include "config.h.example"
#else
#error "Missing config.h or config.h.example"
#endif

constexpr uint8_t BMS_REG_COUNT = 0x24; // 0x0000..0x0023
constexpr size_t BMS_RAW_REGS = BMS_REG_COUNT;
constexpr size_t JSON_DOC_CAPACITY = 12288;

struct BmsSnapshot
{
  bool online = false;
  int errorCount = 0;
  unsigned long lastUpdateMs = 0;
  uint8_t activeSlaveId = 0;
  uint8_t lastTriedSlaveId = 0;

  float packV = 0.0f;
  float current = 0.0f;
  int soc = 0;
  int soh = 0;
  int minCellTemp = 0;
  int maxCellTemp = 0;
  int tempPCB = 0;
  uint16_t remainingAh = 0;
  uint16_t maxChargeCurrentLimit = 0;
  uint16_t status = 0;
  uint16_t warning = 0;
  uint16_t protect = 0;
  uint16_t cycles = 0;
  uint16_t reservedStatus = 0;
  uint16_t cellCount = 0;
  float cellVolts[16] = {0.0f};
  float minCellV = 0.0f;
  float maxCellV = 0.0f;
  float avgCellV = 0.0f;
  float cellDeltaV = 0.0f;

  uint16_t rawRegs[BMS_RAW_REGS] = {0};
  uint8_t rawCount = 0;
  uint8_t rawSlaveId = 0;
  uint8_t rawFunction = 0;
  uint8_t rawByteCount = 0;
  uint16_t crcReceived = 0;
  uint16_t crcCalculated = 0;
};

BmsSnapshot bms;
WebServer server(80);
extern const char UI_THEME_BASE_VARS[] PROGMEM;
extern const char UI_THEME_CUSTOM_VARS[] PROGMEM;
extern const char UI_SHARED_CSS[] PROGMEM;
extern const char UI_SHARED_JS[] PROGMEM;
extern const char INDEX_HTML[] PROGMEM;
extern const char DIAG_HTML[] PROGMEM;

bool wifiConnected()
{
  return WiFi.status() == WL_CONNECTED;
}

uint16_t getModbusCRC(const uint8_t *buf, size_t len)
{
  uint16_t crc = 0xFFFF;
  for (size_t pos = 0; pos < len; pos++)
  {
    crc ^= (uint16_t)buf[pos];
    for (int i = 8; i != 0; i--)
    {
      if ((crc & 0x0001) != 0)
      {
        crc >>= 1;
        crc ^= 0xA001;
      }
      else
      {
        crc >>= 1;
      }
    }
  }
  return crc;
}

static inline uint16_t readReg16(const uint8_t *frame, uint8_t regIndex)
{
  size_t bytePos = 3 + (size_t)regIndex * 2;
  return ((uint16_t)frame[bytePos] << 8) | frame[bytePos + 1];
}

static inline float roundTo2(float value)
{
  return roundf(value * 100.0f) / 100.0f;
}

static inline float roundTo3(float value)
{
  return roundf(value * 1000.0f) / 1000.0f;
}

String formatJsonFloat(float value, uint8_t decimals = 3)
{
  char buf[20];
  snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  return String(buf);
}

String formatHex16(uint16_t value)
{
  char buf[8];
  snprintf(buf, sizeof(buf), "0x%04X", value);
  return String(buf);
}

const char *rawRegisterName(uint8_t index)
{
  switch (index)
  {
  case 0x00:
    return "pack voltage";
  case 0x01:
    return "current";
  case 0x02:
    return "cell 1";
  case 0x03:
    return "cell 2";
  case 0x04:
    return "cell 3";
  case 0x05:
    return "cell 4";
  case 0x06:
    return "cell 5";
  case 0x07:
    return "cell 6";
  case 0x08:
    return "cell 7";
  case 0x09:
    return "cell 8";
  case 0x0A:
    return "cell 9";
  case 0x0B:
    return "cell 10";
  case 0x0C:
    return "cell 11";
  case 0x0D:
    return "cell 12";
  case 0x0E:
    return "cell 13";
  case 0x0F:
    return "cell 14";
  case 0x10:
    return "cell 15";
  case 0x11:
    return "cell 16";
  case 0x12:
    return "temp pcb";
  case 0x13:
    return "temp min";
  case 0x14:
    return "temp max";
  case 0x15:
    return "remaining Ah";
  case 0x16:
    return "max charge current";
  case 0x17:
    return "SOH";
  case 0x18:
    return "SOC";
  case 0x1A:
    return "cycles";
  case 0x1B:
    return "status";
  case 0x1C:
    return "warning";
  case 0x1D:
    return "protect";
  case 0x1E:
    return "cell count";
  default:
    return "unknown";
  }
}

struct FlagName
{
  uint16_t bit;
  const char *name;
};

const FlagName STATUS_FLAGS[] = {
    {0x0001, "Discharge"},
    {0x0002, "Charge"},
    {0x1000, "Standby"},
};

const FlagName WARNING_FLAGS[] = {
    {0x0001, "Cell overvoltage"},
    {0x0002, "Cell undervoltage"},
    {0x0004, "Pack overvoltage"},
    {0x0008, "Pack undervoltage"},
    {0x0010, "Charge overtemperature"},
    {0x0020, "Charge undertemperature"},
    {0x0040, "Discharge overtemperature"},
    {0x0080, "Discharge undertemperature"},
    {0x0100, "Charge overcurrent"},
    {0x0200, "Discharge overcurrent"},
    {0x0400, "Short circuit"},
    {0x0800, "IC front-end error"},
    {0x1000, "Mosfet software lock"},
};

const FlagName PROTECT_FLAGS[] = {
    {0x0001, "Single overvoltage"},
    {0x0002, "Single undervoltage"},
    {0x0004, "Whole group overvoltage"},
    {0x0008, "Whole group undervoltage"},
    {0x0010, "Charge overtemperature"},
    {0x0020, "Charge undertemperature"},
    {0x0040, "Discharge overtemperature"},
    {0x0080, "Discharge undertemperature"},
    {0x0100, "Charge overcurrent"},
    {0x0200, "Discharge overcurrent"},
    {0x0400, "Short circuit"},
    {0x0800, "IC front-end error"},
    {0x1000, "Software lock MOS"},
};

String decodeBitmask(uint16_t mask, const FlagName *flags, size_t flagCount)
{
  String out;
  for (size_t i = 0; i < flagCount; i++)
  {
    if ((mask & flags[i].bit) == 0)
      continue;
    if (!out.isEmpty())
      out += ", ";
    out += flags[i].name;
  }
  if (out.isEmpty())
    out = "none";
  return out;
}

String formatUptime()
{
  unsigned long sec = millis() / 1000UL;
  unsigned int days = sec / 86400UL;
  sec %= 86400UL;
  unsigned int hours = sec / 3600UL;
  sec %= 3600UL;
  unsigned int minutes = sec / 60UL;
  unsigned int seconds = sec % 60UL;

  String out;
  if (days > 0)
  {
    out += String(days) + "d ";
  }
  out += String(hours) + "h ";
  out += String(minutes) + "m ";
  out += String(seconds) + "s";
  return out;
}

String batteryStateText()
{
  return bms.online ? "ONLINE" : "OFFLINE";
}

String healthText()
{
  if (bms.warning != 0)
  {
    return "Warnings: " + decodeBitmask(bms.warning, WARNING_FLAGS, sizeof(WARNING_FLAGS) / sizeof(WARNING_FLAGS[0]));
  }
  if (bms.protect != 0)
  {
    return "Protected: " + decodeBitmask(bms.protect, PROTECT_FLAGS, sizeof(PROTECT_FLAGS) / sizeof(PROTECT_FLAGS[0]));
  }
  return "Normal";
}

String operatingStateText()
{
  if (!bms.online)
  {
    return "Offline";
  }
  const bool dischargeEnabled = (bms.status & 0x0001) != 0;
  const bool chargeEnabled = (bms.status & 0x0002) != 0;
  if (dischargeEnabled && chargeEnabled)
  {
    return "Charging + Discharging";
  }
  if (chargeEnabled)
  {
    return "Charging";
  }
  if (dischargeEnabled)
  {
    return "Discharging";
  }
  if (bms.protect != 0)
  {
    return "Protected";
  }
  if (bms.warning != 0)
  {
    return "Warning";
  }
  if (bms.status == 0 || (bms.status & 0x1000) != 0 || (bms.warning == 2 && bms.protect == 4096 && fabs(bms.current) < 0.5f && bms.soc >= 95))
  {
    return "Standby";
  }
  if (bms.status != 0)
  {
    return "Active";
  }
  return "Normal";
}

String healthClass()
{
  if (bms.warning != 0)
  {
    return "warn";
  }
  if (bms.protect != 0)
  {
    return "warn";
  }
  return "soft";
}

String statusFlagsText()
{
  if (bms.status == 0)
  {
    return "Standby";
  }
  return decodeBitmask(bms.status, STATUS_FLAGS, sizeof(STATUS_FLAGS) / sizeof(STATUS_FLAGS[0]));
}

String warningFlagsText()
{
  return decodeBitmask(bms.warning, WARNING_FLAGS, sizeof(WARNING_FLAGS) / sizeof(WARNING_FLAGS[0]));
}

String protectFlagsText()
{
  return decodeBitmask(bms.protect, PROTECT_FLAGS, sizeof(PROTECT_FLAGS) / sizeof(PROTECT_FLAGS[0]));
}

String haStatusCategory()
{
  if (bms.protect != 0)
    return "protected";
  if (bms.warning != 0)
    return "warning";
  return "status";
}

String statusDetailText()
{
  if (bms.protect != 0)
    return "Protected (" + protectFlagsText() + ")";
  if (bms.warning != 0)
    return "Warning (" + warningFlagsText() + ")";
  return statusFlagsText();
}

String buildDiagRawRegistersHtml()
{
  String html;
  html.reserve(96 + bms.rawCount * 160);
  html += "<div class='card'><h2 style='margin:0'>Raw registers</h2><div class='reg-grid'>";
  for (uint8_t i = 0; i < bms.rawCount; i++)
  {
    html += "<div class='reg-card'>";
    html += "<div class='reg-line reg-addr'>";
    html += formatHex16(i);
    html += "</div>";
    html += "<div class='reg-line reg-hex'>";
    html += formatHex16(bms.rawRegs[i]);
    html += "</div>";
    html += "<div class='reg-line reg-name'>";
    html += rawRegisterName(i);
    html += "</div>";
    html += "<div class='reg-line reg-dec'>";
    html += String(bms.rawRegs[i]);
    html += "</div>";
    html += "</div>";
  }
  html += "</div></div>";
  return html;
}

void clearSnapshotData()
{
  bms.online = false;
  bms.rawCount = 0;
  bms.rawSlaveId = 0;
  bms.rawFunction = 0;
  bms.rawByteCount = 0;
  bms.crcReceived = 0;
  bms.crcCalculated = 0;
}

void decodeSnapshot()
{
  bms.packV = roundTo2(bms.rawRegs[0] / 100.0f);
  bms.current = roundTo2((int16_t)bms.rawRegs[1] / 100.0f);

  float minCell = 1000.0f;
  float maxCell = 0.0f;
  float sumCell = 0.0f;
  uint16_t validCells = 0;
  for (int i = 0; i < 16; i++)
  {
    bms.cellVolts[i] = roundTo3(bms.rawRegs[2 + i] / 1000.0f);
    if (bms.cellVolts[i] > 0.0f)
    {
      validCells++;
      if (bms.cellVolts[i] < minCell)
        minCell = bms.cellVolts[i];
      if (bms.cellVolts[i] > maxCell)
        maxCell = bms.cellVolts[i];
      sumCell += bms.cellVolts[i];
    }
  }
  if (minCell == 1000.0f)
    minCell = 0.0f;
  bms.minCellV = roundTo3(minCell);
  bms.maxCellV = roundTo3(maxCell);
  bms.avgCellV = roundTo3(validCells > 0 ? (sumCell / validCells) : 0.0f);
  bms.cellDeltaV = roundTo3(maxCell - minCell);

  bms.tempPCB = (int16_t)bms.rawRegs[0x12];
  bms.minCellTemp = (int16_t)bms.rawRegs[0x13];
  bms.maxCellTemp = (int16_t)bms.rawRegs[0x14];
  bms.remainingAh = bms.rawRegs[0x15];
  bms.maxChargeCurrentLimit = bms.rawRegs[0x16];
  bms.soh = bms.rawRegs[0x17];
  bms.soc = bms.rawRegs[0x18];
  // Tentative mapping based on observed values: 0x1B=status, 0x1C=warning, 0x1D=protect.
  bms.reservedStatus = bms.rawRegs[0x19];
  bms.cycles = bms.rawRegs[0x1A];
  bms.status = bms.rawRegs[0x1B];
  bms.warning = bms.rawRegs[0x1C];
  bms.protect = bms.rawRegs[0x1D];
  bms.cellCount = bms.rawRegs[0x1E];
  // 0x1F..0x23 do not map cleanly to direct voltages; keep them as auxiliary/config fields.
}

bool requestBmsData(uint8_t slaveId)
{
  bms.lastTriedSlaveId = slaveId;

  while (Serial2.available())
  {
    Serial2.read();
  }

  uint8_t req[8] = {slaveId, 0x03, 0x00, 0x00, 0x00, BMS_REG_COUNT, 0x00, 0x00};
  uint16_t crc = getModbusCRC(req, 6);
  req[6] = crc & 0xFF;
  req[7] = (crc >> 8) & 0xFF;

  digitalWrite(BMS_DE_PIN, HIGH);
  delayMicroseconds(50);
  Serial2.write(req, 8);
  Serial2.flush();
  delayMicroseconds(50);
  digitalWrite(BMS_DE_PIN, LOW);

  uint8_t res[80];
  int idx = 0;
  unsigned long start = millis();
  while (millis() - start < 250 && idx < (int)sizeof(res))
  {
    if (Serial2.available())
    {
      res[idx++] = Serial2.read();
    }
    delay(1);
  }

  if (idx < 5)
  {
    bms.errorCount++;
    clearSnapshotData();
    return false;
  }

  bms.rawSlaveId = res[0];
  bms.rawFunction = res[1];
  bms.rawByteCount = res[2];
  bms.crcReceived = ((uint16_t)res[idx - 1] << 8) | res[idx - 2];
  bms.crcCalculated = getModbusCRC(res, idx - 2);

  if (bms.rawSlaveId != slaveId || bms.rawFunction != 0x03)
  {
    bms.errorCount++;
    clearSnapshotData();
    return false;
  }

  if (bms.crcReceived != bms.crcCalculated)
  {
    bms.errorCount++;
    clearSnapshotData();
    return false;
  }

  const int expectedBytes = 3 + (BMS_RAW_REGS * 2) + 2;
  if (idx < expectedBytes)
  {
    bms.errorCount++;
    clearSnapshotData();
    return false;
  }

  bms.rawCount = BMS_RAW_REGS;
  for (uint8_t i = 0; i < BMS_RAW_REGS; i++)
  {
    bms.rawRegs[i] = readReg16(res, i);
  }

  decodeSnapshot();
  bms.online = true;
  bms.errorCount = 0;
  bms.lastUpdateMs = millis();
  bms.activeSlaveId = slaveId;
  return true;
}

void sendCanFrame(uint32_t id, uint8_t len, uint8_t *data)
{
  if (!ENABLE_DEYE_CAN)
  {
    return;
  }
  twai_message_t message = {};
  message.identifier = id;
  message.extd = 0;
  message.data_length_code = len;
  for (int i = 0; i < len; i++)
  {
    message.data[i] = data[i];
  }
  twai_transmit(&message, pdMS_TO_TICKS(10));
}

void sendDeyeCanTelemetry()
{
  if (!ENABLE_DEYE_CAN || !bms.online)
  {
    return;
  }

  uint16_t avgSoc = (uint16_t)bms.soc;
  uint16_t outV = (uint16_t)(bms.packV * 100.0f);
  int16_t outI = (int16_t)(bms.current * 10.0f);
  uint16_t totalCap = 100;

  uint8_t d351[8] = {0x1C, 0x02, 0xF4, 0x01, 0xF4, 0x01, 0xD6, 0x01};
  sendCanFrame(0x351, 8, d351);

  uint8_t d355[8] = {(uint8_t)(avgSoc & 0xFF), (uint8_t)(avgSoc >> 8), 0x64, 0x00, 0x00, 0x00, 0x00, 0x00};
  sendCanFrame(0x355, 8, d355);

  uint8_t d356[8] = {(uint8_t)(outV & 0xFF), (uint8_t)(outV >> 8), (uint8_t)(outI & 0xFF), (uint8_t)(outI >> 8), 0x00, 0x00, 0x00, 0x00};
  sendCanFrame(0x356, 8, d356);

  uint8_t d359[8] = {0x00, 0x00, 0x00, 0x01, (uint8_t)('0' + 1), 0x00, 0x00, 0x00};
  sendCanFrame(0x359, 8, d359);

  uint8_t d35E[8] = {'P', 'Y', 'L', 'O', 'N', ' ', '0', '1'};
  sendCanFrame(0x35E, 8, d35E);

  uint8_t d35F[8] = {(uint8_t)(totalCap & 0xFF), (uint8_t)(totalCap >> 8), 0x00, 0x00, (uint8_t)(totalCap & 0xFF), (uint8_t)(totalCap >> 8), 0x01, 0x00};
  sendCanFrame(0x35F, 8, d35F);
}

void handleRoot()
{
  String html = FPSTR(INDEX_HTML);
  html.replace("__UI_THEME_BASE_VARS__", String(FPSTR(UI_THEME_BASE_VARS)));
  html.replace("__UI_THEME_CUSTOM_VARS__", String(FPSTR(UI_THEME_CUSTOM_VARS)));
  html.replace("__UI_SHARED_CSS__", String(FPSTR(UI_SHARED_CSS)));
  html.replace("__UI_SHARED_JS__", String(FPSTR(UI_SHARED_JS)));
  server.send(200, "text/html", html);
}

String buildDiagHtml(uint16_t refreshSeconds)
{
  String html = FPSTR(DIAG_HTML);
  html.replace("__UI_THEME_BASE_VARS__", String(FPSTR(UI_THEME_BASE_VARS)));
  html.replace("__UI_THEME_CUSTOM_VARS__", String(FPSTR(UI_THEME_CUSTOM_VARS)));
  html.replace("__UI_SHARED_CSS__", String(FPSTR(UI_SHARED_CSS)));
  html.replace("__REFRESH_META__", refreshSeconds > 0 ? String("<meta http-equiv='refresh' content='") + String(refreshSeconds) + "'>" : "");
  html.replace("__REFRESH_SECONDS__", String(refreshSeconds));
  html.replace("__REFRESH_STATE__", refreshSeconds > 0 ? String("on (") + String(refreshSeconds) + " s)" : "off");
  html.replace("__REFRESH_ACTION__", refreshSeconds > 0 ? "disable" : "enable");
  html.replace("__ACTIVE_ID__", String(bms.activeSlaveId));
  html.replace("__LAST_TRIED__", String(bms.lastTriedSlaveId));
  html.replace("__ONLINE__", String(bms.online ? "ONLINE" : "OFFLINE"));
  html.replace("__ERRORS__", String(bms.errorCount));
  html.replace("__OPERATING__", operatingStateText());
  html.replace("__HEALTH_CLASS__", healthClass());
  html.replace("__HEALTH_TEXT__", healthText());
  html.replace("__STATUS_HEX__", formatHex16(bms.status));
  html.replace("__STATUS_TEXT__", statusFlagsText());
  html.replace("__WARNING_HEX__", formatHex16(bms.warning));
  html.replace("__WARNING_TEXT__", warningFlagsText());
  html.replace("__PROTECT_HEX__", formatHex16(bms.protect));
  html.replace("__PROTECT_TEXT__", protectFlagsText());
  html.replace("__CYCLES__", String(bms.cycles));
  html.replace("__CELL_COUNT__", String(bms.cellCount ? bms.cellCount : 15));
  html.replace("__CRC_RX__", formatHex16(bms.crcReceived));
  html.replace("__CRC_CALC__", formatHex16(bms.crcCalculated));
  html.replace("__RAW_REGS__", buildDiagRawRegistersHtml());
  return html;
}

void handleDiag()
{
  uint16_t refreshSeconds = 0;
  if (server.hasArg("upd"))
  {
    int requested = server.arg("upd").toInt();
    if (requested > 0)
      refreshSeconds = (uint16_t)requested;
  }
  server.send(200, "text/html", buildDiagHtml(refreshSeconds));
}

void fillTelemetryJson(JsonObject root)
{
  JsonObject system = root.createNestedObject("system");
  JsonObject wifi = system.createNestedObject("wifi");
  wifi["connected"] = wifiConnected();
  wifi["ip"] = wifiConnected() ? WiFi.localIP().toString() : "n/a";
  system["uptime_ms"] = millis();
  system["errors"] = bms.errorCount;
  system["active_slave_id"] = bms.activeSlaveId;
  system["last_tried_slave_id"] = bms.lastTriedSlaveId;
  system["last_update_ms"] = bms.lastUpdateMs;

  JsonObject status = root.createNestedObject("status");
  status["online"] = bms.online;
  status["health_text"] = healthText();
  status["operating_text"] = operatingStateText();
  status["category"] = haStatusCategory();
  status["text"] = statusDetailText();
  JsonObject statusRaw = status.createNestedObject("raw");
  statusRaw["status"] = bms.status;
  statusRaw["warning"] = bms.warning;
  statusRaw["protect"] = bms.protect;
  statusRaw["cycles"] = bms.cycles;
  statusRaw["reserved_status"] = bms.reservedStatus;
  JsonObject statusHex = status.createNestedObject("hex");
  statusHex["status"] = formatHex16(bms.status);
  statusHex["warning"] = formatHex16(bms.warning);
  statusHex["protect"] = formatHex16(bms.protect);
  JsonObject diagnostics = root.createNestedObject("diagnostics");
  JsonObject crc = diagnostics.createNestedObject("crc");
  crc["received"] = bms.crcReceived;
  crc["calculated"] = bms.crcCalculated;

  JsonObject pack = root.createNestedObject("pack");
  pack["voltage_v"] = serialized(formatJsonFloat(bms.packV));
  pack["current_a"] = serialized(formatJsonFloat(bms.current));
  pack["soc_pct"] = bms.soc;
  pack["soh_pct"] = bms.soh;

  JsonObject temperatures = root.createNestedObject("temperatures");
  temperatures["pcb_c"] = bms.tempPCB;
  temperatures["cell_min_c"] = bms.minCellTemp;
  temperatures["cell_max_c"] = bms.maxCellTemp;

  JsonObject limits = root.createNestedObject("limits");
  limits["remaining_ah"] = bms.remainingAh;
  limits["max_charge_current_limit"] = bms.maxChargeCurrentLimit;

  JsonObject cells = root.createNestedObject("cells");
  cells["count"] = bms.cellCount ? bms.cellCount : 15;
  cells["min_v"] = serialized(formatJsonFloat(bms.minCellV));
  cells["avg_v"] = serialized(formatJsonFloat(bms.avgCellV));
  cells["max_v"] = serialized(formatJsonFloat(bms.maxCellV));
  cells["delta_v"] = serialized(formatJsonFloat(bms.cellDeltaV));
  JsonArray cellVoltages = cells.createNestedArray("voltages_v");
  for (int i = 0; i < 16; i++)
  {
    cellVoltages.add(serialized(formatJsonFloat(bms.cellVolts[i])));
  }

  JsonArray rawRegs = diagnostics.createNestedArray("raw_regs");
  for (uint8_t i = 0; i < bms.rawCount; i++)
  {
    rawRegs.add(bms.rawRegs[i]);
  }
}

void handleJson()
{
  DynamicJsonDocument doc(JSON_DOC_CAPACITY);
  JsonObject root = doc.to<JsonObject>();
  fillTelemetryJson(root);

  String json;
  json.reserve(measureJson(doc) + 1);
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void setup()
{
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, BMS_RX_PIN, BMS_TX_PIN);
  pinMode(BMS_DE_PIN, OUTPUT);
  digitalWrite(BMS_DE_PIN, LOW);

  if (ENABLE_DEYE_CAN)
  {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK)
    {
      twai_start();
    }
  }

  WiFi.mode(WIFI_STA);
  if (WIFI_SSID_HIDDEN)
  {
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  }
  else
  {
    WiFi.setScanMethod(WIFI_FAST_SCAN);
  }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/", handleRoot);
  server.on("/diag", handleDiag);
  server.on("/json", handleJson);
  server.begin();
}

void loop()
{
  static unsigned long lastPoll = 0;
  static unsigned long lastLog = 0;
  static uint8_t scanId = PREFERRED_BMS_ID;

  if (millis() - lastPoll >= 3000)
  {
    bool ok = requestBmsData(bms.online ? bms.activeSlaveId : scanId);
    if (!ok && !bms.online)
    {
      scanId++;
      if (scanId > BMS_ID_MAX)
      {
        scanId = BMS_ID_MIN;
      }
    }
    lastPoll = millis();
  }

  if (millis() - lastLog >= 3000)
  {
    lastLog = millis();
    Serial.print("BMS ");
    Serial.print(bms.online ? "ONLINE" : "OFFLINE");
    Serial.print(" active_id=");
    Serial.print(bms.activeSlaveId);
    Serial.print(" tried_id=");
    Serial.print(bms.lastTriedSlaveId);
    Serial.print(" err=");
    Serial.print(bms.errorCount);
    Serial.print(" crc=");
    Serial.print(formatHex16(bms.crcReceived));
    Serial.print("/");
    Serial.println(formatHex16(bms.crcCalculated));
  }

  if (ENABLE_DEYE_CAN && bms.online)
    sendDeyeCanTelemetry();

  server.handleClient();
  delay(1);
}
