#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <XPowersLib.h>
#include <TinyGPS++.h>
#include <Adafruit_BME280.h>
#include <Adafruit_AS7331.h>
#include <RTClib.h>
#include <WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <math.h>

// =========================
// 引脚定义 (T-Beam S3 Supreme)
// =========================
#define I2C_SDA        17
#define I2C_SCL        18
#define PMU_SDA        42
#define PMU_SCL        41
#define GPS_RX_PIN     9
#define GPS_TX_PIN     8
#define GPS_EN_PIN     7

#define SD_MOSI        35
#define SD_SCLK        36
#define SD_MISO        37
#define SD_CS          47

#define IMU_CS         34

const char* ssid = "ChinaNet-CD5F";
const char* password = "12345678";

static const int TZ_OFFSET_MINUTES = 8 * 60;
static const uint32_t UI_REFRESH_MS = 500;
static const uint32_t UV_REFRESH_MS = 2000;
static const uint32_t LOG_INTERVAL_MS = 60000;   // 每分钟记录一次
static const uint32_t SD_REFRESH_MS = 3000;
static const int HISTORY_ROWS = 20;

// 主文件固定为 data.csv；不兼容时切换到 fallback 文件
const char* CSV_PRIMARY_PATH = "/data.csv";
const char* CSV_HEADER =
"Date,Time,Temp(℃),Hum(%),Press(hPa),UVA(uW/cm²),UVB(uW/cm²),UVC(uW/cm²),UVOverflow(0/1),Alt(m),Lat(°),Lon(°),Sats,GpsFix(0/1),Batt(%),Batt(mV),Charging(0/1),WiFiRSSI(dBm)";

// =========================
// 硬件对象
// =========================
TwoWire PMUWire = TwoWire(1);
XPowersAXP2101 PMU;
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
TinyGPSPlus gps;
RTC_PCF8563 rtc;
Adafruit_BME280 bme;
Adafruit_AS7331 as7331 = Adafruit_AS7331();
SPIClass sdSPI(FSPI);
WebServer server(80);

// =========================
// 状态变量
// =========================
bool pmu_ok = false;
bool bme_ok = false;
bool rtc_ok = false;
bool uv_ok = false;
bool sd_ok = false;
bool rtc_synced = false;
bool webUrlPrinted = false;
bool g_uvOverflow = false;

// CSV 安全策略状态
bool csv_primary_header_ok = false;      // /data.csv 是否与当前表头兼容
bool csv_logging_enabled = false;        // 当前是否允许写日志
bool csv_using_fallback_file = false;    // 当前是否在用 fallback 文件
String g_activeCsvPath = "/data.csv";    // 当前实际写入的文件
String g_preservedPrimaryPath = "";      // 旧 data.csv 被保留时，记录它（通常就是 /data.csv）

uint32_t lastUiRefreshMs = 0;
uint32_t lastLogMs = 0;
uint32_t lastRtcSyncMs = 0;
uint32_t lastSdRefreshMs = 0;
uint32_t lastUvReadMs = 0;
uint32_t lastSensorUpdateMs = 0;

bool lastSdWriteOk = false;
bool lastHttpOk = false;
int lastHttpCode = 0;

// =========================
// 最近一次缓存数据
// =========================
float g_temp = NAN;
float g_hum = NAN;
float g_pres = NAN;

float g_uva = NAN;
float g_uvb = NAN;
float g_uvc = NAN;

double g_alt = NAN;
double g_lat = NAN;
double g_lon = NAN;

int g_sats = 0;
bool g_gpsFix = false;

int g_batt = 0;
int g_battMv = 0;
bool g_charging = false;

String g_timeText = "--:--";
String g_dateText = "--/--";
String g_timestamp = "2026-01-01 00:00:00";

int g_wifiRssi = 0;
String g_wifiIp = "0.0.0.0";

// =========================
// 工具函数
// =========================
static DateTime gpsToLocalDateTime() {
    DateTime utc(
        gps.date.year(),
        gps.date.month(),
        gps.date.day(),
        gps.time.hour(),
        gps.time.minute(),
        gps.time.second()
    );
    return utc + TimeSpan(TZ_OFFSET_MINUTES * 60);
}

static DateTime getNowTime() {
    if (rtc_ok) {
        DateTime now = rtc.now();
        if (now.year() >= 2024 && now.year() <= 2099) {
            return now;
        }
    }

    if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024) {
        return gpsToLocalDateTime();
    }

    return DateTime(2026, 1, 1, 0, 0, 0);
}

static void updateTimeText(const DateTime &now) {
    char timeBuf[9];
    char dateBuf[11];
    char tsBuf[20];

    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
    snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", now.year(), now.month(), now.day());
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second());

    g_timeText = String(timeBuf);
    g_dateText = String(dateBuf);
    g_timestamp = String(tsBuf);
}

static String yesNo(bool v) { return v ? "YES" : "NO"; }
static String onOff(bool v) { return v ? "ON" : "OFF"; }
static String okBad(bool v) { return v ? "OK" : "FAIL"; }

static String uptimeString() {
    uint32_t sec = millis() / 1000;
    uint32_t d = sec / 86400;
    sec %= 86400;
    uint32_t h = sec / 3600;
    sec %= 3600;
    uint32_t m = sec / 60;
    sec %= 60;

    char buf[32];
    snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u", d, h, m, sec);
    return String(buf);
}

static uint32_t nextLogInSeconds() {
    uint32_t elapsed = millis() - lastLogMs;
    if (elapsed >= LOG_INTERVAL_MS) return 0;
    return (LOG_INTERVAL_MS - elapsed) / 1000;
}

static uint32_t lastSampleAgeSeconds() {
    if (lastSensorUpdateMs == 0) return 0;
    return (millis() - lastSensorUpdateMs) / 1000;
}

static String baseNameFromPath(const String &path) {
    int idx = path.lastIndexOf('/');
    if (idx >= 0 && idx < path.length() - 1) return path.substring(idx + 1);
    return path;
}

static String csvField(const String &line, int fieldIndex) {
    int start = 0;
    int currentField = 0;

    for (int i = 0; i <= line.length(); i++) {
        if (i == line.length() || line[i] == ',') {
            if (currentField == fieldIndex) {
                return line.substring(start, i);
            }
            currentField++;
            start = i + 1;
        }
    }
    return "";
}

