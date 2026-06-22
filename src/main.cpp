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
  int envTemp = 0;
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
  uint16_t cellsV[15] = {0};
  float cellVolts[15] = {0.0f};
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
  case 0x13:
    return "env temp";
  case 0x14:
    return "max cell temp";
  case 0x15:
    return "remaining Ah";
  case 0x16:
    return "max charge current";
  case 0x17:
    return "SOH";
  case 0x18:
    return "SOC";
  case 0x19:
    return "status";
  case 0x1A:
    return "warning";
  case 0x1B:
    return "protect";
  case 0x1C:
    return "cycles";
  case 0x1D:
    return "reserved status";
  case 0x1E:
    return "cell count";
  case 0x1F:
    return "aux cfg 1";
  case 0x20:
    return "aux cfg 2";
  case 0x21:
    return "aux cfg 3";
  case 0x22:
    return "aux cfg 4";
  case 0x23:
    return "aux cfg 5";
  default:
    return "reserved";
  }
}

struct FlagName
{
  uint16_t bit;
  const char *name;
};

const FlagName STATUS_FLAGS[] = {
    {0x0001, "Discharge FET"},
    {0x0002, "Charge FET"},
    {0x1000, "Standby / extra bit"},
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
  for (size_t i = 0; i < flagCount; i++) {
    if ((mask & flags[i].bit) == 0) continue;
    if (!out.isEmpty()) out += ", ";
    out += flags[i].name;
  }
  if (out.isEmpty()) out = "none";
  return out;
}

