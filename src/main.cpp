#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <driver/twai.h>
#include <stdarg.h>
#include <cstring>

constexpr uint8_t BMS_REG_COUNT = 0x24; // 0x0000..0x0023
constexpr size_t BMS_RAW_REGS = BMS_REG_COUNT;
constexpr size_t JSON_DOC_CAPACITY = 12288;
constexpr uint32_t CONFIG_MAGIC = 0x424D5301UL;
constexpr uint16_t CONFIG_VERSION = 1;
constexpr char CONFIG_NAMESPACE[] = "bmscfg";
constexpr char AP_SSID[] = "BMS";
const IPAddress AP_IP(192, 168, 0, 1);
const IPAddress AP_MASK(255, 255, 255, 0);
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;
constexpr size_t CUSTOM_CSS_MAX_LEN = 1536;

#ifndef ENABLE_DEBUG_SERIAL
constexpr bool ENABLE_DEBUG_SERIAL = true;
#endif

#ifndef ENABLE_DEBUG_RAW_FRAMES
constexpr bool ENABLE_DEBUG_RAW_FRAMES = true;
#endif

enum class BmsFailReason : uint8_t
{
  None = 0,
  ShortFrame,
  SlaveMismatch,
  FunctionMismatch,
  CrcMismatch,
  LengthMismatch,
};

struct RuntimeConfig
{
  char wifiSsid[33];
  char wifiPassword[65];
  bool wifiHidden;

  uint8_t bmsDePin;
  uint8_t bmsRxPin;
  uint8_t bmsTxPin;
  uint8_t canRxPin;
  uint8_t canTxPin;
  bool enableDeyeCan;

  uint8_t preferredBmsId;
  uint8_t bmsIdMin;
  uint8_t bmsIdMax;

  char customCss[CUSTOM_CSS_MAX_LEN];
};

RuntimeConfig cfg;
Preferences configPrefs;
DNSServer dnsServer;
bool portalActive = false;
bool configLoaded = false;
bool rebootPending = false;
unsigned long rebootAtMs = 0;

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
  int avgCellTemp = 0;
  int tempPCB = 0;
  int tempSensors[4] = {0, 0, 0, 0};
  uint8_t tempSensorCount = 0;
  int batteryTemps[4] = {0, 0, 0, 0};
  uint8_t batteryTempCount = 0;
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
extern const char UI_SHARED_CSS[] PROGMEM;
extern const char UI_SHARED_JS[] PROGMEM;
extern const char INDEX_HTML[] PROGMEM;
extern const char DIAG_HTML[] PROGMEM;
extern const char SETUP_HTML[] PROGMEM;

void handleSetup();
void handleDashboard();

struct BmsDebugState
{
  uint32_t pollCount = 0;
  uint32_t successCount = 0;
  uint32_t failCount = 0;
  uint32_t shortFrameCount = 0;
  uint32_t slaveMismatchCount = 0;
  uint32_t functionMismatchCount = 0;
  uint32_t crcMismatchCount = 0;
  uint32_t lengthMismatchCount = 0;
  uint32_t lastPollStartMs = 0;
  uint32_t lastPollDurationMs = 0;
  uint32_t lastSuccessMs = 0;
  uint16_t lastFrameLen = 0;
  BmsFailReason lastFailReason = BmsFailReason::None;
  uint8_t lastResponse[80] = {0};
  uint8_t lastResponseLen = 0;
  bool canDriverInstalled = false;
  bool canStarted = false;
  esp_err_t canInitResult = ESP_FAIL;
  esp_err_t canStartResult = ESP_FAIL;
  uint32_t canTxCount = 0;
  uint32_t canTxFail = 0;
  esp_err_t lastCanTxResult = ESP_OK;
  esp_err_t canStatusResult = ESP_FAIL;
  twai_status_info_t canStatus = {};
  struct CanFrameSnapshot
  {
    uint32_t id = 0;
    uint8_t len = 0;
    uint8_t data[8] = {0};
  };
  static constexpr uint8_t CAN_FRAME_CAPACITY = 8;
  CanFrameSnapshot canFrames[CAN_FRAME_CAPACITY] = {};
  uint8_t canFrameCount = 0;
};

BmsDebugState dbg;

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

String formatIntArray(const int *values, uint8_t count)
{
  String out;
  for (uint8_t i = 0; i < count; i++)
  {
    if (i > 0)
      out += ", ";
    out += String(values[i]);
  }
  if (out.isEmpty())
  {
    out = "none";
  }
  return out;
}

String formatByteBuffer(const uint8_t *buf, size_t len)
{
  String out;
  for (size_t i = 0; i < len; i++)
  {
    if (i > 0)
      out += ' ';
    char tmp[4];
    snprintf(tmp, sizeof(tmp), "%02X", buf[i]);
    out += tmp;
  }
  return out;
}

String formatByteBufferDec(const uint8_t *buf, size_t len)
{
  String out;
  for (size_t i = 0; i < len; i++)
  {
    if (i > 0)
      out += ", ";
    out += String(buf[i]);
  }
  if (out.isEmpty())
  {
    out = "none";
  }
  return out;
}

String formatTempPair(uint16_t raw)
{
  return String((uint8_t)(raw >> 8)) + ", " + String((uint8_t)(raw & 0xFF));
}

String htmlEscape(const String &input)
{
  String out;
  out.reserve(input.length() * 2);
  for (size_t i = 0; i < input.length(); i++)
  {
    char ch = input[i];
    switch (ch)
    {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&#39;";
      break;
    default:
      out += ch;
      break;
    }
  }
  return out;
}

template <size_t N>
void copyStringField(char (&dest)[N], const String &value)
{
  snprintf(dest, N, "%s", value.c_str());
}

uint8_t clampPinValue(int value)
{
  if (value < 0)
    return 0;
  if (value > 39)
    return 39;
  return (uint8_t)value;
}