// =========================
// 串口帮助 / 状态
// =========================
static void printHelp() {
    Serial.println();
    Serial.println("===== SERIAL COMMANDS =====");
    Serial.println("h  -> help");
    Serial.println("s  -> show status");
    Serial.println("d  -> dump ACTIVE csv file");
    Serial.println("p  -> dump PRIMARY data.csv");
    Serial.println("e  -> erase ACTIVE log file (authorized)");
    Serial.println("r  -> rebuild PRIMARY data.csv and switch logging back to it (authorized)");
    Serial.println("===========================");
    Serial.println();
}

static void printStatus() {
    Serial.println();
    Serial.println("===== SYSTEM STATUS =====");
    Serial.print("SD: ");
    Serial.println(sd_ok ? "ONLINE" : "OFFLINE");

    Serial.print("WiFi: ");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("CONNECTED  IP=");
        Serial.print(WiFi.localIP());
        Serial.print("  RSSI=");
        Serial.println(WiFi.RSSI());
    } else {
        Serial.println("DISCONNECTED");
    }

    Serial.print("RTC: ");
    Serial.println(rtc_ok ? "OK" : "FAIL");

    Serial.print("RTC synced: ");
    Serial.println(rtc_synced ? "YES" : "NO");

    Serial.print("BME280: ");
    Serial.println(bme_ok ? "OK" : "FAIL");

    Serial.print("UV(AS7331): ");
    Serial.println(uv_ok ? "OK" : "FAIL");

    Serial.print("UV overflow: ");
    Serial.println(g_uvOverflow ? "YES" : "NO");

    Serial.print("GPS Fix: ");
    Serial.println(g_gpsFix ? "YES" : "NO");

    Serial.print("Satellites: ");
    Serial.println(g_sats);

    Serial.print("Primary CSV Header: ");
    Serial.println(csv_primary_header_ok ? "OK" : "MISMATCH");

    Serial.print("Logging Enabled: ");
    Serial.println(csv_logging_enabled ? "YES" : "NO");

    Serial.print("Logging Mode: ");
    Serial.println(csv_using_fallback_file ? "FALLBACK" : "PRIMARY");

    Serial.print("Active Log File: ");
    Serial.println(g_activeCsvPath);

    if (csv_using_fallback_file) {
        Serial.print("Preserved Primary File: ");
        Serial.println(g_preservedPrimaryPath);
    }

    Serial.println("=========================");
    Serial.println();
}

// =========================
// Wi-Fi
// =========================
void initWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.println("WiFi: Connecting...");
}

void ensureWiFiConnected() {
    static uint32_t lastRetryMs = 0;
    static bool wasConnected = false;

    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected) {
        g_wifiRssi = WiFi.RSSI();
        g_wifiIp = WiFi.localIP().toString();
    } else {
        g_wifiRssi = 0;
        g_wifiIp = "0.0.0.0";
    }

    if (connected && !wasConnected) {
        Serial.print("WiFi: Connected. IP = ");
        Serial.println(WiFi.localIP());

        if (!webUrlPrinted) {
            Serial.print("Web: Open http://");
            Serial.print(WiFi.localIP());
            Serial.println("/");
            webUrlPrinted = true;
        }
    }

    wasConnected = connected;

    if (connected) return;

    if (millis() - lastRetryMs > 10000) {
        lastRetryMs = millis();
        Serial.println("WiFi: Reconnecting...");
        WiFi.disconnect();
        WiFi.begin(ssid, password);
    }
}

// =========================
// CSV / SD 核心策略
// =========================
bool readFirstLine(const String &path, String &outLine) {
    if (!SD.exists(path.c_str())) return false;

    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) return false;

    outLine = file.readStringUntil('\n');
    outLine.trim();
    file.close();
    return true;
}

bool headerMatchesPath(const String &path) {
    String firstLine;
    if (!readFirstLine(path, firstLine)) return false;
    return firstLine == String(CSV_HEADER);
}

bool createCsvWithHeader(const String &path, bool overwriteExisting) {
    if (overwriteExisting && SD.exists(path.c_str())) {
        if (!SD.remove(path.c_str())) {
            Serial.print("SD: Failed to remove ");
            Serial.println(path);
            return false;
        }
    }

    File file = SD.open(path.c_str(), FILE_WRITE);
    if (!file) {
        Serial.print("SD: Failed to create ");
        Serial.println(path);
        return false;
    }

    file.println(CSV_HEADER);
    file.close();
    return true;
}

String chooseFallbackCsvPath() {
    if (!SD.exists("/data_new.csv")) return "/data_new.csv";

    for (int i = 1; i <= 999; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "/data_new_%03d.csv", i);
        if (!SD.exists(buf)) return String(buf);
    }

    return "/data_recover.csv";
}

// 授权操作：清空当前活动日志文件并重建表头
bool eraseActiveCsvAuthorized() {
    if (!sd_ok) return false;

    if (!createCsvWithHeader(g_activeCsvPath, true)) {
        return false;
    }

    csv_logging_enabled = true;

    // 如果当前活动文件就是主文件，则说明主文件已经恢复为兼容格式
    if (g_activeCsvPath == String(CSV_PRIMARY_PATH)) {
        csv_primary_header_ok = true;
        csv_using_fallback_file = false;
        g_preservedPrimaryPath = "";
    }

    return true;
}

// 授权操作：重建主文件 data.csv，并切回主文件写入
bool rebuildPrimaryCsvAuthorized() {
    if (!sd_ok) return false;

    if (!createCsvWithHeader(String(CSV_PRIMARY_PATH), true)) {
        return false;
    }

    csv_primary_header_ok = true;
    csv_logging_enabled = true;
    csv_using_fallback_file = false;
    g_activeCsvPath = String(CSV_PRIMARY_PATH);
    g_preservedPrimaryPath = "";

    Serial.println("SD: Primary data.csv rebuilt and logging switched back to it.");
    return true;
}