String activeFlagChips(uint16_t mask, const FlagName *flags, size_t flagCount, const char *chipClass)
{
  String out;
  bool hasAny = false;
  for (size_t i = 0; i < flagCount; i++) {
    if ((mask & flags[i].bit) == 0) continue;
    if (!hasAny) {
      out += "<div class='flag-group'><div class='flag-title'>Active bits</div>";
      hasAny = true;
    }
    out += "<span class='tag ";
    out += chipClass;
    out += "'>";
    out += flags[i].name;
    out += "</span>";
  }
  if (!hasAny) {
    out = "<div class='flag-group'><div class='flag-title'>Active bits</div><span class='tag tag-muted'>none</span></div>";
  }
  else {
    out += "</div>";
  }
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
  if (days > 0) {
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
  if ((bms.status & 0x1000) != 0 || (bms.warning == 2 && bms.protect == 4096 && fabs(bms.current) < 0.5f && bms.soc >= 95))
  {
    return "Standby";
  }
  if (bms.protect != 0)
  {
    return "Protected";
  }
  if (bms.warning != 0)
  {
    return "Warning";
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
  if (bms.protect != 0) return "protected";
  if (bms.warning != 0) return "warning";
  return "status";
}

String haSubstatusText()
{
  if (bms.protect != 0) return protectFlagsText();
  if (bms.warning != 0) return warningFlagsText();
  return statusFlagsText();
}

String buildDiagRawRegistersHtml()
{
  String html;
  html.reserve(64 + bms.rawCount * 140);
  html += "<div class='card'><h2>Raw registers</h2><div class='grid'>";
  html += "<div class='muted' style='margin-bottom:10px'>0x1F..0x23 look like auxiliary/config fields on this pack, not temperatures.</div>";
  for (uint8_t i = 0; i < bms.rawCount; i++)
  {
    html += "<div class='cell'><div class='muted'>Field</div><div><strong>";
    html += rawRegisterName(i);
    html += "</strong></div><div class='muted'>Reg 0x";
    char idxBuf[8];
    snprintf(idxBuf, sizeof(idxBuf), "%04X", i);
    html += idxBuf;
    html += "</div><div><strong>";
    html += formatHex16(bms.rawRegs[i]);
    html += "</strong></div><div class='muted'>";
    html += String(bms.rawRegs[i]);
    html += "</div></div>";
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
  bms.packV = bms.rawRegs[0] / 100.0f;
  bms.current = (int16_t)bms.rawRegs[1] / 100.0f;

  float minCell = 1000.0f;
  float maxCell = 0.0f;
  float sumCell = 0.0f;
  for (int i = 0; i < 15; i++)
  {
    bms.cellsV[i] = bms.rawRegs[2 + i];
    bms.cellVolts[i] = bms.cellsV[i] / 1000.0f;
    if (bms.cellVolts[i] > 0.0f)
    {
      if (bms.cellVolts[i] < minCell)
        minCell = bms.cellVolts[i];
      if (bms.cellVolts[i] > maxCell)
        maxCell = bms.cellVolts[i];
      sumCell += bms.cellVolts[i];
    }
  }
  if (minCell == 1000.0f)
    minCell = 0.0f;
  bms.minCellV = minCell;
  bms.maxCellV = maxCell;
  bms.avgCellV = sumCell / 15.0f;
  bms.cellDeltaV = maxCell - minCell;

  bms.envTemp = (int16_t)bms.rawRegs[0x13];
  bms.maxCellTemp = (int16_t)bms.rawRegs[0x14];
  bms.remainingAh = bms.rawRegs[0x15];
  bms.maxChargeCurrentLimit = bms.rawRegs[0x16];
  bms.soh = bms.rawRegs[0x17];
  bms.soc = bms.rawRegs[0x18];
  // Raw samples match the original layout: 0x19=status, 0x1A=warning, 0x1B=protect.
  bms.status = bms.rawRegs[0x19];
  bms.warning = bms.rawRegs[0x1A];
  bms.protect = bms.rawRegs[0x1B];
  bms.cycles = bms.rawRegs[0x1C];
  bms.reservedStatus = bms.rawRegs[0x1D];
  bms.cellCount = bms.rawRegs[0x1E];
  // 0x1F..0x23 do not map cleanly to direct voltages; keep them as auxiliary/config fields.
  bms.tempPCB = bms.envTemp;
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
  server.send_P(200, "text/html", INDEX_HTML);
}

String buildDiagHtml()
{
  String html = FPSTR(DIAG_HTML);
  html.replace("__ACTIVE_ID__", String(bms.activeSlaveId));
  html.replace("__LAST_TRIED__", String(bms.lastTriedSlaveId));
  html.replace("__ONLINE__", String(bms.online ? "ONLINE" : "OFFLINE"));
  html.replace("__ERRORS__", String(bms.errorCount));
  html.replace("__OPERATING__", operatingStateText());
  html.replace("__HEALTH_CLASS__", healthClass());
  html.replace("__HEALTH_TEXT__", healthText());
  html.replace("__STATUS_HEX__", formatHex16(bms.status));
  html.replace("__STATUS_TEXT__", statusFlagsText());
  html.replace("__STATUS_CHIPS__", activeFlagChips(bms.status, STATUS_FLAGS, sizeof(STATUS_FLAGS) / sizeof(STATUS_FLAGS[0]), "tag-status"));
  html.replace("__WARNING_HEX__", formatHex16(bms.warning));
  html.replace("__WARNING_TEXT__", warningFlagsText());
  html.replace("__WARNING_CHIPS__", activeFlagChips(bms.warning, WARNING_FLAGS, sizeof(WARNING_FLAGS) / sizeof(WARNING_FLAGS[0]), "tag-warn"));
  html.replace("__PROTECT_HEX__", formatHex16(bms.protect));
  html.replace("__PROTECT_TEXT__", protectFlagsText());
  html.replace("__PROTECT_CHIPS__", activeFlagChips(bms.protect, PROTECT_FLAGS, sizeof(PROTECT_FLAGS) / sizeof(PROTECT_FLAGS[0]), "tag-protect"));
  html.replace("__CYCLES__", String(bms.cycles));
  html.replace("__CELL_COUNT__", String(bms.cellCount ? bms.cellCount : 15));
  html.replace("__CRC_RX__", formatHex16(bms.crcReceived));
  html.replace("__CRC_CALC__", formatHex16(bms.crcCalculated));
  html.replace("__RAW_REGS__", buildDiagRawRegistersHtml());
  return html;
}

void handleDiag()
{
  server.send(200, "text/html", buildDiagHtml());
}

void fillTelemetryJson(JsonObject root, bool haFormat)
{
  if (!haFormat)
  {
    JsonObject wifi = root.createNestedObject("wifi");
    wifi["connected"] = wifiConnected();
    wifi["ip"] = wifiConnected() ? WiFi.localIP().toString() : "n/a";
    root["uptime_ms"] = millis();
    root["online"] = bms.online;
    root["errors"] = bms.errorCount;
    root["active_slave_id"] = bms.activeSlaveId;
    root["last_tried_slave_id"] = bms.lastTriedSlaveId;
    root["can_enabled"] = ENABLE_DEYE_CAN;
    root["mode"] = ENABLE_DEYE_CAN ? "bms_can" : "bms_read";
    root["mode_label"] = ENABLE_DEYE_CAN ? "BMS + Deye CAN" : "BMS read only";
    root["health_text"] = healthText();
    root["operating_text"] = operatingStateText();
    root["last_update_ms"] = bms.lastUpdateMs;
    root["crc_received"] = bms.crcReceived;
    root["crc_calculated"] = bms.crcCalculated;
    root["pack_v"] = bms.packV;
    root["current_a"] = bms.current;
    root["soc"] = bms.soc;
    root["soh"] = bms.soh;
    root["env_temp"] = bms.envTemp;
    root["max_cell_temp"] = bms.maxCellTemp;
    root["remaining_ah"] = bms.remainingAh;
    root["max_charge_current_limit"] = bms.maxChargeCurrentLimit;
    root["status_category"] = haStatusCategory();
    root["substatus"] = haSubstatusText();
    root["status_raw"] = bms.status;
    root["warning_raw"] = bms.warning;
    root["protect_raw"] = bms.protect;
    root["cycles"] = bms.cycles;
    root["reserved_status"] = bms.reservedStatus;
    root["cell_count"] = bms.cellCount ? bms.cellCount : 15;
    root["cell_min_v"] = bms.minCellV;
    root["cell_avg_v"] = bms.avgCellV;
    root["cell_max_v"] = bms.maxCellV;
    root["cell_delta_v"] = bms.cellDeltaV;

    JsonArray cellVoltages = root.createNestedArray("cell_voltages_mv");
    JsonArray cells = root.createNestedArray("cells");
    for (int i = 0; i < 15; i++)
    {
      root[String("cell_") + String(i + 1) + "_mv"] = bms.cellsV[i];
      cellVoltages.add(bms.cellsV[i]);

      JsonObject cell = cells.createNestedObject();
      cell["index"] = i + 1;
      cell["mv"] = bms.cellsV[i];
      cell["v"] = bms.cellVolts[i];
    }

    JsonArray rawRegs = root.createNestedArray("raw_regs");
    for (uint8_t i = 0; i < bms.rawCount; i++)
    {
      rawRegs.add(bms.rawRegs[i]);
    }
    return;
  }

  root["online"] = bms.online;
  root["active_slave_id"] = bms.activeSlaveId;
  root["health_text"] = healthText();
  root["operating_text"] = operatingStateText();
  root["status"] = haStatusCategory();
  root["substatus"] = haSubstatusText();
  root["status_raw"] = bms.status;
  root["warning_raw"] = bms.warning;
  root["protect_raw"] = bms.protect;
  root["cell_count"] = bms.cellCount ? bms.cellCount : 15;
  root["pack_voltage_v"] = bms.packV;
  root["current_a"] = bms.current;
  root["soc_pct"] = bms.soc;
  root["soh_pct"] = bms.soh;
  root["env_temp_c"] = bms.envTemp;
  root["max_cell_temp_c"] = bms.maxCellTemp;
  root["remaining_ah"] = bms.remainingAh;
  root["max_charge_current_limit"] = bms.maxChargeCurrentLimit;
  root["cell_min_v"] = bms.minCellV;
  root["cell_avg_v"] = bms.avgCellV;
  root["cell_max_v"] = bms.maxCellV;
  root["cell_delta_v"] = bms.cellDeltaV;
  root["warning_hex"] = formatHex16(bms.warning);
  root["protect_hex"] = formatHex16(bms.protect);
  root["status_hex"] = formatHex16(bms.status);
  root["cycles"] = bms.cycles;
  root["reserved_status"] = bms.reservedStatus;

  for (int i = 0; i < 15; i++)
  {
    root[String("cell_") + String(i + 1) + "_mv"] = bms.cellsV[i];
  }

  JsonArray cellVoltages = root.createNestedArray("cell_voltages_mv");
  for (int i = 0; i < 15; i++)
  {
    cellVoltages.add(bms.cellsV[i]);
  }
}

void handleJson()
{
  DynamicJsonDocument doc(JSON_DOC_CAPACITY);
  JsonObject root = doc.to<JsonObject>();
  fillTelemetryJson(root, false);

  String json;
  json.reserve(measureJson(doc) + 1);
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleHaJson()
{
  DynamicJsonDocument doc(JSON_DOC_CAPACITY);
  JsonObject root = doc.to<JsonObject>();
  fillTelemetryJson(root, true);

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
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/", handleRoot);
  server.on("/diag", handleDiag);
  server.on("/json", handleJson);
  server.on("/ha", handleHaJson);
  server.begin();
}

void loop()
{
  static unsigned long lastPoll = 0;
  static unsigned long lastLog = 0;
  static uint8_t scanId = PREFERRED_BMS_ID;

  if (millis() - lastPoll >= 3000) {
    bool ok = requestBmsData(bms.online ? bms.activeSlaveId : scanId);
    if (!ok && !bms.online) {
      scanId++;
      if (scanId > BMS_ID_MAX) {
        scanId = BMS_ID_MIN;
      }
    }
    lastPoll = millis();
  }

  if (millis() - lastLog >= 3000) {
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

  if (ENABLE_DEYE_CAN && bms.online) sendDeyeCanTelemetry();

  server.handleClient();
  delay(1);
}
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="uk">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BMS Dashboard</title>
  <style>
    *{box-sizing:border-box}
    body{--bg:#111;--panel:#1a1a1a;--panel2:#202020;--text:#ececec;--muted:#a8a8a8;--line:#333;--accent:#d6d6d6;--ok:#dcdcdc;--warn:#bdbdbd;--bad:#8d8d8d;--cell-low-bg:#181818;--cell-high-bg:#242424;--fill-low:#6e6e6e;--fill-mid:#9c9c9c;--fill-high:#d0d0d0;margin:0;font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:var(--text)}
    .wrap{max-width:1280px;margin:0 auto;padding:20px}
    .hero{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;margin-bottom:18px}
    h1{margin:0;font-size:28px}
    .sub{color:var(--muted);margin-top:6px}
    .pill{display:inline-block;padding:4px 10px;border-radius:999px;background:var(--panel2);border:1px solid var(--line);color:var(--muted);font-size:12px}
    .pill.live{color:var(--ok)}
    .pill.offline{color:var(--bad)}
    .grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}
    .card{grid-column:span 12;background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:16px}
    .half{grid-column:span 6}
    .stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}
    .stat{padding:14px;border-radius:14px;background:var(--panel2);border:1px solid var(--line)}
    #crc{font-size:.7em}
    .label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}
    .value{font-size:22px;font-weight:700;margin-top:6px}
    .ok{color:var(--ok)} .warn{color:var(--warn)} .bad{color:var(--bad)}
    .soft{color:var(--muted)}
    .row{display:flex;gap:12px;flex-wrap:wrap;align-items:center}
    .muted{color:var(--muted)}
    .grid-cells{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:10px;margin-top:12px}
    .cell{padding:10px;border-radius:12px;background:var(--panel2);border:1px solid var(--line)}
    .cell.low{background:var(--cell-low-bg);border-color:var(--bad)}
    .cell.high{background:var(--cell-high-bg);border-color:var(--ok)}
    .cell .a{color:var(--muted);font-size:12px}
    .cell .v{font-weight:700;margin-top:6px}
    .cell .r{color:var(--muted);font-size:12px;margin-top:4px}
    .bar{height:10px;background:var(--panel);border:1px solid var(--line);border-radius:999px;overflow:hidden}
    .fill{height:100%;background:var(--fill-mid)}
    .fill.low{background:var(--fill-low)}
    .fill.mid{background:var(--fill-mid)}
    .fill.high{background:var(--fill-high)}
    a{color:var(--accent);text-decoration:none}
    @media (max-width:900px){.half{grid-column:span 12}.stats{grid-template-columns:repeat(2,minmax(0,1fr))}}
    @media (max-width:560px){.hero{flex-direction:column;align-items:flex-start}.stats{grid-template-columns:1fr}}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <div>
        <h1>BMS Dashboard</h1>
        <div class="sub">Vision V-LFP48100</div>
      </div>
      <div class="pill" id="pollState">loading</div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="stats">
          <div class="stat"><div class="label">Wi-Fi</div><div class="value" id="wifiState">-</div><div class="muted" id="wifiIp">-</div></div>
          <div class="stat"><div class="label">Uptime</div><div class="value" id="uptime">-</div><div class="muted" id="lastUpdate">-</div></div>
          <div class="stat"><div class="label">Errors</div><div class="value" id="errors">-</div><div class="muted" id="crc">-</div></div>
        </div>
      </div>

      <div class="card half">
        <div class="row">
          <div>
            <div class="label">Status</div>
            <div class="value" id="statusText">-</div>
            <div class="muted" style="margin-top:4px">Substatus: <strong id="substatusText">-</strong></div>
          </div>
          <div style="min-width:220px;flex:1">
            <div class="label">SOC</div>
            <div class="bar"><div class="fill" id="socBar" style="width:0%"></div></div>
            <div class="muted" id="socText" style="margin-top:6px">-</div>
          </div>
        </div>
        <div class="row" style="margin-top:12px">
          <div><span class="muted">Voltage:</span> <strong id="packV">-</strong></div>
          <div><span class="muted">Current:</span> <strong id="currentA">-</strong></div>
          <div><span class="muted">SOH:</span> <strong id="soh">-</strong></div>
        </div>
        <div class="row" style="margin-top:12px">
          <div><span class="muted">Env temp:</span> <strong id="envTemp">-</strong></div>
          <div><span class="muted">Max cell temp:</span> <strong id="maxCellTemp">-</strong></div>
          <div><span class="muted">Remaining Ah:</span> <strong id="remainingAh">-</strong></div>
        </div>
        <div class="row">
          <div><span class="muted">BMS ID:</span> <strong id="slaveId">-</strong></div>
          <div><span class="muted">last tried:</span> <strong id="scanId">-</strong></div>
        </div>
        <div style="margin-top:12px">
          <div class="label">Cell summary</div>
          <div class="row" style="margin-top:10px">
            <div><span class="muted">Cell count:</span> <strong id="cellCount">-</strong></div>
            <div><span class="muted">State:</span> <strong id="healthText">-</strong></div>
          </div>
          <div class="row" style="margin-top:8px">
            <div><span class="muted">Min:</span> <strong id="cellMin">-</strong></div>
            <div><span class="muted">Avg:</span> <strong id="cellAvg">-</strong></div>
            <div><span class="muted">Max:</span> <strong id="cellMax">-</strong></div>
            <div><span class="muted">Delta:</span> <strong id="cellDelta">-</strong></div>
          </div>
        </div>
      </div>

      <div class="card half">
        <div class="label">Cells</div>
        <div class="grid-cells" id="cellGrid"></div>
      </div>

      <div class="card">
        <div class="row" style="justify-content:space-between">
          <div class="label">Links</div>
          <div class="row">
            <a href="/ha">HA</a>
            <a href="/json">JSON</a>
            <a href="/diag">Diagnostic</a>
          </div>
        </div>
      </div>
    </div>
  </div>
  <script>
    const byId = (id) => document.getElementById(id);
    const hex16 = (n) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(4, '0');
    const fmt = (v, d = 0) => Number(v).toFixed(d);
    const clamp = (v, min, max) => Math.max(min, Math.min(max, v));
    const setText = (id, val) => {
      const el = byId(id);
      if (el) el.textContent = val;
    };

    function badgeForOnline(online) {
      return online ? { text: 'ONLINE', cls: 'ok' } : { text: 'OFFLINE', cls: 'bad' };
    }

    function deriveOperatingText(data) {
      if (!data.online) return 'Offline';
      const currentA = Number(data.current_a || 0);
      const soc = Number(data.soc || 0);
      const warning = Number(data.warning_raw || 0);
      const protect = Number(data.protect_raw || 0);
      const status = Number(data.status_raw || 0);

      if (warning === 2 && protect === 4096 && status === 0 && Math.abs(currentA) < 0.5 && soc >= 95) {
        return 'Standby';
      }
      if (currentA > 0.05) return 'Charging';
      if (currentA < -0.05) return 'Discharging';
      return data.operating_text || data.health_text || '-';
    }

    function extractCellMvList(data) {
      const cells = [];
      if (Array.isArray(data.cells) && data.cells.length) {
        data.cells.forEach((cell) => {
          if (typeof cell === 'number') {
            cells.push(Number(cell || 0));
            return;
          }
          if (cell && typeof cell === 'object') {
            if (cell.mv != null) {
              cells.push(Number(cell.mv || 0));
              return;
            }
            if (cell.v != null) {
              cells.push(Number(cell.v || 0) * 1000);
              return;
            }
          }
          cells.push(0);
        });
      } else if (Array.isArray(data.cell_voltages_mv) && data.cell_voltages_mv.length) {
        data.cell_voltages_mv.forEach((mv) => cells.push(Number(mv || 0)));
      } else if (Array.isArray(data.raw_regs) && data.raw_regs.length >= 17) {
        data.raw_regs.slice(2, 17).forEach((reg) => cells.push(Number(reg || 0)));
      } else {
        for (let i = 1; i <= 15; i++) {
          const key = 'cell_' + i + '_mv';
          if (data[key] != null)
            cells.push(Number(data[key] || 0));
        }
      }
      return cells;
    }

    function renderCells(data, minMv, maxMv) {
      const grid = byId('cellGrid');
      grid.innerHTML = '';
      const mvList = extractCellMvList(data);
      mvList.forEach((mv, i) => {
        const v = (mv / 1000).toFixed(3);
        const el = document.createElement('div');
        el.className = 'cell';
        if (mv === minMv) el.classList.add('low');
        if (mv === maxMv) el.classList.add('high');
        el.innerHTML = '<div class="a">Cell ' + (i + 1) + '</div>' +
                       '<div class="v">' + v + ' V</div>';
        grid.appendChild(el);
      });
    }

    async function refresh() {
      try {
        const res = await fetch('/json', { cache: 'no-store' });
        const data = await res.json();
        setText('pollState', 'live');
        byId('pollState').className = 'pill live';
        setText('wifiState', data.wifi.connected ? 'connected' : 'disconnected');
        byId('wifiState').className = 'value ' + (data.wifi.connected ? 'ok' : 'bad');
        setText('wifiIp', 'IP: ' + data.wifi.ip);
        setText('uptime', Math.floor(data.uptime_ms / 1000) + ' s');
        setText('lastUpdate', data.last_update_ms ? ('last update: ' + Math.floor((data.uptime_ms - data.last_update_ms) / 1000) + ' s ago') : 'no data yet');
        setText('slaveId', data.active_slave_id || '-');
        setText('scanId', 'last tried: ' + (data.last_tried_slave_id || '-'));
        setText('errors', data.errors);
        setText('crc', 'CRC rx ' + hex16(data.crc_received || 0) + ' / calc ' + hex16(data.crc_calculated || 0));
        setText('mode', data.mode_label || data.mode || '-');
        setText('canEnabled', 'CAN: ' + (data.can_enabled ? 'enabled' : 'disabled'));

        const s = badgeForOnline(data.online);
        setText('statusText', s.text);
        byId('statusText').className = 'value ' + s.cls;
        setText('substatusText', data.substatus || '-');
        setText('packV', fmt(data.pack_v || 0, 2) + ' V');
        setText('currentA', fmt(data.current_a || 0, 2) + ' A');
        setText('soh', (data.soh ?? '-') + '%');
        setText('envTemp', (data.env_temp ?? '-') + ' C');
        setText('maxCellTemp', (data.max_cell_temp ?? '-') + ' C');
        setText('remainingAh', data.remaining_ah ?? '-');
        setText('cellMin', fmt(data.cell_min_v || 0, 3) + ' V');
        setText('cellAvg', fmt(data.cell_avg_v || 0, 3) + ' V');
        setText('cellMax', fmt(data.cell_max_v || 0, 3) + ' V');
        setText('cellDelta', fmt(data.cell_delta_v || 0, 3) + ' V');
        setText('healthText', deriveOperatingText(data));
        const mvList = extractCellMvList(data);
        const cellCount = Number(data.cell_count || mvList.length || 0);
        setText('cellCount', cellCount > 0 ? String(cellCount) : '-');

        const soc = clamp(Number(data.soc || 0), 0, 100);
        byId('socBar').style.width = soc + '%';
        byId('socBar').className = 'fill ' + (soc < 20 ? 'low' : soc < 80 ? 'mid' : 'high');
        setText('socText', soc + '%');

        renderCells(data, Math.round((data.cell_min_v || 0) * 1000), Math.round((data.cell_max_v || 0) * 1000));
      } catch (e) {
        setText('pollState', 'offline');
        byId('pollState').className = 'pill offline';
      }
    }
    refresh();
    setInterval(refresh, 1000);
  </script>
</body>
</html>
)rawliteral";

const char DIAG_HTML[] PROGMEM = R"rawliteral(
<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='1'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
body{--bg:#111;--panel:#1a1a1a;--panel2:#202020;--text:#ececec;--muted:#a8a8a8;--line:#333;--accent:#d6d6d6;--ok:#dcdcdc;--warn:#bdbdbd;--bad:#8d8d8d;margin:0;font-family:Arial,sans-serif;background:var(--bg);color:var(--text);padding:18px;}
.card{max-width:1200px;margin:0 auto 16px auto;padding:16px;border:1px solid var(--line);border-radius:12px;background:var(--panel);}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px;}
.cell{padding:10px;border-radius:12px;background:var(--panel2);border:1px solid var(--line);}
.flag-group{margin-top:8px;padding:10px;border:1px solid var(--line);border-radius:10px;background:#151515;}
.flag-title{color:var(--muted);font-size:11px;letter-spacing:.05em;text-transform:uppercase;margin-bottom:6px;}
.tag{display:inline-block;margin:0 6px 6px 0;padding:4px 8px;border-radius:999px;font-size:12px;border:1px solid transparent;}
.tag-muted{background:#202020;color:var(--muted);border-color:var(--line);}
.tag-status{background:#1f2b3f;color:#dbe8ff;border-color:#35537a;}
.tag-warn{background:#3d3415;color:#fff2ba;border-color:#7f6a20;}
.tag-protect{background:#3c1818;color:#ffd0d0;border-color:#7f2e2e;}
.muted{color:var(--muted);font-size:12px;}
.soft{color:var(--muted);}
.ok{color:var(--ok);}.warn{color:var(--warn);}.bad{color:var(--bad);}
a{color:var(--accent);text-decoration:none;}
</style></head><body>
<div class='card'><h1>Diagnostics</h1>
<div>Active ID: __ACTIVE_ID__ | Last tried: __LAST_TRIED__</div>
<div>Status: __ONLINE__ | Errors: __ERRORS__</div>
<div>Operating state: __OPERATING__</div>
<div>Health: <span class='__HEALTH_CLASS__'>__HEALTH_TEXT__</span></div>
<div>Status: __STATUS_HEX__ <span class='muted'>(__STATUS_TEXT__)</span>__STATUS_CHIPS__</div>
<div>Warning: __WARNING_HEX__ <span class='muted'>(__WARNING_TEXT__)</span>__WARNING_CHIPS__</div>
<div>Protect: __PROTECT_HEX__ <span class='muted'>(__PROTECT_TEXT__)</span>__PROTECT_CHIPS__</div>
<div>Cycles: __CYCLES__</div>
<div>Cell count: __CELL_COUNT__</div>
<div>CRC rx: __CRC_RX__ | CRC calc: __CRC_CALC__</div>
<div><a href='/'>Back to dashboard</a></div>
</div>
__RAW_REGS__
</body></html>
)rawliteral";