void setDefaultConfig()
{
  memset(&cfg, 0, sizeof(cfg));
  cfg.wifiHidden = false;
  cfg.bmsDePin = 4;
  cfg.bmsRxPin = 16;
  cfg.bmsTxPin = 17;
  cfg.canRxPin = 18;
  cfg.canTxPin = 19;
  cfg.enableDeyeCan = true;
  cfg.preferredBmsId = 16;
  cfg.bmsIdMin = 1;
  cfg.bmsIdMax = 16;
  cfg.customCss[0] = '\0';
}

void sanitizeConfig()
{
  cfg.bmsDePin = clampPinValue(cfg.bmsDePin);
  cfg.bmsRxPin = clampPinValue(cfg.bmsRxPin);
  cfg.bmsTxPin = clampPinValue(cfg.bmsTxPin);
  cfg.canRxPin = clampPinValue(cfg.canRxPin);
  cfg.canTxPin = clampPinValue(cfg.canTxPin);

  if (cfg.bmsIdMin == 0)
    cfg.bmsIdMin = 1;
  if (cfg.bmsIdMax == 0)
    cfg.bmsIdMax = 16;
  if (cfg.bmsIdMin > cfg.bmsIdMax)
  {
    uint8_t swapValue = cfg.bmsIdMin;
    cfg.bmsIdMin = cfg.bmsIdMax;
    cfg.bmsIdMax = swapValue;
  }
  if (cfg.preferredBmsId < cfg.bmsIdMin || cfg.preferredBmsId > cfg.bmsIdMax)
  {
    cfg.preferredBmsId = cfg.bmsIdMax;
  }
  cfg.customCss[CUSTOM_CSS_MAX_LEN - 1] = '\0';
}

bool loadConfig()
{
  setDefaultConfig();
  if (!configPrefs.begin(CONFIG_NAMESPACE, true))
  {
    return false;
  }

  const uint32_t magic = configPrefs.getUInt("magic", 0);
  const uint16_t version = configPrefs.getUShort("version", 0);
  if (magic != CONFIG_MAGIC || version != CONFIG_VERSION)
  {
    configPrefs.end();
    sanitizeConfig();
    return false;
  }

  String wifiSsid = configPrefs.getString("wifi_ssid", "");
  String wifiPassword = configPrefs.getString("wifi_password", "");
  String customCss = configPrefs.getString("custom_css", "");
  wifiSsid.trim();
  copyStringField(cfg.wifiSsid, wifiSsid);
  copyStringField(cfg.wifiPassword, wifiPassword);
  copyStringField(cfg.customCss, customCss);
  cfg.wifiHidden = configPrefs.getBool("wifi_hidden", false);
  cfg.bmsDePin = (uint8_t)configPrefs.getUChar("bms_de_pin", 4);
  cfg.bmsRxPin = (uint8_t)configPrefs.getUChar("bms_rx_pin", 16);
  cfg.bmsTxPin = (uint8_t)configPrefs.getUChar("bms_tx_pin", 17);
  cfg.canRxPin = (uint8_t)configPrefs.getUChar("can_rx_pin", 18);
  cfg.canTxPin = (uint8_t)configPrefs.getUChar("can_tx_pin", 19);
  cfg.enableDeyeCan = configPrefs.getBool("enable_can", true);
  cfg.preferredBmsId = (uint8_t)configPrefs.getUChar("pref_bms_id", 16);
  cfg.bmsIdMin = (uint8_t)configPrefs.getUChar("bms_id_min", 1);
  cfg.bmsIdMax = (uint8_t)configPrefs.getUChar("bms_id_max", 16);
  configPrefs.end();

  sanitizeConfig();
  configLoaded = true;
  return true;
}

void saveConfig()
{
  configPrefs.begin(CONFIG_NAMESPACE, false);
  configPrefs.putUInt("magic", CONFIG_MAGIC);
  configPrefs.putUShort("version", CONFIG_VERSION);
  configPrefs.putString("wifi_ssid", cfg.wifiSsid);
  configPrefs.putString("wifi_password", cfg.wifiPassword);
  configPrefs.putBool("wifi_hidden", cfg.wifiHidden);
  configPrefs.putUChar("bms_de_pin", cfg.bmsDePin);
  configPrefs.putUChar("bms_rx_pin", cfg.bmsRxPin);
  configPrefs.putUChar("bms_tx_pin", cfg.bmsTxPin);
  configPrefs.putUChar("can_rx_pin", cfg.canRxPin);
  configPrefs.putUChar("can_tx_pin", cfg.canTxPin);
  configPrefs.putBool("enable_can", cfg.enableDeyeCan);
  configPrefs.putUChar("pref_bms_id", cfg.preferredBmsId);
  configPrefs.putUChar("bms_id_min", cfg.bmsIdMin);
  configPrefs.putUChar("bms_id_max", cfg.bmsIdMax);
  configPrefs.putString("custom_css", cfg.customCss);
  configPrefs.end();
  configLoaded = true;
}

void clearConfig()
{
  configPrefs.begin(CONFIG_NAMESPACE, false);
  configPrefs.clear();
  configPrefs.end();
  setDefaultConfig();
  configLoaded = false;
}

String failReasonText(BmsFailReason reason)
{
  switch (reason)
  {
  case BmsFailReason::ShortFrame:
    return "short frame";
  case BmsFailReason::SlaveMismatch:
    return "slave id mismatch";
  case BmsFailReason::FunctionMismatch:
    return "function mismatch";
  case BmsFailReason::CrcMismatch:
    return "CRC mismatch";
  case BmsFailReason::LengthMismatch:
    return "length mismatch";
  case BmsFailReason::None:
  default:
    return "none";
  }
}

void debugPrintf(const char *fmt, ...)
{
  if (!ENABLE_DEBUG_SERIAL)
  {
    return;
  }

  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  Serial.println(buffer);
}