// 安全策略：绝不自动删除主文件；若不兼容，则保留并切到新文件写
bool configureCsvLoggingTargets() {
    csv_logging_enabled = false;
    g_preservedPrimaryPath = "";
    csv_primary_header_ok = false;

    // 如果之前已经在用 fallback 文件，并且该文件还存在且表头正确，则优先继续用它
    if (g_activeCsvPath != String(CSV_PRIMARY_PATH) &&
        SD.exists(g_activeCsvPath.c_str()) &&
        headerMatchesPath(g_activeCsvPath)) {

        csv_logging_enabled = true;
        csv_using_fallback_file = true;
        csv_primary_header_ok = SD.exists(CSV_PRIMARY_PATH) ? headerMatchesPath(String(CSV_PRIMARY_PATH)) : false;
        g_preservedPrimaryPath = String(CSV_PRIMARY_PATH);

        Serial.print("SD: Reusing existing fallback log file: ");
        Serial.println(g_activeCsvPath);
        return true;
    }

    // 主文件不存在：直接创建主文件
    if (!SD.exists(CSV_PRIMARY_PATH)) {
        if (!createCsvWithHeader(String(CSV_PRIMARY_PATH), false)) {
            return false;
        }

        g_activeCsvPath = String(CSV_PRIMARY_PATH);
        csv_primary_header_ok = true;
        csv_logging_enabled = true;
        csv_using_fallback_file = false;

        Serial.println("SD: New primary data.csv created.");
        return true;
    }

    // 主文件存在且兼容：继续写主文件
    if (headerMatchesPath(String(CSV_PRIMARY_PATH))) {
        g_activeCsvPath = String(CSV_PRIMARY_PATH);
        csv_primary_header_ok = true;
        csv_logging_enabled = true;
        csv_using_fallback_file = false;
        g_preservedPrimaryPath = "";

        Serial.println("SD: Primary data.csv header OK, logging to data.csv.");
        return true;
    }

    // 主文件存在但不兼容：保留主文件，自动切换到新文件
    csv_primary_header_ok = false;
    csv_using_fallback_file = true;
    g_preservedPrimaryPath = String(CSV_PRIMARY_PATH);

    String fallbackPath = chooseFallbackCsvPath();
    if (!createCsvWithHeader(fallbackPath, false)) {
        Serial.println("SD WARNING: Primary data.csv header mismatch, and fallback file creation failed.");
        csv_logging_enabled = false;
        return false;
    }

    g_activeCsvPath = fallbackPath;
    csv_logging_enabled = true;

    Serial.println("SD WARNING: Primary data.csv header mismatch.");
    Serial.println("SD WARNING: Existing data.csv preserved untouched.");
    Serial.print("SD WARNING: Logging redirected to fallback file: ");
    Serial.println(g_activeCsvPath);

    return true;
}

// =========================
// SD 卡挂载与热插拔
// =========================
bool tryMountSD(bool allowFormatOnFail) {
    pinMode(IMU_CS, OUTPUT);
    digitalWrite(IMU_CS, HIGH);

    sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

    bool ok = SD.begin(SD_CS, sdSPI, 4000000, "/sd", 5, allowFormatOnFail);
    if (!ok) {
        sd_ok = false;
        csv_logging_enabled = false;
        return false;
    }

    sd_ok = configureCsvLoggingTargets();
    return sd_ok;
}

void initSD() {
    Serial.println("SD: Mounting...");
    if (tryMountSD(true)) {
        Serial.println("SD: Initialized.");
    } else {
        Serial.println("SD: Mount failed and format failed!");
    }
}

void refreshSDStatus() {
    if (millis() - lastSdRefreshMs < SD_REFRESH_MS) return;
    lastSdRefreshMs = millis();

    if (sd_ok) {
        File testFile = SD.open(g_activeCsvPath.c_str(), FILE_APPEND);
        if (testFile) {
            testFile.close();
        } else {
            Serial.println("SD: Active log file lost.");
            sd_ok = false;
            csv_logging_enabled = false;
            SD.end();
        }
    } else {
        Serial.println("SD: Re-mounting...");
        if (tryMountSD(false)) {
            Serial.println("SD: Re-mounted.");
        }
    }
}

bool logDataToSD(const String &csvLine) {
    if (!sd_ok) return false;
    if (!csv_logging_enabled) {
        Serial.println("SD: Logging paused.");
        return false;
    }

    File file = SD.open(g_activeCsvPath.c_str(), FILE_APPEND);
    if (file) {
        file.println(csvLine);
        file.close();
        return true;
    } else {
        Serial.print("SD: Failed to open active log file for append: ");
        Serial.println(g_activeCsvPath);
        sd_ok = false;
        csv_logging_enabled = false;
        SD.end();
        return false;
    }
}

void dumpFileToSerial(const String &path, const String &label) {
    if (!sd_ok) {
        Serial.println("SD: Offline, cannot dump file.");
        return;
    }

    if (!SD.exists(path.c_str())) {
        Serial.print("SD: File not found: ");
        Serial.println(path);
        return;
    }

    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) {
        Serial.print("SD: Failed to open ");
        Serial.println(path);
        return;
    }

    Serial.println();
    Serial.print("===== BEGIN ");
    Serial.print(label);
    Serial.println(" =====");
    while (file.available()) {
        Serial.write(file.read());
    }
    Serial.println();
    Serial.print("===== END ");
    Serial.print(label);
    Serial.println(" =====");
    Serial.println();

    file.close();
}

// =========================
// HTTP
// =========================
bool sendRemote(const String &jsonData) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("HTTP: Skipped, WiFi not connected.");
        lastHttpCode = -999;
        return false;
    }

    HTTPClient http;
    http.begin("http://httpbin.org/post");
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST(jsonData);
    lastHttpCode = httpResponseCode;

    Serial.print("HTTP code: ");
    Serial.println(httpResponseCode);

    bool ok = false;
    if (httpResponseCode > 0) {
        String payload = http.getString();
        Serial.print("HTTP response length: ");
        Serial.println(payload.length());
        ok = true;
    }

    http.end();
    return ok;
}

// =========================
// RTC
// =========================
static void trySyncRtcFromGps() {
    if (!rtc_ok || !gps.date.isValid() || !gps.time.isValid() || gps.date.year() < 2024) return;

    if (!rtc_synced || millis() - lastRtcSyncMs > 10UL * 60UL * 1000UL) {
        rtc.adjust(gpsToLocalDateTime());
        rtc_synced = true;
        lastRtcSyncMs = millis();
        Serial.println("RTC: Synced from GPS.");
    }
}

