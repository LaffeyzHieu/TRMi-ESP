#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SHT31.h>
#include <SensirionI2cScd4x.h>
#include <PMS.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>


// ================================================
// TRMi Environment Monitor v0.1
// ESP32-S3 DevKit N16R8
// ================================================

// OLED SSD1306 - Wire
#define OLED_SDA       8
#define OLED_SCL       9
#define OLED_ADDR      0x3C
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64

// SHT30 + SCD40 - Shared Wire1
#define SENSOR_SDA     5
#define SENSOR_SCL     4
#define SHT_ADDR       0x44
#define SCD_ADDR       0x62

// PMS7003 - UART1
#define PMS_RX         18
#define PMS_TX         17
#define PMS_BAUD       9600

// Misc
#define BUZZER_PIN 10

// WiFi AP
#define WIFI_SSID "TRMi"
#define WIFI_PASS "trmi2025"

WebServer server(80);

// ================================================
// Shared data structures
// ================================================

struct SensorSnapshot {
    float    temp   = NAN;
    float    humi   = NAN;
    bool     tempOK = false;

    uint16_t co2    = 0;
    bool     co2OK  = false;

    uint16_t pm1    = 0;
    uint16_t pm25   = 0;
    uint16_t pm10   = 0;
    bool     pmOK   = false;
    uint32_t pmAge  = 0;

    uint32_t ts     = 0;
} snap;

struct AlarmThresholds {
    float    tempCrit  = 35.0f;
    float    humiLow   = 35.0f;
    uint16_t co2Warn   = 1000;
    uint16_t co2Crit   = 1500;
    float    pm25Warn  = 15.0f;
    float    pm25Crit  = 35.0f;
    float    pm10Warn  = 45.0f;
    float    pm10Crit  = 100.0f;
} thresholds;

enum AlarmLevel {
  ALARM_NORMAL = 0,
  ALARM_WARN,
  ALARM_CRIT
};

AlarmLevel currentAlarm = ALARM_NORMAL;
// Update intervals
constexpr uint32_t SHT_INTERVAL_MS     = 2000;
constexpr uint32_t SCD_CHECK_MS        = 500;
constexpr uint32_t OLED_INTERVAL_MS    = 500;
constexpr uint32_t PMS_STALE_MS        = 10000;

// ================================================
// Objects
// ================================================

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

Adafruit_SHT31 sht30(&Wire1);

SensirionI2cScd4x scd40;

HardwareSerial pmsSerial(1);
PMS pms(pmsSerial);
PMS::DATA pmsData;

// ================================================
// Sensor state
// ================================================

bool oledOK = false;
bool shtOK = false;
bool scdOK = false;

bool shtDataOK = false;
bool scdDataOK = false;
bool pmsDataOK = false;

float shtTemp = NAN;
float shtRH = NAN;

uint16_t co2ppm = 0;
float scdTemp = NAN;
float scdRH = NAN;

uint16_t pm1 = 0;
uint16_t pm25 = 0;
uint16_t pm10 = 0;

uint32_t lastSHTRead = 0;
uint32_t lastSCDCheck = 0;
uint32_t lastOLEDUpdate = 0;
uint32_t lastPMSRead = 0;

char scdErrorMessage[64];

// ================================================
// HTTP Handlers
// ================================================