String canStateText(twai_state_t state)
{
  switch (state)
  {
  case TWAI_STATE_STOPPED:
    return "stopped";
  case TWAI_STATE_RUNNING:
    return "running";
  case TWAI_STATE_BUS_OFF:
    return "bus-off";
  case TWAI_STATE_RECOVERING:
    return "recovering";
  default:
    return "unknown";
  }
}

void refreshCanStatus()
{
  if (!cfg.enableDeyeCan || !dbg.canStarted)
  {
    return;
  }

  dbg.canStatusResult = twai_get_status_info(&dbg.canStatus);
  if (dbg.canStatusResult == ESP_OK && ENABLE_DEBUG_SERIAL)
  {
    debugPrintf("[CAN] state=%s tx_err=%u rx_err=%u tx_fail=%u rx_missed=%u bus_err=%u arb_lost=%u queue=%u",
                canStateText(dbg.canStatus.state).c_str(),
                dbg.canStatus.tx_error_counter,
                dbg.canStatus.rx_error_counter,
                dbg.canStatus.tx_failed_count,
                dbg.canStatus.rx_missed_count,
                dbg.canStatus.bus_error_count,
                dbg.canStatus.arb_lost_count,
                dbg.canStatus.msgs_to_tx);
  }
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
  case 0x1B:
    return "status";
  case 0x1C:
    return "warning";
  case 0x1D:
    return "protect";
  case 0x1E:
    return "cell count";
  case 0x1F:
    return "pack voltage limit";
  case 0x20:
    return "pack max capacity";
  case 0x21:
    return "battery temp 1/2";
  case 0x22:
    return "battery temp 3/4";
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
    {0x1000, "Balancing"},
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
  if (bms.status == 0 || (bms.warning == 2 && bms.protect == 4096 && fabs(bms.current) < 0.5f && bms.soc >= 95))
  {
    return "Standby";
  }
  if ((bms.status & 0x1000) != 0)
  {
    return "Balancing";
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
    if (i == 0x21 || i == 0x22)
    {
      html += formatTempPair(bms.rawRegs[i]);
    }
    else
    {
      html += String(bms.rawRegs[i]);
    }
    html += "</div>";
    html += "</div>";
  }
  html += "</div></div>";
  return html;
}

String buildDiagLastBmsFrameHtml()
{
  String html;
  html.reserve(160 + dbg.lastResponseLen * 4);
  html += "<div class='card'><h2 style='margin:0'>Last BMS frame</h2>";
  html += "<div class='soft'>Last UART/Modbus response from the BMS, not CAN.</div>";
  html += "<div class='reg-card' style='margin-top:12px'>";
  html += "<div class='can-row' style='margin-top:0'><span class='can-k'>Len</span><span class='can-v'>";
  html += String(dbg.lastResponseLen);
  html += "</span></div>";
  html += "<div class='reg-line reg-name' style='margin-top:8px'>Hex</div>";
  html += "<div class='reg-line reg-dec' style='word-break:break-all'>";
  html += htmlEscape(formatByteBuffer(dbg.lastResponse, dbg.lastResponseLen));
  html += "</div>";
  html += "</div></div>";
  return html;
}

static String canFrameTitle(uint32_t id)
{
  switch (id)
  {
  case 0x351:
    return "Status";
  case 0x355:
    return "SOC";
  case 0x356:
    return "Pack voltage / current";
  case 0x359:
    return "Node / mode";
  case 0x35E:
    return "Manufacturer";
  case 0x35F:
    return "Capacity / config";
  default:
    return "Unknown frame";
  }
}

static void appendCanField(String &html, const char *name, const String &value)
{
  html += "<div class='can-row'><span class='can-k'>";
  html += name;
  html += "</span><span class='can-v'>";
  html += value;
  html += "</span></div>";
}