// =========================
// 统一实时采集
// =========================
void updateLiveData() {
    DateTime now = getNowTime();
    updateTimeText(now);

    if (bme_ok) {
        g_temp = bme.readTemperature();
        g_hum  = bme.readHumidity();
        g_pres = bme.readPressure() / 100.0F;
    } else {
        g_temp = NAN;
        g_hum  = NAN;
        g_pres = NAN;
    }

    if (uv_ok && millis() - lastUvReadMs >= UV_REFRESH_MS) {
        lastUvReadMs = millis();

        float ua, ub, uc;
        if (as7331.oneShot_uWcm2(&ua, &ub, &uc)) {
            g_uva = ua;
            g_uvb = ub;
            g_uvc = uc;
        } else {
            g_uva = NAN;
            g_uvb = NAN;
            g_uvc = NAN;
        }

        g_uvOverflow = as7331.hasOverflow();
    }

    g_gpsFix = gps.location.isValid();
    g_sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    g_alt = gps.altitude.isValid() ? gps.altitude.meters() : NAN;
    g_lat = g_gpsFix ? gps.location.lat() : NAN;
    g_lon = g_gpsFix ? gps.location.lng() : NAN;

    if (pmu_ok) {
        g_batt = constrain(PMU.getBatteryPercent(), 0, 100);
        g_battMv = PMU.getBattVoltage();
        g_charging = PMU.isCharging();
    } else {
        g_batt = 0;
        g_battMv = 0;
        g_charging = false;
    }

    if (WiFi.status() == WL_CONNECTED) {
        g_wifiRssi = WiFi.RSSI();
        g_wifiIp = WiFi.localIP().toString();
    } else {
        g_wifiRssi = 0;
        g_wifiIp = "0.0.0.0";
    }

    lastSensorUpdateMs = millis();
}

// =========================
// 历史页面支持
// =========================
struct HistoryRow {
    String ts;
    float temp;
    float hum;
    float batt;
    bool valid;
};

bool parseHistoryLine(const String &line, HistoryRow &row) {
    if (line.length() == 0) return false;
    if (line.startsWith("Date,Time,")) return false;

    String dateStr = csvField(line, 0);
    String timeStr = csvField(line, 1);
    String tempStr = csvField(line, 2);
    String humStr  = csvField(line, 3);
    String battStr = csvField(line, 14);

    if (dateStr.length() == 0 || timeStr.length() == 0 ||
        tempStr.length() == 0 || humStr.length() == 0 || battStr.length() == 0) {
        return false;
    }

    row.ts = dateStr + " " + timeStr;
    row.temp = tempStr.toFloat();
    row.hum = humStr.toFloat();
    row.batt = battStr.toFloat();
    row.valid = true;
    return true;
}

int loadRecentHistory(HistoryRow rows[], int maxRows, const String &path) {
    if (!sd_ok) return 0;
    if (!SD.exists(path.c_str())) return 0;

    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) return 0;

    int count = 0;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();

        HistoryRow r;
        if (!parseHistoryLine(line, r)) continue;

        if (count < maxRows) {
            rows[count++] = r;
        } else {
            for (int i = 0; i < maxRows - 1; i++) rows[i] = rows[i + 1];
            rows[maxRows - 1] = r;
        }
        delay(0);
    }

    file.close();
    return count;
}

String buildSparklineSVG(HistoryRow rows[], int count, char seriesType) {
    if (count <= 1) {
        return "<svg viewBox='0 0 320 120' width='100%' height='120'><text x='12' y='60' fill='#666'>Not enough data</text></svg>";
    }

    float values[HISTORY_ROWS];
    float minV = 1e9, maxV = -1e9;

    for (int i = 0; i < count; i++) {
        float v = NAN;
        if (seriesType == 'T') v = rows[i].temp;
        if (seriesType == 'H') v = rows[i].hum;
        if (seriesType == 'B') v = rows[i].batt;

        values[i] = v;
        if (!isnan(v)) {
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }
    }

    if (minV > maxV) {
        minV = 0;
        maxV = 1;
    }
    if (fabs(maxV - minV) < 0.001f) {
        maxV += 1.0f;
        minV -= 1.0f;
    }

    String points;
    const float left = 10.0f;
    const float top = 10.0f;
    const float chartW = 300.0f;
    const float chartH = 90.0f;

    for (int i = 0; i < count; i++) {
        float x = left + chartW * i / (float)(count - 1);
        float norm = (values[i] - minV) / (maxV - minV);
        float y = top + chartH * (1.0f - norm);

        points += String(x, 1) + "," + String(y, 1) + " ";
    }

    String title = "Temp";
    String color = "#d9480f";
    if (seriesType == 'H') { title = "Humidity"; color = "#1971c2"; }
    if (seriesType == 'B') { title = "Battery"; color = "#2b8a3e"; }

    String svg;
    svg += "<svg viewBox='0 0 320 120' width='100%' height='120' xmlns='http://www.w3.org/2000/svg'>";
    svg += "<rect x='0' y='0' width='320' height='120' fill='#fff'/>";
    svg += "<line x1='10' y1='100' x2='310' y2='100' stroke='#ddd'/>";
    svg += "<polyline fill='none' stroke='" + color + "' stroke-width='3' points='" + points + "'/>";
    svg += "<text x='12' y='16' fill='#333' font-size='12'>" + title + "</text>";
    svg += "<text x='12' y='114' fill='#666' font-size='11'>min=" + String(minV, 1) + " max=" + String(maxV, 1) + "</text>";
    svg += "</svg>";
    return svg;
}