void handleGetData() {
    StaticJsonDocument<256> doc;
    doc["ts"]   = snap.ts;
    doc["temp"] = snap.tempOK ? snap.temp : (float)0;
    doc["humi"] = snap.tempOK ? snap.humi : (float)0;
    doc["co2"]  = snap.co2OK  ? snap.co2  : 0;
    doc["pm1"]  = snap.pmOK   ? snap.pm1  : 0;
    doc["pm25"] = snap.pmOK   ? snap.pm25 : 0;
    doc["pm10"] = snap.pmOK   ? snap.pm10 : 0;

    JsonObject status = doc.createNestedObject("status");
    status["temp"] = snap.tempOK;
    status["co2"]  = snap.co2OK;
    status["pm"]   = snap.pmOK;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void handleGetThreshold() {
    StaticJsonDocument<256> doc;
    doc["temp_crit"]  = thresholds.tempCrit;
    doc["humi_low"]   = thresholds.humiLow;
    doc["co2_warn"]   = thresholds.co2Warn;
    doc["co2_crit"]   = thresholds.co2Crit;
    doc["pm25_warn"]  = thresholds.pm25Warn;
    doc["pm25_crit"]  = thresholds.pm25Crit;
    doc["pm10_warn"]  = thresholds.pm10Warn;
    doc["pm10_crit"]  = thresholds.pm10Crit;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void handlePostThreshold() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"no body\"}");
        return;
    }

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));

    if (err) {
        server.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    if (doc.containsKey("temp_crit"))  thresholds.tempCrit  = doc["temp_crit"];
    if (doc.containsKey("humi_low"))   thresholds.humiLow   = doc["humi_low"];
    if (doc.containsKey("co2_warn"))   thresholds.co2Warn   = doc["co2_warn"];
    if (doc.containsKey("co2_crit"))   thresholds.co2Crit   = doc["co2_crit"];
    if (doc.containsKey("pm25_warn"))  thresholds.pm25Warn  = doc["pm25_warn"];
    if (doc.containsKey("pm25_crit"))  thresholds.pm25Crit  = doc["pm25_crit"];
    if (doc.containsKey("pm10_warn"))  thresholds.pm10Warn  = doc["pm10_warn"];
    if (doc.containsKey("pm10_crit"))  thresholds.pm10Crit  = doc["pm10_crit"];

    server.send(200, "application/json", "{\"ok\":true}");
}

// ===============
// WEB
// ===============

void handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>TRMi Monitor</title>
    <style>
        body { font-family: monospace; background: #111; color: #eee; padding: 16px; }
        h2 { color: #7eb8f7; margin-bottom: 4px; }
        .card { background: #1e1e1e; border-radius: 8px; padding: 12px 16px; margin-bottom: 10px; }
        .label { color: #888; font-size: 12px; }
        .value { font-size: 22px; font-weight: bold; }
        .ok { color: #4caf50; }
        .warn { color: #ff9800; }
        .crit { color: #f44336; }
        .err { color: #555; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .status { font-size: 12px; color: #555; margin-top: 16px; }
    </style>
</head>
<body>
    <h2>🌡 TRMi Monitor</h2>
    <div class="grid">
        <div class="card">
            <div class="label">Nhiệt độ</div>
            <div class="value" id="temp">--</div>
        </div>
        <div class="card">
            <div class="label">Độ ẩm</div>
            <div class="value" id="humi">--</div>
        </div>
        <div class="card">
            <div class="label">CO₂</div>
            <div class="value" id="co2">--</div>
        </div>
        <div class="card">
            <div class="label">PM2.5</div>
            <div class="value" id="pm25">--</div>
        </div>
        <div class="card">
            <div class="label">PM10</div>
            <div class="value" id="pm10">--</div>
        </div>
        <div class="card">
            <div class="label">PM1</div>
            <div class="value" id="pm1">--</div>
        </div>
    </div>
    <div class="status" id="status">Đang tải...</div>

<script>
function colorClass(id, val) {
    const rules = {
        co2:  [[1500,'crit'],[1000,'warn'],[0,'ok']],
        pm25: [[35,'crit'],[15,'warn'],[0,'ok']],
        pm10: [[100,'crit'],[45,'warn'],[0,'ok']],
        pm1:  [[100,'crit'],[0,'ok']],
        temp: [[35,'crit'],[0,'ok']],
        humi: [[0,'ok']]
    };
    const r = rules[id] || [];
    for (const [threshold, cls] of r) {
        if (val >= threshold) return cls;
    }
    return 'ok';
}

async function fetchData() {
    try {
        const res = await fetch('/data');
        const d = await res.json();

        const fields = {
            temp:  { val: d.status.temp ? d.temp.toFixed(1) + ' °C' : '--',  raw: d.temp },
            humi:  { val: d.status.temp ? d.humi.toFixed(1) + ' %'  : '--',  raw: d.humi },
            co2:   { val: d.status.co2  ? d.co2  + ' ppm'           : '--',  raw: d.co2  },
            pm25:  { val: d.status.pm   ? d.pm25 + ' µg/m³'         : '--',  raw: d.pm25 },
            pm10:  { val: d.status.pm   ? d.pm10 + ' µg/m³'         : '--',  raw: d.pm10 },
            pm1:   { val: d.status.pm   ? d.pm1  + ' µg/m³'         : '--',  raw: d.pm1  },
        };

        for (const [id, f] of Object.entries(fields)) {
            const el = document.getElementById(id);
            el.textContent = f.val;
            el.className = 'value ' + (f.val === '--' ? 'err' : colorClass(id, f.raw));
        }

        const ts = new Date().toLocaleTimeString();
        document.getElementById('status').textContent = 'Cập nhật lúc ' + ts;

    } catch(e) {
        document.getElementById('status').textContent = 'Lỗi kết nối: ' + e.message;
    }
}

fetchData();
setInterval(fetchData, 45000);
</script>
</body>
</html>
)rawliteral";

    server.send(200, "text/html", html);
}

void setupWiFi() {
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    WiFi.softAPConfig(
        IPAddress(192,168,4,1),
        IPAddress(192,168,4,1),
        IPAddress(255,255,255,0)
    );

    server.on("/",         HTTP_GET,  handleRoot);
    server.on("/data",      HTTP_GET,  handleGetData);
    server.on("/threshold", HTTP_GET,  handleGetThreshold);
    server.on("/threshold", HTTP_POST, handlePostThreshold);
    server.begin();

    Serial.println("[WiFi] AP started: TRMi");
    Serial.println("[WiFi] IP: 192.168.4.1");
}

// ================================================
// Setup
// ================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println();
  Serial.println("================================");
  Serial.println("TRMi Environment Monitor v0.1");
  Serial.println("ESP32-S3");
  Serial.println("================================");

  // OLED bus
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000);

  oledOK = display.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDR
  );

  if (oledOK) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("TRMi ENV MONITOR");
    display.println("Initializing...");
    display.display();

    Serial.println("[OLED] OK");
  } else {
    Serial.println("[OLED] INIT FAILED");
  }

  // Shared sensor I2C bus
  Wire1.begin(SENSOR_SDA, SENSOR_SCL);
  Wire1.setClock(100000);

  // SHT30
  shtOK = sht30.begin(SHT_ADDR);

  if (shtOK) {
    Serial.println("[SHT30] OK - 0x44");
  } else {
    Serial.println("[SHT30] INIT FAILED");
  }

  // SCD40
  scd40.begin(Wire1, SCD_ADDR);
  delay(30);

  // Ensure a clean start of periodic measurement.
  // An error from stopPeriodicMeasurement() is not
  // necessarily fatal if the sensor was already idle.
  int16_t error = scd40.stopPeriodicMeasurement();

  if (error != 0) {
    errorToString(
      error,
      scdErrorMessage,
      sizeof(scdErrorMessage)
    );

    Serial.print("[SCD40] Stop notice: ");
    Serial.println(scdErrorMessage);
  }

  delay(500);

  error = scd40.startPeriodicMeasurement();

  if (error == 0) {
    scdOK = true;
    Serial.println("[SCD40] OK - 0x62");
    Serial.println("[SCD40] Periodic measurement started");
  } else {
    errorToString(
      error,
      scdErrorMessage,
      sizeof(scdErrorMessage)
    );

    Serial.print("[SCD40] START FAILED: ");
    Serial.println(scdErrorMessage);
  }

  // PMS7003
  pmsSerial.begin(
    PMS_BAUD,
    SERIAL_8N1,
    PMS_RX,
    PMS_TX
  );

  Serial.println("[PMS7003] UART1 initialized");
  Serial.println("[PMS7003] RX=GPIO18 TX=GPIO17");

  Serial.println("--------------------------------");
  Serial.println("Initialization complete.");

  setupWiFi();
}

// ================================================
// Read SHT30
// ================================================

void updateSHT30(uint32_t now) {
  if (!shtOK) return;

  if (now - lastSHTRead < SHT_INTERVAL_MS) {
    return;
  }

  lastSHTRead = now;

  float t;
  float h;

  if (sht30.readBoth(&t, &h) &&
      !isnan(t) &&
      !isnan(h) &&
      h >= 0.0f &&
      h <= 100.0f) {

      snap.temp   = t;
      snap.humi   = h;
      snap.tempOK = true;
      snap.ts     = now;

    Serial.print("[SHT30] ");
    Serial.print(snap.temp, 1);
    Serial.print(" C | ");
    Serial.print(snap.humi, 1);
    Serial.println(" %RH");

  } else {
    snap.tempOK = false;
    Serial.println("[SHT30] READ ERROR");
  }
}

// ================================================
// Read SCD40
// ================================================

void updateSCD40(uint32_t now) {
  if (!scdOK) return;

  if (now - lastSCDCheck < SCD_CHECK_MS) {
    return;
  }

  lastSCDCheck = now;

  bool dataReady = false;

  int16_t error = scd40.getDataReadyStatus(dataReady);

  if (error != 0) {
    errorToString(
      error,
      scdErrorMessage,
      sizeof(scdErrorMessage)
    );

    Serial.print("[SCD40] STATUS ERROR: ");
    Serial.println(scdErrorMessage);
    return;
  }

  if (!dataReady) {
    return;
  }

  uint16_t newCO2 = 0;
  float newTemp = NAN;
  float newRH = NAN;

  error = scd40.readMeasurement(
    newCO2,
    newTemp,
    newRH
  );

  if (error != 0 || newCO2 == 0 ||
      isnan(newTemp) || isnan(newRH)) {

    if (error != 0) {
      errorToString(
        error,
        scdErrorMessage,
        sizeof(scdErrorMessage)
      );

      Serial.print("[SCD40] READ ERROR: ");
      Serial.println(scdErrorMessage);
    }

    return;
  }

  snap.co2   = newCO2;
  snap.co2OK = true;
  snap.ts    = now;

  Serial.print("[SCD40] CO2=");
  Serial.print(snap.co2);
  Serial.print(" ppm | T=");
  Serial.print(newTemp, 1);
  Serial.print(" C | RH=");
  Serial.print(newRH, 1);
  Serial.println(" %");
}

// ================================================
// Read PMS7003
// ================================================

void updatePMS7003() {
  if (pms.read(pmsData)) {
    snap.pm1   = pmsData.PM_AE_UG_1_0;
    snap.pm25  = pmsData.PM_AE_UG_2_5;
    snap.pm10  = pmsData.PM_AE_UG_10_0;
    snap.pmOK  = true;
    snap.pmAge = millis();
    snap.ts    = millis();
    lastPMSRead = millis();

    static uint8_t pmsLogCount = 0;
    if (++pmsLogCount >= 10) {
      pmsLogCount = 0;
      Serial.print("[PMS7003] PM1.0=");
      Serial.print((int)snap.pm1);
      Serial.print(" | PM2.5=");
      Serial.print((int)snap.pm25);
      Serial.print(" | PM10=");
      Serial.println((int)snap.pm10);
    }
  }

  if (snap.pmOK &&
      millis() - lastPMSRead > PMS_STALE_MS) {
    snap.pmOK = false;
    Serial.println("[PMS7003] DATA TIMEOUT");
  }
}

// ================================================
// OLED
// ================================================

void drawOLED(uint32_t now) {
  if (!oledOK) return;

  if (now - lastOLEDUpdate < OLED_INTERVAL_MS) {
    return;
  }

  lastOLEDUpdate = now;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("TRMi ");
  display.print("S:");
  display.print(shtOK ? "OK" : "ER");
  display.print(" C:");
  display.print(scdOK ? "OK" : "ER");
  display.print(" P:");
  display.println(snap.pmOK ? "OK" : "--");

  display.setCursor(0, 9);
  display.print("CO2 : ");
  if (snap.co2OK) {
    display.print((int)snap.co2);
    display.print(" ppm");
  } else {
    display.print("-- ppm");
  }

  display.setCursor(0, 18);
  display.print("Temp: ");
  if (snap.tempOK) {
    display.print(snap.temp, 1);
    display.print(" C");
  } else {
    display.print("--.- C");
  }

  display.setCursor(0, 27);
  display.print("RH  : ");
  if (snap.tempOK) {
    display.print(snap.humi, 1);
    display.print(" %");
  } else {
    display.print("--.- %");
  }

  display.setCursor(0, 36);
  display.print("PM1 : ");
  if (snap.pmOK) {
    display.print((int)snap.pm1);
    display.print(" ug/m3");
  } else {
    display.print("-- ug/m3");
  }

  display.setCursor(0, 45);
  display.print("PM2.5:");
  if (snap.pmOK) {
    display.print((int)snap.pm25);
    display.print(" ug/m3");
  } else {
    display.print("-- ug/m3");
  }

  display.setCursor(0, 54);
  display.print("PM10: ");
  if (snap.pmOK) {
    display.print((int)snap.pm10);
    display.print(" ug/m3");
  } else {
    display.print("-- ug/m3");
  }

  display.display();
}

// ================================================
// TRMi Alarm Manager
// ================================================



constexpr uint32_t WARN_INTERVAL_MS = 60000;
constexpr uint32_t CRIT_INTERVAL_MS = 5000;
constexpr uint32_t BEEP_DURATION_MS = 200;

uint32_t lastBeepTime = 0;
uint32_t beepStartTime = 0;

bool buzzerActive = false;

// ================================================
// Determine alarm level
// ================================================

AlarmLevel getAlarmLevel() {
  bool warning = false;
  bool critical = false;

  if (snap.co2OK) {
      if (snap.co2 >= thresholds.co2Crit) critical = true;
      else if (snap.co2 >= thresholds.co2Warn) warning = true;
  }

  if (snap.pmOK) {
      if (snap.pm25 >= thresholds.pm25Crit) critical = true;
      else if (snap.pm25 >= thresholds.pm25Warn) warning = true;

      if (snap.pm10 >= thresholds.pm10Crit) critical = true;
      else if (snap.pm10 >= thresholds.pm10Warn) warning = true;
  }

  if (critical) return ALARM_CRIT;
  if (warning)  return ALARM_WARN;
  return ALARM_NORMAL;
}
// ================================================
// Non-blocking buzzer control
// ================================================

void updateBuzzer(uint32_t now) {
  AlarmLevel newAlarm = getAlarmLevel();

  // Alarm level changed
  if (newAlarm != currentAlarm) {
    currentAlarm = newAlarm;

    Serial.print("[ALARM] Level: ");

    switch (currentAlarm) {
      case ALARM_NORMAL:
        Serial.println("NORMAL");
        break;

      case ALARM_WARN:
        Serial.println("WARN");
        break;

      case ALARM_CRIT:
        Serial.println("CRIT");
        break;
    }

    // Stop an ongoing beep when returning to NORMAL
    if (currentAlarm == ALARM_NORMAL) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerActive = false;
      return;
    }

    // Trigger a beep immediately on escalation
    lastBeepTime = now -
      (currentAlarm == ALARM_CRIT
        ? CRIT_INTERVAL_MS
        : WARN_INTERVAL_MS);
  }

  if (currentAlarm == ALARM_NORMAL) {
    return;
  }

  // End beep without blocking sensor reads
  if (buzzerActive &&
      now - beepStartTime >= BEEP_DURATION_MS) {

    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
  }

  uint32_t interval =
    (currentAlarm == ALARM_CRIT)
      ? CRIT_INTERVAL_MS
      : WARN_INTERVAL_MS;

  // Start next beep
  if (!buzzerActive &&
      now - lastBeepTime >= interval) {

    digitalWrite(BUZZER_PIN, HIGH);

    buzzerActive = true;
    beepStartTime = now;
    lastBeepTime = now;
  }
}

// ================================================
// Main loop
// ================================================

void loop() {
  uint32_t now = millis();

  updatePMS7003();

  updateSHT30(now);
  updateSCD40(now);

  server.handleClient();

  updateBuzzer(millis());

  drawOLED(millis());
}