String buildDiagCanHtml()
{
  String html;
  html.reserve(180 + dbg.canFrameCount * 220);
  html += "<div class='card'><h2 style='margin:0'>CAN frames</h2>";
  html += "<div class='soft'>Transmit frames captured from the latest cycle.</div>";
  html += "<div style='margin-top:12px;padding-top:12px;border-top:1px solid var(--line)'>";
  html += "<div class='flag-title'>CAN status</div>";
  html += "<div>CAN init: ";
  html += String((int)dbg.canInitResult);
  html += " | CAN start: ";
  html += String((int)dbg.canStartResult);
  html += " | CAN state: ";
  html += dbg.canStarted ? "started" : (cfg.enableDeyeCan ? "not started" : "disabled");
  html += "</div>";
  html += "<div>CAN TWAI state: ";
  html += dbg.canStatusResult == ESP_OK ? canStateText(dbg.canStatus.state) : "unknown";
  html += " | TX err: ";
  html += String(dbg.canStatus.tx_error_counter);
  html += " | RX err: ";
  html += String(dbg.canStatus.rx_error_counter);
  html += "</div>";
  html += "<div>CAN bus err: ";
  html += String(dbg.canStatus.bus_error_count);
  html += " | Arb lost: ";
  html += String(dbg.canStatus.arb_lost_count);
  html += " | Queue: ";
  html += String(dbg.canStatus.msgs_to_tx);
  html += "</div>";
  html += "<div>CAN tx: ";
  html += String(dbg.canTxCount);
  html += " | CAN tx fail: ";
  html += String(dbg.canTxFail);
  html += "</div>";
  html += "</div>";
  html += "<div class='reg-grid can-grid'>";
  if (dbg.canFrameCount == 0)
  {
    html += "<div class='soft'>No CAN frames captured yet.</div>";
  }
  for (uint8_t i = 0; i < dbg.canFrameCount; i++)
  {
    const auto &frame = dbg.canFrames[i];
    html += "<div class='reg-card can-card'>";
    html += "<div class='reg-line reg-addr'>ID ";
    html += formatHex16((uint16_t)frame.id);
    html += "</div>";
    html += "<div class='reg-line reg-hex'>LEN ";
    html += String(frame.len);
    html += "</div>";
    html += "<div class='reg-line reg-name'>Raw</div>";
    html += "<div class='reg-line reg-dec' style='word-break:break-all'>";
    html += htmlEscape(formatByteBuffer(frame.data, frame.len));
    html += "</div>";
    html += "<div class='reg-line reg-dec' style='word-break:break-all'>";
    html += htmlEscape(formatByteBufferDec(frame.data, frame.len));
    html += "</div>";
    html += "<div class='reg-line reg-name'>";
    html += canFrameTitle(frame.id);
    html += "</div>";
    switch (frame.id)
    {
    case 0x351:
    {
      uint16_t w0 = (uint16_t)frame.data[0] | ((uint16_t)frame.data[1] << 8);
      uint16_t w1 = (uint16_t)frame.data[2] | ((uint16_t)frame.data[3] << 8);
      uint16_t w2 = (uint16_t)frame.data[4] | ((uint16_t)frame.data[5] << 8);
      uint16_t w3 = (uint16_t)frame.data[6] | ((uint16_t)frame.data[7] << 8);
      appendCanField(html, "Word 0", String(w0));
      appendCanField(html, "Word 1", String(w1));
      appendCanField(html, "Word 2", String(w2));
      appendCanField(html, "Word 3", String(w3));
      break;
    }
    case 0x355:
      appendCanField(html, "SOC", String((uint16_t)frame.data[0] | ((uint16_t)frame.data[1] << 8)) + " %");
      appendCanField(html, "Word 1", String((uint16_t)frame.data[2] | ((uint16_t)frame.data[3] << 8)));
      appendCanField(html, "Word 2", String((uint16_t)frame.data[4] | ((uint16_t)frame.data[5] << 8)));
      appendCanField(html, "Word 3", String((uint16_t)frame.data[6] | ((uint16_t)frame.data[7] << 8)));
      break;
    case 0x356:
    {
      int16_t rawCurrent = (int16_t)(((uint16_t)frame.data[2]) | ((uint16_t)frame.data[3] << 8));
      float packV = ((uint16_t)frame.data[0] | ((uint16_t)frame.data[1] << 8)) / 100.0f;
      float currentA = rawCurrent / 10.0f;
      appendCanField(html, "Pack voltage", formatJsonFloat(packV, 2) + " V (" + String((uint16_t)frame.data[0] | ((uint16_t)frame.data[1] << 8)) + ")");
      appendCanField(html, "Pack current", formatJsonFloat(currentA, 1) + " A (" + String(rawCurrent) + ")");
      appendCanField(html, "Word 2", String((uint16_t)frame.data[4] | ((uint16_t)frame.data[5] << 8)));
      appendCanField(html, "Word 3", String((uint16_t)frame.data[6] | ((uint16_t)frame.data[7] << 8)));
      break;
    }
    case 0x359:
      appendCanField(html, "Byte 0", String(frame.data[0]));
      appendCanField(html, "Byte 1", String(frame.data[1]));
      appendCanField(html, "Byte 2", String(frame.data[2]));
      appendCanField(html, "Byte 3", String(frame.data[3]));
      appendCanField(html, "Byte 4", String((char)frame.data[4]));
      appendCanField(html, "Byte 5", String(frame.data[5]));
      appendCanField(html, "Byte 6", String(frame.data[6]));
      appendCanField(html, "Byte 7", String(frame.data[7]));
      break;
    case 0x35E:
    {
      char text[9] = {0};
      memcpy(text, frame.data, min<uint8_t>(8, frame.len));
      appendCanField(html, "ASCII", String(text));
      break;
    }
    case 0x35F:
      appendCanField(html, "Capacity 0", String((uint16_t)frame.data[0] | ((uint16_t)frame.data[1] << 8)) + " Ah");
      appendCanField(html, "Word 1", String((uint16_t)frame.data[2] | ((uint16_t)frame.data[3] << 8)));
      appendCanField(html, "Capacity 2", String((uint16_t)frame.data[4] | ((uint16_t)frame.data[5] << 8)) + " Ah");
      appendCanField(html, "Word 3", String((uint16_t)frame.data[6] | ((uint16_t)frame.data[7] << 8)));
      break;
    default:
      appendCanField(html, "Hex", htmlEscape(formatByteBuffer(frame.data, frame.len)));
      appendCanField(html, "Dec", htmlEscape(formatByteBufferDec(frame.data, frame.len)));
      break;
    }
    html += "</div>";
  }
  html += "</div></div>";
  return html;
}

void captureCanFrame(uint32_t id, uint8_t len, const uint8_t *data)
{
  for (uint8_t i = 0; i < dbg.canFrameCount; i++)
  {
    if (dbg.canFrames[i].id == id)
    {
      dbg.canFrames[i].len = len;
      memcpy(dbg.canFrames[i].data, data, len);
      return;
    }
  }

  if (dbg.canFrameCount >= BmsDebugState::CAN_FRAME_CAPACITY)
    return;

  auto &frame = dbg.canFrames[dbg.canFrameCount++];
  frame.id = id;
  frame.len = len;
  memcpy(frame.data, data, len);
}

void clearSnapshotData()
{
  bms.online = false;
  bms.rawCount = 0;
  bms.tempSensorCount = 0;
  bms.rawSlaveId = 0;
  bms.rawFunction = 0;
  bms.rawByteCount = 0;
  bms.crcReceived = 0;
  bms.crcCalculated = 0;
  dbg.canFrameCount = 0;
  memset(dbg.canFrames, 0, sizeof(dbg.canFrames));
}