// =========================
// Web 页面
// =========================
String htmlHeader(const String &title, bool autoRefresh = true) {
    String s;
    s += "<!doctype html><html><head><meta charset='utf-8'>";
    s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    if (autoRefresh) s += "<meta http-equiv='refresh' content='5'>";
    s += "<title>" + title + "</title>";
    s += "<style>";
    s += "body{font-family:Arial,Helvetica,sans-serif;background:#f5f7fb;color:#222;margin:0;padding:20px;}";
    s += ".wrap{max-width:1180px;margin:auto;}";
    s += ".card{background:#fff;border-radius:14px;padding:16px;margin-bottom:16px;box-shadow:0 2px 10px rgba(0,0,0,.08);}";
    s += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;}";
    s += ".big{font-size:24px;font-weight:700;}";
    s += ".muted{color:#666;font-size:13px;}";
    s += ".btn{display:inline-block;margin:6px 8px 6px 0;padding:10px 14px;background:#1f6feb;color:#fff;text-decoration:none;border-radius:10px;}";
    s += ".btn-danger{background:#c62828;}";
    s += ".btn-warn{background:#ef6c00;}";
    s += "pre{white-space:pre-wrap;word-break:break-word;background:#111;color:#eee;padding:12px;border-radius:10px;}";
    s += "table{width:100%;border-collapse:collapse;font-size:14px;}";
    s += "th,td{padding:8px;border-bottom:1px solid #eee;text-align:left;}";
    s += ".ok{color:#0a8f3d;font-weight:700;}.bad{color:#c62828;font-weight:700;}.warn{color:#ef6c00;font-weight:700;}";
    s += "</style></head><body><div class='wrap'>";
    return s;
}

String htmlFooter() {
    return "</div></body></html>";
}

void handleRoot() {
    String html = htmlHeader("GreenMind Local Panel");

    html += "<div class='card'>";
    html += "<div class='big'>GreenMind Local Panel</div>";
    html += "<div class='muted'>本地实时状态总览。若 data.csv 表头不兼容，将保留原文件并自动写入新文件。</div>";
    html += "</div>";

    if (csv_using_fallback_file) {
        html += "<div class='card'>";
        html += "<div class='warn'>WARNING: 主文件 data.csv 表头与当前版本不兼容。原文件已完整保留，当前正在写入新文件：<b>" + g_activeCsvPath + "</b></div>";
        html += "<div class='muted'>如需切回 data.csv，请明确授权执行“重建主文件并切回”。</div>";
        html += "</div>";
    }

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>日期</div><div class='big'>" + g_dateText + "</div></div>";
    html += "<div><div class='muted'>时间</div><div class='big'>" + g_timeText + "</div></div>";
    html += "<div><div class='muted'>时间戳</div><div class='big' style='font-size:18px'>" + g_timestamp + "</div></div>";
    html += "<div><div class='muted'>温度</div><div class='big'>" + (isnan(g_temp) ? String("--.-") : String(g_temp, 1)) + " ℃</div></div>";
    html += "<div><div class='muted'>湿度</div><div class='big'>" + (isnan(g_hum) ? String("--") : String(g_hum, 1)) + " %</div></div>";
    html += "<div><div class='muted'>气压</div><div class='big'>" + (isnan(g_pres) ? String("--") : String(g_pres, 1)) + " hPa</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>UVA</div><div class='big'>" + (isnan(g_uva) ? String("--.--") : String(g_uva, 2)) + " uW/cm²</div></div>";
    html += "<div><div class='muted'>UVB</div><div class='big'>" + (isnan(g_uvb) ? String("--.--") : String(g_uvb, 2)) + " uW/cm²</div></div>";
    html += "<div><div class='muted'>UVC</div><div class='big'>" + (isnan(g_uvc) ? String("--.--") : String(g_uvc, 2)) + " uW/cm²</div></div>";
    html += "<div><div class='muted'>UV Overflow</div><div class='big " + String(g_uvOverflow ? "bad" : "ok") + "'>" + yesNo(g_uvOverflow) + "</div></div>";
    html += "<div><div class='muted'>海拔</div><div class='big'>" + (isnan(g_alt) ? String("--") : String(g_alt, 2)) + " m</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>GPS Fix</div><div class='big'>" + yesNo(g_gpsFix) + "</div></div>";
    html += "<div><div class='muted'>卫星数</div><div class='big'>" + String(g_sats) + "</div></div>";
    html += "<div><div class='muted'>纬度</div><div class='big' style='font-size:18px'>" + (isnan(g_lat) ? String("nan") : String(g_lat, 6)) + " °</div></div>";
    html += "<div><div class='muted'>经度</div><div class='big' style='font-size:18px'>" + (isnan(g_lon) ? String("nan") : String(g_lon, 6)) + " °</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>电量</div><div class='big'>" + String(g_batt) + " %</div></div>";
    html += "<div><div class='muted'>电池电压</div><div class='big'>" + String(g_battMv) + " mV</div></div>";
    html += "<div><div class='muted'>充电状态</div><div class='big'>" + onOff(g_charging) + "</div></div>";
    html += "<div><div class='muted'>Wi-Fi RSSI</div><div class='big'>" + String(g_wifiRssi) + " dBm</div></div>";
    html += "<div><div class='muted'>IP 地址</div><div class='big' style='font-size:18px'>" + g_wifiIp + "</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>Uptime</div><div class='big' style='font-size:20px'>" + uptimeString() + "</div></div>";
    html += "<div><div class='muted'>Free Heap</div><div class='big'>" + String(ESP.getFreeHeap()) + "</div></div>";
    html += "<div><div class='muted'>RTC Synced</div><div class='big'>" + yesNo(rtc_synced) + "</div></div>";
    html += "<div><div class='muted'>Last Sample Age</div><div class='big'>" + String(lastSampleAgeSeconds()) + " s</div></div>";
    html += "<div><div class='muted'>Next Log In</div><div class='big'>" + String(nextLogInSeconds()) + " s</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>SD</div><div class='big " + String(sd_ok ? "ok" : "bad") + "'>" + String(sd_ok ? "ONLINE" : "OFFLINE") + "</div></div>";
    html += "<div><div class='muted'>Wi-Fi</div><div class='big " + String(WiFi.status() == WL_CONNECTED ? "ok" : "bad") + "'>" + String(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED") + "</div></div>";
    html += "<div><div class='muted'>Primary CSV Header</div><div class='big " + String(csv_primary_header_ok ? "ok" : "warn") + "'>" + String(csv_primary_header_ok ? "OK" : "MISMATCH") + "</div></div>";
    html += "<div><div class='muted'>Logging Enabled</div><div class='big " + String(csv_logging_enabled ? "ok" : "bad") + "'>" + yesNo(csv_logging_enabled) + "</div></div>";
    html += "<div><div class='muted'>Logging Mode</div><div class='big'>" + String(csv_using_fallback_file ? "FALLBACK" : "PRIMARY") + "</div></div>";
    html += "<div><div class='muted'>Active Log File</div><div class='big' style='font-size:18px'>" + g_activeCsvPath + "</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>最近一次 SD 写入</div><div class='big " + String(lastSdWriteOk ? "ok" : "bad") + "'>" + okBad(lastSdWriteOk) + "</div></div>";
    html += "<div><div class='muted'>最近一次 HTTP</div><div class='big " + String(lastHttpOk ? "ok" : "bad") + "'>" + okBad(lastHttpOk) + "</div><div class='muted'>code=" + String(lastHttpCode) + "</div></div>";
    html += "</div>";

    html += "<div class='card'>";
    html += "<a class='btn' href='/history'>查看历史趋势（活动文件）</a>";
    html += "<a class='btn' href='/view'>查看活动日志文件</a>";
    html += "<a class='btn' href='/download'>下载活动日志文件</a>";
    html += "<a class='btn' href='/view_primary'>查看主文件 data.csv</a>";
    html += "<a class='btn' href='/download_primary'>下载主文件 data.csv</a>";
    html += "<a class='btn' href='/status'>查看 JSON 状态</a>";
    html += "<a class='btn btn-danger' href='/erase_active' onclick=\"return confirm('确认清空当前活动日志文件吗？')\">清空当前活动日志文件</a>";
    html += "<a class='btn btn-warn' href='/rebuild_primary' onclick=\"return confirm('确认删除并重建主文件 data.csv，并切回主文件写入吗？')\">重建主文件并切回</a>";
    html += "</div>";

    html += htmlFooter();
    server.send(200, "text/html; charset=utf-8", html);
}

void handleStatusJson() {
    String json = "{";
    json += "\"date\":\"" + g_dateText + "\",";
    json += "\"time\":\"" + g_timeText + "\",";
    json += "\"timestamp\":\"" + g_timestamp + "\",";
    json += "\"temp_c\":" + String(g_temp, 2) + ",";
    json += "\"hum_pct\":" + String(g_hum, 2) + ",";
    json += "\"press_hpa\":" + String(g_pres, 2) + ",";
    json += "\"uva_uWcm2\":" + String(g_uva, 2) + ",";
    json += "\"uvb_uWcm2\":" + String(g_uvb, 2) + ",";
    json += "\"uvc_uWcm2\":" + String(g_uvc, 2) + ",";
    json += "\"uv_overflow\":" + String(g_uvOverflow ? "true" : "false") + ",";
    json += "\"alt_m\":" + String(g_alt, 2) + ",";
    json += "\"lat_deg\":" + String(g_lat, 6) + ",";
    json += "\"lon_deg\":" + String(g_lon, 6) + ",";
    json += "\"sat\":" + String(g_sats) + ",";
    json += "\"gps_fix\":" + String(g_gpsFix ? "true" : "false") + ",";
    json += "\"battery_pct\":" + String(g_batt) + ",";
    json += "\"battery_mv\":" + String(g_battMv) + ",";
    json += "\"charging\":" + String(g_charging ? "true" : "false") + ",";
    json += "\"wifi_rssi_dbm\":" + String(g_wifiRssi) + ",";
    json += "\"wifi_ip\":\"" + g_wifiIp + "\",";
    json += "\"sd_ok\":" + String(sd_ok ? "true" : "false") + ",";
    json += "\"wifi_ok\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    json += "\"last_sd_write_ok\":" + String(lastSdWriteOk ? "true" : "false") + ",";
    json += "\"last_http_ok\":" + String(lastHttpOk ? "true" : "false") + ",";
    json += "\"last_http_code\":" + String(lastHttpCode) + ",";
    json += "\"uptime_sec\":" + String(millis() / 1000) + ",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"rtc_synced\":" + String(rtc_synced ? "true" : "false") + ",";
    json += "\"next_log_in_sec\":" + String(nextLogInSeconds()) + ",";
    json += "\"last_sample_age_sec\":" + String(lastSampleAgeSeconds()) + ",";
    json += "\"primary_csv_header_ok\":" + String(csv_primary_header_ok ? "true" : "false") + ",";
    json += "\"csv_logging_enabled\":" + String(csv_logging_enabled ? "true" : "false") + ",";
    json += "\"csv_using_fallback_file\":" + String(csv_using_fallback_file ? "true" : "false") + ",";
    json += "\"active_csv_path\":\"" + g_activeCsvPath + "\",";
    json += "\"preserved_primary_path\":\"" + g_preservedPrimaryPath + "\"";
    json += "}";
    server.send(200, "application/json; charset=utf-8", json);
}

void handleHistory() {
    HistoryRow rows[HISTORY_ROWS];
    int count = loadRecentHistory(rows, HISTORY_ROWS, g_activeCsvPath);

    String html = htmlHeader("History", false);

    html += "<div class='card'>";
    html += "<div class='big'>History</div>";
    html += "<div class='muted'>趋势基于当前活动日志文件：<b>" + g_activeCsvPath + "</b></div>";
    html += "</div>";

    html += "<div class='card'><div class='muted'>Temperature Trend</div>";
    html += buildSparklineSVG(rows, count, 'T');
    html += "</div>";

    html += "<div class='card'><div class='muted'>Humidity Trend</div>";
    html += buildSparklineSVG(rows, count, 'H');
    html += "</div>";

    html += "<div class='card'><div class='muted'>Battery Trend</div>";
    html += buildSparklineSVG(rows, count, 'B');
    html += "</div>";

    html += "<div class='card'><div class='muted'>Recent " + String(count) + " Records</div><table>";
    html += "<tr><th>Date Time</th><th>Temp(℃)</th><th>Hum(%)</th><th>Batt(%)</th></tr>";

    for (int i = 0; i < count; i++) {
        html += "<tr>";
        html += "<td>" + rows[i].ts + "</td>";
        html += "<td>" + String(rows[i].temp, 2) + "</td>";
        html += "<td>" + String(rows[i].hum, 2) + "</td>";
        html += "<td>" + String(rows[i].batt, 0) + "</td>";
        html += "</tr>";
    }

    html += "</table></div>";
    html += "<div class='card'><a class='btn' href='/'>返回首页</a></div>";
    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}

// 通用查看文件页面
void streamCsvAsHtml(const String &path, const String &title) {
    if (!sd_ok) {
        server.send(503, "text/plain; charset=utf-8", "SD offline");
        return;
    }

    if (!SD.exists(path.c_str())) {
        server.send(404, "text/plain; charset=utf-8", "File not found");
        return;
    }

    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) {
        server.send(500, "text/plain; charset=utf-8", "Failed to open file");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    server.sendContent(htmlHeader(title, false));
    server.sendContent("<div class='card'><div class='big'>" + path + "</div><div class='muted'>原始内容预览</div></div><div class='card'><pre>");

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.replace("&", "&amp;");
        line.replace("<", "&lt;");
        line.replace(">", "&gt;");
        server.sendContent(line + "\n");
        delay(0);
    }

    server.sendContent("</pre></div>");
    server.sendContent(htmlFooter());
    file.close();
    server.sendContent("");
}