void recordBmsFailure(BmsFailReason reason, const uint8_t *frame = nullptr, uint8_t frameLen = 0)
{
  dbg.failCount++;
  dbg.lastFailReason = reason;
  dbg.lastFrameLen = frameLen;
  if (frame != nullptr && frameLen > 0)
  {
    dbg.lastResponseLen = min<uint8_t>(frameLen, sizeof(dbg.lastResponse));
    memcpy(dbg.lastResponse, frame, dbg.lastResponseLen);
  }
  switch (reason)
  {
  case BmsFailReason::ShortFrame:
    dbg.shortFrameCount++;
    break;
  case BmsFailReason::SlaveMismatch:
    dbg.slaveMismatchCount++;
    break;
  case BmsFailReason::FunctionMismatch:
    dbg.functionMismatchCount++;
    break;
  case BmsFailReason::CrcMismatch:
    dbg.crcMismatchCount++;
    break;
  case BmsFailReason::LengthMismatch:
    dbg.lengthMismatchCount++;
    break;
  case BmsFailReason::None:
  default:
    break;
  }
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
  bms.tempSensorCount = 0;
  bms.tempSensors[bms.tempSensorCount++] = (int16_t)bms.rawRegs[0x13];
  bms.tempSensors[bms.tempSensorCount++] = (int16_t)bms.rawRegs[0x14];

  bms.batteryTempCount = 0;
  for (uint8_t reg = 0x21; reg <= 0x22; reg++)
  {
    uint16_t raw = bms.rawRegs[reg];
    bms.batteryTemps[bms.batteryTempCount++] = (int16_t)((raw >> 8) & 0xFF);
    bms.batteryTemps[bms.batteryTempCount++] = (int16_t)(raw & 0xFF);
  }

  int tempMin = bms.batteryTemps[0];
  int tempMax = bms.batteryTemps[0];
  int tempSum = 0;
  for (uint8_t i = 0; i < bms.batteryTempCount; i++)
  {
    tempSum += bms.batteryTemps[i];
    if (bms.batteryTemps[i] < tempMin)
      tempMin = bms.batteryTemps[i];
    if (bms.batteryTemps[i] > tempMax)
      tempMax = bms.batteryTemps[i];
  }
  bms.minCellTemp = tempMin;
  bms.maxCellTemp = tempMax;
  bms.avgCellTemp = bms.batteryTempCount > 0 ? (tempSum / bms.batteryTempCount) : 0;
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
  // 0x1F..0x23 are auxiliary/config fields; 0x21..0x22 appear to hold 4 battery temps packed as bytes.
}