void downloadCsvFile(const String &path) {
    if (!sd_ok) {
        server.send(503, "text/plain; charset=utf-8", "SD offline");
        return;
    }

    if (!SD.exists(path.c_str())) {
        server.send(404, "text/plain; charset=utf-8", "File not found");
        return;
    }

    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) {
        server.send(500, "text/plain; charset=utf-8", "Failed to open file");
        return;
    }

    String name = baseNameFromPath(path);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    server.streamFile(file, "text/csv");
    file.close();
}

void handleViewActive() { streamCsvAsHtml(g_activeCsvPath, "View Active Log"); }
void handleDownloadActive() { downloadCsvFile(g_activeCsvPath); }
void handleViewPrimary() { streamCsvAsHtml(String(CSV_PRIMARY_PATH), "View Primary data.csv"); }
void handleDownloadPrimary() { downloadCsvFile(String(CSV_PRIMARY_PATH)); }

void handleEraseActive() {
    bool ok = eraseActiveCsvAuthorized();

    String html = htmlHeader("Erase Active Log", false);
    html += "<div class='card'><div class='big'>清空当前活动日志文件</div>";
    if (ok) {
        html += "<div class='ok'>已重建当前活动日志文件：<b>" + g_activeCsvPath + "</b></div>";
    } else {
        html += "<div class='bad'>操作失败，请检查 SD 状态。</div>";
    }
    html += "</div><div class='card'><a class='btn' href='/'>返回首页</a></div>";
    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}

void handleRebuildPrimary() {
    bool ok = rebuildPrimaryCsvAuthorized();

    String html = htmlHeader("Rebuild Primary CSV", false);
    html += "<div class='card'><div class='big'>重建主文件 data.csv</div>";
    if (ok) {
        html += "<div class='ok'>已重建主文件 <b>/data.csv</b>，并已切回主文件写入。</div>";
    } else {
        html += "<div class='bad'>操作失败，请检查 SD 状态。</div>";
    }
    html += "</div><div class='card'><a class='btn' href='/'>返回首页</a></div>";
    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}

void handleNotFound() {
    server.send(404, "text/plain; charset=utf-8", "Not found");
}

void initWebServer() {
    server.on("/", handleRoot);
    server.on("/status", handleStatusJson);
    server.on("/history", handleHistory);
    server.on("/view", handleViewActive);
    server.on("/download", handleDownloadActive);
    server.on("/view_primary", handleViewPrimary);
    server.on("/download_primary", handleDownloadPrimary);
    server.on("/erase_active", handleEraseActive);
    server.on("/rebuild_primary", handleRebuildPrimary);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("Web: Server started.");
}

// =========================
// OLED
// =========================
static void drawDashUI(DateTime &now) {
    u8g2.clearBuffer();

    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, 128, 15);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_6x10_tf);

    if (WiFi.status() == WL_CONNECTED) u8g2.drawStr(2, 11, "W");
    if (sd_ok) {
        if (csv_logging_enabled) u8g2.drawStr(12, 11, "SD");
        else u8g2.drawStr(12, 11, "SD!");
    }

    char tBuf[10];
    snprintf(tBuf, sizeof(tBuf), "%02d%s%02d",
             now.hour(),
             (millis() / 500) % 2 ? ":" : " ",
             now.minute());
    u8g2.drawStr(54 - (u8g2.getStrWidth(tBuf) / 2), 11, tBuf);

    char bBuf[8];
    snprintf(bBuf, sizeof(bBuf), "%d%%", g_batt);
    int bw = u8g2.getStrWidth(bBuf);
    u8g2.drawStr(126 - bw, 11, bBuf);

    int bx = 126 - bw - 17;
    u8g2.drawFrame(bx, 3, 14, 9);
    if (g_charging) {
        u8g2.drawLine(bx + 7, 5, bx + 5, 7);
        u8g2.drawLine(bx + 5, 7, bx + 8, 7);
        u8g2.drawLine(bx + 8, 7, bx + 6, 10);
    } else {
        int fill = map(constrain(g_batt, 0, 100), 0, 100, 0, 10);
        if (fill > 0) u8g2.drawBox(bx + 2, 5, fill, 5);
    }

    u8g2.setDrawColor(1);

    char tempBuf[10];
    if (isnan(g_temp)) snprintf(tempBuf, sizeof(tempBuf), "--.-");
    else snprintf(tempBuf, sizeof(tempBuf), "%.1f", g_temp);

    u8g2.setFont(u8g2_font_helvB18_tf);
    u8g2.setCursor(0, 42);
    u8g2.print(tempBuf);

    int tempWidth = u8g2.getStrWidth(tempBuf);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawCircle(tempWidth + 4, 30, 2);
    u8g2.setCursor(tempWidth + 8, 42);
    u8g2.print("C");

    int rX = 72;

    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.setCursor(rX, 28);
    u8g2.print("H:");
    if (isnan(g_hum)) u8g2.print("--");
    else u8g2.print((int)g_hum);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.print("%");

    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.setCursor(rX, 47);
    u8g2.print("P:");
    if (isnan(g_pres)) u8g2.print("---");
    else u8g2.print((int)g_pres);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.print("h");

    u8g2.drawHLine(0, 52, 128);
    u8g2.setFont(u8g2_font_5x8_tf);

    u8g2.setCursor(0, 62);
    u8g2.print("SAT:");
    if (g_sats < 10) u8g2.print("0");
    u8g2.print(g_sats);

    u8g2.drawVLine(38, 54, 10);

    if (g_gpsFix) {
        u8g2.setCursor(43, 62);
        u8g2.print(fabs(g_lat), 2);
        u8g2.print(g_lat >= 0 ? "N " : "S ");
        u8g2.print(fabs(g_lon), 2);
        u8g2.print(g_lon >= 0 ? "E" : "W");
    } else {
        u8g2.drawStr(43, 62, "WAITING GPS...");
    }

    u8g2.sendBuffer();
}

// =========================
// 串口命令
// =========================
void handleSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\n' || c == '\r') continue;

        switch (c) {
            case 'h':
            case 'H':
                printHelp();
                break;
            case 's':
            case 'S':
                printStatus();
                break;
            case 'd':
            case 'D':
                dumpFileToSerial(g_activeCsvPath, "ACTIVE LOG FILE");
                break;
            case 'p':
            case 'P':
                dumpFileToSerial(String(CSV_PRIMARY_PATH), "PRIMARY data.csv");
                break;
            case 'e':
            case 'E':
                if (eraseActiveCsvAuthorized()) {
                    Serial.print("SD: Active log file erased and recreated: ");
                    Serial.println(g_activeCsvPath);
                } else {
                    Serial.println("SD: Failed to erase active log file.");
                }
                break;
            case 'r':
            case 'R':
                if (rebuildPrimaryCsvAuthorized()) {
                    Serial.println("SD: Primary data.csv rebuilt and switched back.");
                } else {
                    Serial.println("SD: Failed to rebuild primary data.csv.");
                }
                break;
            default:
                Serial.print("Unknown command: ");
                Serial.println(c);
                printHelp();
                break;
        }
    }
}

// =========================
// Setup
// =========================
void setup() {
    Serial.begin(115200);

    PMUWire.begin(PMU_SDA, PMU_SCL);
    pmu_ok = PMU.begin(PMUWire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL);

    if (pmu_ok) {
        PMU.setALDO1Voltage(3300);
        PMU.enableALDO1();

        PMU.setALDO4Voltage(3300);
        PMU.enableALDO4();

        PMU.setBLDO1Voltage(3300);
        PMU.enableBLDO1();

        PMU.disableTSPinMeasure();
        PMU.enableBattVoltageMeasure();
    }

    pinMode(GPS_EN_PIN, OUTPUT);
    digitalWrite(GPS_EN_PIN, HIGH);
    Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    Wire.begin(I2C_SDA, I2C_SCL);

    rtc_ok = rtc.begin(&PMUWire);

    bme_ok = bme.begin(0x77, &Wire);
    if (!bme_ok) {
        bme_ok = bme.begin(0x76, &Wire);
    }

    if (as7331.begin(&Wire)) {
        uv_ok = true;
    }

    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(10, 35, "Agri-Node Started");
    u8g2.sendBuffer();

    initSD();
    initWiFi();
    initWebServer();
    printHelp();

    delay(1000);
}

// =========================
// Loop
// =========================
void loop() {
    handleSerialCommands();
    server.handleClient();

    while (Serial1.available() > 0) {
        gps.encode(Serial1.read());
    }

    ensureWiFiConnected();
    trySyncRtcFromGps();
    refreshSDStatus();

    updateLiveData();

    if (millis() - lastUiRefreshMs >= UI_REFRESH_MS) {
        lastUiRefreshMs = millis();
        DateTime now = getNowTime();
        drawDashUI(now);
    }

    if (millis() - lastLogMs >= LOG_INTERVAL_MS) {
        lastLogMs = millis();

        updateLiveData();

        DateTime now = getNowTime();

        char dateBuf[11];
        char timeBuf[9];
        snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
                 now.year(), now.month(), now.day());
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                 now.hour(), now.minute(), now.second());

        String csvLine =
            String(dateBuf) + "," +
            String(timeBuf) + "," +
            String(g_temp, 2) + "," +
            String(g_hum, 2) + "," +
            String(g_pres, 2) + "," +
            String(g_uva, 2) + "," +
            String(g_uvb, 2) + "," +
            String(g_uvc, 2) + "," +
            String(g_uvOverflow ? 1 : 0) + "," +
            String(g_alt, 2) + "," +
            String(g_lat, 6) + "," +
            String(g_lon, 6) + "," +
            String(g_sats) + "," +
            String(g_gpsFix ? 1 : 0) + "," +
            String(g_batt) + "," +
            String(g_battMv) + "," +
            String(g_charging ? 1 : 0) + "," +
            String(g_wifiRssi);

        lastSdWriteOk = logDataToSD(csvLine);

        String json = "{"
            "\"date\":\"" + String(dateBuf) + "\","
            "\"time\":\"" + String(timeBuf) + "\","
            "\"timestamp\":\"" + g_timestamp + "\","
            "\"temp_c\":" + String(g_temp, 2) + ","
            "\"hum_pct\":" + String(g_hum, 2) + ","
            "\"press_hpa\":" + String(g_pres, 2) + ","
            "\"uva_uWcm2\":" + String(g_uva, 2) + ","
            "\"uvb_uWcm2\":" + String(g_uvb, 2) + ","
            "\"uvc_uWcm2\":" + String(g_uvc, 2) + ","
            "\"uv_overflow\":" + String(g_uvOverflow ? "true" : "false") + ","
            "\"alt_m\":" + String(g_alt, 2) + ","
            "\"lat_deg\":" + String(g_lat, 6) + ","
            "\"lon_deg\":" + String(g_lon, 6) + ","
            "\"sat\":" + String(g_sats) + ","
            "\"gps_fix\":" + String(g_gpsFix ? "true" : "false") + ","
            "\"battery_pct\":" + String(g_batt) + ","
            "\"battery_mv\":" + String(g_battMv) + ","
            "\"charging\":" + String(g_charging ? "true" : "false") + ","
            "\"wifi_rssi_dbm\":" + String(g_wifiRssi) + ","
            "\"wifi_ip\":\"" + g_wifiIp + "\","
            "\"active_csv_path\":\"" + g_activeCsvPath + "\""
            "}";

        lastHttpOk = sendRemote(json);

        Serial.print("Data sync result -> SD: ");
        Serial.print(lastSdWriteOk ? "OK" : "FAIL");
        Serial.print(" | HTTP: ");
        Serial.println(lastHttpOk ? "OK" : "FAIL");
    }
}