bool requestBmsData(uint8_t slaveId)
{
  dbg.pollCount++;
  dbg.lastPollStartMs = millis();
  bms.lastTriedSlaveId = slaveId;

  while (Serial2.available())
  {
    Serial2.read();
  }

  uint8_t req[8] = {slaveId, 0x03, 0x00, 0x00, 0x00, BMS_REG_COUNT, 0x00, 0x00};
  uint16_t crc = getModbusCRC(req, 6);
  req[6] = crc & 0xFF;
  req[7] = (crc >> 8) & 0xFF;

  digitalWrite(cfg.bmsDePin, HIGH);
  delayMicroseconds(50);
  Serial2.write(req, 8);
  Serial2.flush();
  delayMicroseconds(50);
  digitalWrite(cfg.bmsDePin, LOW);

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

  dbg.lastPollDurationMs = millis() - dbg.lastPollStartMs;
  dbg.lastFrameLen = idx;
  dbg.lastResponseLen = min<uint8_t>(idx, sizeof(dbg.lastResponse));
  if (dbg.lastResponseLen > 0)
  {
    memcpy(dbg.lastResponse, res, dbg.lastResponseLen);
  }

  if (idx < 5)
  {
    bms.errorCount++;
    recordBmsFailure(BmsFailReason::ShortFrame, res, (uint8_t)idx);
    clearSnapshotData();
    debugPrintf("[BMS] slave=%u fail=short frame len=%d poll=%lu ms", slaveId, idx, dbg.lastPollDurationMs);
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
    recordBmsFailure(bms.rawSlaveId != slaveId ? BmsFailReason::SlaveMismatch : BmsFailReason::FunctionMismatch, res, (uint8_t)idx);
    debugPrintf("[BMS] slave=%u fail=%s rx_slave=%u rx_fn=0x%02X len=%d frame=%s",
                slaveId,
                bms.rawSlaveId != slaveId ? "slave id mismatch" : "function mismatch",
                bms.rawSlaveId,
                bms.rawFunction,
                idx,
                ENABLE_DEBUG_RAW_FRAMES ? formatByteBuffer(res, idx).c_str() : "");
    clearSnapshotData();
    return false;
  }

  if (bms.crcReceived != bms.crcCalculated)
  {
    bms.errorCount++;
    recordBmsFailure(BmsFailReason::CrcMismatch, res, (uint8_t)idx);
    debugPrintf("[BMS] slave=%u fail=crc mismatch rx=%s calc=%s len=%d frame=%s",
                slaveId,
                formatHex16(bms.crcReceived).c_str(),
                formatHex16(bms.crcCalculated).c_str(),
                idx,
                ENABLE_DEBUG_RAW_FRAMES ? formatByteBuffer(res, idx).c_str() : "");
    clearSnapshotData();
    return false;
  }

  const int expectedBytes = 3 + (BMS_RAW_REGS * 2) + 2;
  if (idx < expectedBytes)
  {
    bms.errorCount++;
    recordBmsFailure(BmsFailReason::LengthMismatch, res, (uint8_t)idx);
    debugPrintf("[BMS] slave=%u fail=length mismatch rx=%d expected=%d frame=%s",
                slaveId,
                idx,
                expectedBytes,
                ENABLE_DEBUG_RAW_FRAMES ? formatByteBuffer(res, idx).c_str() : "");
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
  dbg.successCount++;
  dbg.lastSuccessMs = millis();
  dbg.lastFailReason = BmsFailReason::None;
  if (ENABLE_DEBUG_SERIAL)
  {
    debugPrintf("[BMS] slave=%u ok len=%d poll=%lu ms V=%.2f I=%.2f SOC=%d raw=%s",
                slaveId,
                idx,
                dbg.lastPollDurationMs,
                bms.packV,
                bms.current,
                bms.soc,
                ENABLE_DEBUG_RAW_FRAMES ? formatByteBuffer(res, idx).c_str() : "");
  }
  return true;
}

void sendCanFrame(uint32_t id, uint8_t len, uint8_t *data)
{
  if (!cfg.enableDeyeCan)
  {
    return;
  }
  captureCanFrame(id, len, data);
  dbg.canTxCount++;
  twai_message_t message = {};
  message.identifier = id;
  message.extd = 0;
  message.data_length_code = len;
  for (int i = 0; i < len; i++)
  {
    message.data[i] = data[i];
  }
  dbg.lastCanTxResult = twai_transmit(&message, pdMS_TO_TICKS(10));
  if (dbg.lastCanTxResult != ESP_OK)
  {
    dbg.canTxFail++;
    if (ENABLE_DEBUG_SERIAL)
    {
      debugPrintf("[CAN] tx fail id=0x%03lX err=%d", (unsigned long)id, (int)dbg.lastCanTxResult);
    }
    refreshCanStatus();
    if (dbg.canStatusResult == ESP_OK && dbg.canStatus.state == TWAI_STATE_BUS_OFF)
    {
      esp_err_t recovery = twai_initiate_recovery();
      debugPrintf("[CAN] bus-off recovery start: %d", (int)recovery);
    }
  }
}

void sendDeyeCanTelemetry()
{
  if (!cfg.enableDeyeCan || !bms.online)
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

String buildDiagHtml(uint16_t refreshSeconds)
{
  String html = FPSTR(DIAG_HTML);
  html.replace("__UI_THEME_BASE_VARS__", String(FPSTR(UI_THEME_BASE_VARS)));
  html.replace("__UI_SHARED_CSS__", String(FPSTR(UI_SHARED_CSS)));
  html.replace("__CUSTOM_CSS__", String(cfg.customCss));
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
  html.replace("__DEBUG_POLL_COUNT__", String(dbg.pollCount));
  html.replace("__DEBUG_SUCCESS_COUNT__", String(dbg.successCount));
  html.replace("__DEBUG_FAIL_COUNT__", String(dbg.failCount));
  html.replace("__DEBUG_LAST_FAIL__", failReasonText(dbg.lastFailReason));
  html.replace("__DEBUG_LAST_POLL_MS__", String(dbg.lastPollDurationMs));
  html.replace("__DEBUG_LAST_FRAME_LEN__", String(dbg.lastFrameLen));
  html.replace("__DEBUG_TEMP_SENSORS__", formatIntArray(bms.tempSensors, bms.tempSensorCount));
  html.replace("__DEBUG_TEMP_SENSOR_COUNT__", String(bms.tempSensorCount));
  String diagBlocks = buildDiagRawRegistersHtml();
  diagBlocks += buildDiagLastBmsFrameHtml();
  diagBlocks += buildDiagCanHtml();
  html.replace("__RAW_REGS__", diagBlocks);
  html.replace("__LAST_BMS_FRAME__", "");
  return html;
}

String buildSetupHtml()
{
  String html = FPSTR(SETUP_HTML);
  html.replace("__UI_THEME_BASE_VARS__", String(FPSTR(UI_THEME_BASE_VARS)));
  html.replace("__UI_SHARED_CSS__", String(FPSTR(UI_SHARED_CSS)));
  html.replace("__CUSTOM_CSS__", String(cfg.customCss));
  html.replace("__STATUS__", portalActive
                                   ? String("Access point is active on ") + AP_SSID + " (" + AP_IP.toString() + ")"
                                   : String("Wi-Fi state: ") + (WiFi.status() == WL_CONNECTED ? String("connected as ") + WiFi.SSID() : String("not connected")));
  if (WiFi.status() == WL_CONNECTED && cfg.wifiSsid[0] != '\0')
  {
    html.replace("__TOP_ACTION__", String("<div><a href='/'>Back to dashboard</a></div>"));
  }
  else
  {
    html.replace("__TOP_ACTION__", String("<div class='pill live'>") + (portalActive ? "AP mode" : "setup") + "</div>");
  }
  html.replace("__WIFI_SSID__", htmlEscape(String(cfg.wifiSsid)));
  html.replace("__WIFI_HIDDEN_CHECKED__", cfg.wifiHidden ? "checked" : "");
  html.replace("__ENABLE_CAN_CHECKED__", cfg.enableDeyeCan ? "checked" : "");
  html.replace("__BMS_DE_PIN__", String(cfg.bmsDePin));
  html.replace("__BMS_RX_PIN__", String(cfg.bmsRxPin));
  html.replace("__BMS_TX_PIN__", String(cfg.bmsTxPin));
  html.replace("__CAN_RX_PIN__", String(cfg.canRxPin));
  html.replace("__CAN_TX_PIN__", String(cfg.canTxPin));
  html.replace("__PREFERRED_BMS_ID__", String(cfg.preferredBmsId));
  html.replace("__BMS_ID_MIN__", String(cfg.bmsIdMin));
  html.replace("__BMS_ID_MAX__", String(cfg.bmsIdMax));
  html.replace("__CUSTOM_CSS_TEXT__", htmlEscape(String(cfg.customCss)));
  return html;
}

void handleDashboard()
{
  String html = FPSTR(INDEX_HTML);
  html.replace("__UI_THEME_BASE_VARS__", String(FPSTR(UI_THEME_BASE_VARS)));
  html.replace("__UI_SHARED_CSS__", String(FPSTR(UI_SHARED_CSS)));
  html.replace("__CUSTOM_CSS__", String(cfg.customCss));
  html.replace("__UI_SHARED_JS__", String(FPSTR(UI_SHARED_JS)));
  server.send(200, "text/html", html);
}

void handleRoot()
{
  if (WiFi.status() != WL_CONNECTED || cfg.wifiSsid[0] == '\0')
  {
    handleSetup();
    return;
  }
  handleDashboard();
}

void handleSetup()
{
  server.send(200, "text/html", buildSetupHtml());
}

void scheduleReboot(unsigned long delayMs = 1500UL)
{
  rebootPending = true;
  rebootAtMs = millis() + delayMs;
}

void handleSave()
{
  if (server.method() != HTTP_POST)
  {
    server.send(405, "text/plain", "POST only");
    return;
  }

  copyStringField(cfg.wifiSsid, server.arg("wifi_ssid"));
  if (server.hasArg("wifi_password") && server.arg("wifi_password").length() > 0)
  {
    copyStringField(cfg.wifiPassword, server.arg("wifi_password"));
  }
  cfg.wifiHidden = server.hasArg("wifi_hidden");
  cfg.enableDeyeCan = server.hasArg("enable_deye_can");
  cfg.bmsDePin = clampPinValue(server.arg("bms_de_pin").toInt());
  cfg.bmsRxPin = clampPinValue(server.arg("bms_rx_pin").toInt());
  cfg.bmsTxPin = clampPinValue(server.arg("bms_tx_pin").toInt());
  cfg.canRxPin = clampPinValue(server.arg("can_rx_pin").toInt());
  cfg.canTxPin = clampPinValue(server.arg("can_tx_pin").toInt());
  cfg.preferredBmsId = (uint8_t)server.arg("preferred_bms_id").toInt();
  cfg.bmsIdMin = (uint8_t)server.arg("bms_id_min").toInt();
  cfg.bmsIdMax = (uint8_t)server.arg("bms_id_max").toInt();
  copyStringField(cfg.customCss, server.arg("custom_css"));
  sanitizeConfig();
  saveConfig();
  debugPrintf("[CFG] saved ssid='%s' can=%d bms=%u range=%u..%u",
              cfg.wifiSsid,
              cfg.enableDeyeCan ? 1 : 0,
              cfg.preferredBmsId,
              cfg.bmsIdMin,
              cfg.bmsIdMax);

  String html;
  html.reserve(512);
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2;url=/setup'>";
  html += "<title>Saved</title><style>body{font-family:Arial,Helvetica,sans-serif;background:#121212;color:#f2f2f2;padding:24px}</style></head><body>";
  html += "<h1>Saved</h1><p>Settings were saved. Rebooting now...</p></body></html>";
  server.send(200, "text/html", html);
  scheduleReboot();
}

void handleReset()
{
  if (server.method() != HTTP_POST)
  {
    server.send(405, "text/plain", "POST only");
    return;
  }
  clearConfig();
  debugPrintf("[CFG] cleared");
  server.send(200, "text/plain", "Configuration cleared. Rebooting...");
  scheduleReboot();
}

void handleReboot()
{
  if (server.method() != HTTP_POST)
  {
    server.send(405, "text/plain", "POST only");
    return;
  }
  server.send(200, "text/plain", "Rebooting...");
  scheduleReboot();
}

void handleNotFound()
{
  if (WiFi.status() != WL_CONNECTED || cfg.wifiSsid[0] == '\0')
  {
    handleSetup();
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void startPortal()
{
  if (portalActive)
  {
    return;
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  WiFi.softAP(AP_SSID);
  dnsServer.start(53, "*", AP_IP);
  portalActive = true;
  debugPrintf("[AP] started ssid=%s ip=%s", AP_SSID, AP_IP.toString().c_str());
}

void stopPortal()
{
  if (!portalActive)
  {
    return;
  }
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  debugPrintf("[AP] stopped");
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
  JsonObject debug = diagnostics.createNestedObject("debug");
  debug["poll_count"] = dbg.pollCount;
  debug["success_count"] = dbg.successCount;
  debug["fail_count"] = dbg.failCount;
  debug["short_frame_count"] = dbg.shortFrameCount;
  debug["slave_mismatch_count"] = dbg.slaveMismatchCount;
  debug["function_mismatch_count"] = dbg.functionMismatchCount;
  debug["crc_mismatch_count"] = dbg.crcMismatchCount;
  debug["length_mismatch_count"] = dbg.lengthMismatchCount;
  debug["last_poll_duration_ms"] = dbg.lastPollDurationMs;
  debug["last_success_ms"] = dbg.lastSuccessMs;
  debug["last_frame_len"] = dbg.lastFrameLen;
  debug["last_fail_reason"] = failReasonText(dbg.lastFailReason);
  debug["last_frame_hex"] = ENABLE_DEBUG_RAW_FRAMES ? formatByteBuffer(dbg.lastResponse, dbg.lastResponseLen) : "";
  debug["can_driver_installed"] = dbg.canDriverInstalled;
  debug["can_started"] = dbg.canStarted;
  debug["can_init_result"] = (int)dbg.canInitResult;
  debug["can_start_result"] = (int)dbg.canStartResult;
  debug["can_tx_count"] = dbg.canTxCount;
  debug["can_tx_fail"] = dbg.canTxFail;
  debug["can_last_tx_result"] = (int)dbg.lastCanTxResult;
  debug["can_status_result"] = (int)dbg.canStatusResult;
  debug["can_state"] = dbg.canStatusResult == ESP_OK ? canStateText(dbg.canStatus.state) : "unknown";
  debug["can_tx_error_counter"] = dbg.canStatus.tx_error_counter;
  debug["can_rx_error_counter"] = dbg.canStatus.rx_error_counter;
  debug["can_bus_error_count"] = dbg.canStatus.bus_error_count;
  debug["can_arb_lost_count"] = dbg.canStatus.arb_lost_count;
  debug["can_msgs_to_tx"] = dbg.canStatus.msgs_to_tx;

  JsonObject pack = root.createNestedObject("pack");
  pack["voltage_v"] = serialized(formatJsonFloat(bms.packV));
  pack["current_a"] = serialized(formatJsonFloat(bms.current));
  pack["soc_pct"] = bms.soc;
  pack["soh_pct"] = bms.soh;

  JsonObject temperatures = root.createNestedObject("temperatures");
  temperatures["pcb_c"] = bms.tempPCB;
  temperatures["temp_avg_c"] = bms.avgCellTemp;
  temperatures["cell_min_c"] = bms.minCellTemp;
  temperatures["cell_max_c"] = bms.maxCellTemp;
  temperatures["sensor_count"] = bms.tempSensorCount;
  JsonArray tempSensors = temperatures.createNestedArray("raw_c");
  for (uint8_t i = 0; i < bms.tempSensorCount; i++)
  {
    tempSensors.add(bms.tempSensors[i]);
  }
  temperatures["battery_temp_count"] = bms.batteryTempCount;
  JsonArray batteryTemps = temperatures.createNestedArray("battery_raw_c");
  for (uint8_t i = 0; i < bms.batteryTempCount; i++)
  {
    batteryTemps.add(bms.batteryTemps[i]);
  }

  JsonObject limits = root.createNestedObject("limits");
  limits["remaining_ah"] = bms.remainingAh;
  limits["max_charge_current_limit"] = bms.maxChargeCurrentLimit;
  limits["pack_voltage_limit"] = bms.rawRegs[0x1F];

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
  delay(200);
  setDefaultConfig();
  loadConfig();
  sanitizeConfig();
  debugPrintf("[CFG] loaded=%d ssid='%s' can=%d pins BMS[%u,%u,%u] CAN[%u,%u]",
              configLoaded ? 1 : 0,
              cfg.wifiSsid,
              cfg.enableDeyeCan ? 1 : 0,
              cfg.bmsDePin,
              cfg.bmsRxPin,
              cfg.bmsTxPin,
              cfg.canRxPin,
              cfg.canTxPin);
  debugPrintf("[BOOT] starting");
  debugPrintf("[BOOT] BMS RX=%u TX=%u DE=%u CAN RX=%u TX=%u", cfg.bmsRxPin, cfg.bmsTxPin, cfg.bmsDePin, cfg.canRxPin, cfg.canTxPin);
  Serial2.begin(9600, SERIAL_8N1, cfg.bmsRxPin, cfg.bmsTxPin);
  pinMode(cfg.bmsDePin, OUTPUT);
  digitalWrite(cfg.bmsDePin, LOW);

  if (cfg.enableDeyeCan)
  {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)cfg.canTxPin, (gpio_num_t)cfg.canRxPin, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    dbg.canInitResult = twai_driver_install(&g_config, &t_config, &f_config);
    dbg.canDriverInstalled = (dbg.canInitResult == ESP_OK);
    debugPrintf("[CAN] driver install: %d", (int)dbg.canInitResult);
    if (dbg.canDriverInstalled)
    {
      dbg.canStartResult = twai_start();
      dbg.canStarted = (dbg.canStartResult == ESP_OK);
      debugPrintf("[CAN] start: %d", (int)dbg.canStartResult);
      refreshCanStatus();
    }
  }
  else
  {
    debugPrintf("[CAN] disabled in config");
  }

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  if (cfg.wifiSsid[0] != '\0')
  {
    WiFi.mode(WIFI_STA);
    if (cfg.wifiHidden)
    {
      WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    }
    else
    {
      WiFi.setScanMethod(WIFI_FAST_SCAN);
    }
    WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);
    debugPrintf("[WIFI] connecting to SSID='%s' hidden=%d", cfg.wifiSsid, cfg.wifiHidden ? 1 : 0);
    unsigned long wifiStart = millis();
    while (millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS && WiFi.status() != WL_CONNECTED)
    {
      delay(250);
      server.handleClient();
    }
    if (WiFi.status() != WL_CONNECTED)
    {
      debugPrintf("[WIFI] connect timeout, enabling AP fallback");
      startPortal();
    }
  }
  else
  {
    debugPrintf("[WIFI] no saved SSID, starting AP");
    startPortal();
  }

  server.on("/", handleRoot);
  server.on("/setup", handleSetup);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/diag", handleDiag);
  server.on("/json", handleJson);
  server.onNotFound(handleNotFound);
  server.begin();
  debugPrintf("[HTTP] server started");
}

void loop()
{
  static unsigned long lastPoll = 0;
  static unsigned long lastLog = 0;
  static uint8_t scanId = cfg.preferredBmsId;

  if (rebootPending && millis() >= rebootAtMs)
  {
    ESP.restart();
  }

  if (portalActive)
  {
    dnsServer.processNextRequest();
  }

  if (portalActive && WiFi.status() == WL_CONNECTED && cfg.wifiSsid[0] != '\0')
  {
    stopPortal();
  }

  if (millis() - lastPoll >= 3000)
  {
    bool ok = requestBmsData(bms.online ? bms.activeSlaveId : scanId);
    if (!ok && !bms.online)
    {
      scanId++;
      if (scanId > cfg.bmsIdMax)
      {
        scanId = cfg.bmsIdMin;
      }
    }
    lastPoll = millis();
  }

  if (millis() - lastLog >= 3000)
  {
    lastLog = millis();
    refreshCanStatus();
    debugPrintf("[STAT] bms=%s active=%u tried=%u err=%d polls=%lu ok=%lu fail=%lu last_fail=%s can=%s tx=%lu/%lu",
                bms.online ? "ONLINE" : "OFFLINE",
                bms.activeSlaveId,
                bms.lastTriedSlaveId,
                bms.errorCount,
                dbg.pollCount,
                dbg.successCount,
                dbg.failCount,
                failReasonText(dbg.lastFailReason).c_str(),
                (cfg.enableDeyeCan ? (dbg.canStarted ? "started" : "init-failed") : "disabled"),
                dbg.canTxCount,
                dbg.canTxFail);
  }

  if (cfg.enableDeyeCan && bms.online)
    sendDeyeCanTelemetry();

  server.handleClient();
  delay(1);
}
