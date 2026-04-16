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

#define IMU_CS         34   // 让板载 IMU 失能，避免 SPI 总线冲突

const char* ssid = "ChinaNet-CD5F";
const char* password = "12345678";

static const int TZ_OFFSET_MINUTES = 8 * 60;   // 东八区

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

uint32_t lastUiRefreshMs = 0;
uint32_t lastLogMs = 0;
uint32_t lastRtcSyncMs = 0;
uint32_t lastSdRefreshMs = 0;
uint32_t lastUvReadMs = 0;

bool lastSdWriteOk = false;
bool lastHttpOk = false;
int lastHttpCode = 0;
bool webUrlPrinted = false;

// =========================
// 最近一次缓存数据（网页 / JSON / OLED / CSV 共用）
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
String g_timestamp = "2026-01-01T00:00:00";

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

static void updateTimeText(const DateTime &now) {
    char timeBuf[8];
    char dateBuf[8];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", now.hour(), now.minute());
    snprintf(dateBuf, sizeof(dateBuf), "%d/%d", now.month(), now.day());

    g_timeText = String(timeBuf);
    g_dateText = String(dateBuf);
    g_timestamp = String(now.timestamp());
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

static String yesNo(bool v) {
    return v ? "YES" : "NO";
}

static String onOff(bool v) {
    return v ? "ON" : "OFF";
}

static void printHelp() {
    Serial.println();
    Serial.println("===== SERIAL COMMANDS =====");
    Serial.println("h  -> help");
    Serial.println("s  -> show status");
    Serial.println("d  -> dump /data.csv");
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

    Serial.print("BME280: ");
    Serial.println(bme_ok ? "OK" : "FAIL");

    Serial.print("UV(AS7331): ");
    Serial.println(uv_ok ? "OK" : "FAIL");

    Serial.print("GPS Fix: ");
    Serial.println(gps.location.isValid() ? "YES" : "NO");

    Serial.print("Satellites: ");
    if (gps.satellites.isValid()) Serial.println(gps.satellites.value());
    else Serial.println(0);

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
// SD 卡
// =========================
bool ensureCSVHeader() {
    File file = SD.open("/data.csv", FILE_APPEND);
    if (!file) {
        Serial.println("SD: Failed to open /data.csv while checking header.");
        return false;
    }

    if (file.size() == 0) {
        file.println("Time,TempC,HumPct,PresshPa,UVA,UVB,UVC,AltM,Lat,Lon,Sats,GpsFix,BattPct,BattmV,Charging,WiFiRSSI");
    }

    file.close();
    return true;
}

bool tryMountSD(bool allowFormatOnFail) {
    pinMode(IMU_CS, OUTPUT);
    digitalWrite(IMU_CS, HIGH);

    sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

    bool ok = SD.begin(SD_CS, sdSPI, 4000000, "/sd", 5, allowFormatOnFail);
    if (!ok) {
        sd_ok = false;
        return false;
    }

    sd_ok = ensureCSVHeader();
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
    if (millis() - lastSdRefreshMs < 3000) return;
    lastSdRefreshMs = millis();

    if (sd_ok) {
        File testFile = SD.open("/data.csv", FILE_APPEND);
        if (testFile) {
            testFile.close();
        } else {
            Serial.println("SD: Lost.");
            sd_ok = false;
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

    File file = SD.open("/data.csv", FILE_APPEND);
    if (file) {
        file.println(csvLine);
        file.close();
        return true;
    } else {
        Serial.println("SD: Failed to open /data.csv for append.");
        sd_ok = false;
        SD.end();
        return false;
    }
}

void dumpSDToSerial() {
    if (!sd_ok) {
        Serial.println("SD: Offline, cannot dump file.");
        return;
    }

    File file = SD.open("/data.csv", FILE_READ);
    if (!file) {
        Serial.println("SD: Failed to open /data.csv for reading.");
        sd_ok = false;
        SD.end();
        return;
    }

    Serial.println();
    Serial.println("===== BEGIN /data.csv =====");
    while (file.available()) {
        Serial.write(file.read());
    }
    Serial.println();
    Serial.println("===== END /data.csv =====");
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
// Web 页面
// =========================
String htmlHeader(const String &title) {
    String s;
    s += "<!doctype html><html><head><meta charset='utf-8'>";
    s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    s += "<meta http-equiv='refresh' content='5'>";
    s += "<title>" + title + "</title>";
    s += "<style>";
    s += "body{font-family:Arial,Helvetica,sans-serif;background:#f5f7fb;color:#222;margin:0;padding:20px;}";
    s += ".wrap{max-width:1100px;margin:auto;}";
    s += ".card{background:#fff;border-radius:14px;padding:16px;margin-bottom:16px;box-shadow:0 2px 10px rgba(0,0,0,.08);}";
    s += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;}";
    s += ".big{font-size:24px;font-weight:700;}";
    s += ".muted{color:#666;font-size:13px;}";
    s += ".btn{display:inline-block;margin:6px 8px 6px 0;padding:10px 14px;background:#1f6feb;color:#fff;text-decoration:none;border-radius:10px;}";
    s += "pre{white-space:pre-wrap;word-break:break-word;background:#111;color:#eee;padding:12px;border-radius:10px;}";
    s += ".ok{color:#0a8f3d;font-weight:700;}.bad{color:#c62828;font-weight:700;}";
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
    html += "<div class='muted'>本地查看当前设备数据、状态与 SD 数据下载</div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>时间</div><div class='big'>" + g_timeText + "</div><div>" + g_dateText + "</div></div>";
    html += "<div><div class='muted'>时间戳</div><div class='big' style='font-size:18px'>" + g_timestamp + "</div></div>";
    html += "<div><div class='muted'>温度</div><div class='big'>" + (isnan(g_temp) ? String("--.-") : String(g_temp, 1)) + " C</div></div>";
    html += "<div><div class='muted'>湿度</div><div class='big'>" + (isnan(g_hum) ? String("--") : String(g_hum, 1)) + " %</div></div>";
    html += "<div><div class='muted'>气压</div><div class='big'>" + (isnan(g_pres) ? String("--") : String(g_pres, 1)) + " hPa</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>UVA</div><div class='big'>" + (isnan(g_uva) ? String("--.--") : String(g_uva, 2)) + "</div></div>";
    html += "<div><div class='muted'>UVB</div><div class='big'>" + (isnan(g_uvb) ? String("--.--") : String(g_uvb, 2)) + "</div></div>";
    html += "<div><div class='muted'>UVC</div><div class='big'>" + (isnan(g_uvc) ? String("--.--") : String(g_uvc, 2)) + "</div></div>";
    html += "<div><div class='muted'>海拔</div><div class='big'>" + (isnan(g_alt) ? String("--") : String(g_alt, 2)) + " m</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>GPS Fix</div><div class='big'>" + yesNo(g_gpsFix) + "</div></div>";
    html += "<div><div class='muted'>卫星数</div><div class='big'>" + String(g_sats) + "</div></div>";
    html += "<div><div class='muted'>纬度</div><div class='big' style='font-size:18px'>" + (isnan(g_lat) ? String("nan") : String(g_lat, 6)) + "</div></div>";
    html += "<div><div class='muted'>经度</div><div class='big' style='font-size:18px'>" + (isnan(g_lon) ? String("nan") : String(g_lon, 6)) + "</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>电量</div><div class='big'>" + String(g_batt) + "%</div></div>";
    html += "<div><div class='muted'>电池电压</div><div class='big'>" + String(g_battMv) + " mV</div></div>";
    html += "<div><div class='muted'>充电状态</div><div class='big'>" + onOff(g_charging) + "</div></div>";
    html += "<div><div class='muted'>Wi-Fi RSSI</div><div class='big'>" + String(g_wifiRssi) + " dBm</div></div>";
    html += "<div><div class='muted'>IP 地址</div><div class='big' style='font-size:18px'>" + g_wifiIp + "</div></div>";
    html += "</div>";

    html += "<div class='card grid'>";
    html += "<div><div class='muted'>SD</div><div class='big " + String(sd_ok ? "ok" : "bad") + "'>" + String(sd_ok ? "ONLINE" : "OFFLINE") + "</div></div>";
    html += "<div><div class='muted'>Wi-Fi</div><div class='big " + String(WiFi.status() == WL_CONNECTED ? "ok" : "bad") + "'>" + String(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED") + "</div></div>";
    html += "<div><div class='muted'>最近一次 SD 写入</div><div class='big " + String(lastSdWriteOk ? "ok" : "bad") + "'>" + String(lastSdWriteOk ? "OK" : "FAIL") + "</div></div>";
    html += "<div><div class='muted'>最近一次 HTTP</div><div class='big " + String(lastHttpOk ? "ok" : "bad") + "'>" + String(lastHttpOk ? "OK" : "FAIL") + "</div><div class='muted'>code=" + String(lastHttpCode) + "</div></div>";
    html += "</div>";

    html += "<div class='card'>";
    html += "<a class='btn' href='/view'>查看 data.csv</a>";
    html += "<a class='btn' href='/download'>下载 data.csv</a>";
    html += "<a class='btn' href='/status'>查看 JSON 状态</a>";
    html += "</div>";

    html += htmlFooter();
    server.send(200, "text/html; charset=utf-8", html);
}

void handleStatusJson() {
    String json = "{";
    json += "\"timestamp\":\"" + g_timestamp + "\",";
    json += "\"time\":\"" + g_timeText + "\",";
    json += "\"date\":\"" + g_dateText + "\",";
    json += "\"temp\":" + String(g_temp, 2) + ",";
    json += "\"hum\":" + String(g_hum, 2) + ",";
    json += "\"pres\":" + String(g_pres, 2) + ",";
    json += "\"uva\":" + String(g_uva, 2) + ",";
    json += "\"uvb\":" + String(g_uvb, 2) + ",";
    json += "\"uvc\":" + String(g_uvc, 2) + ",";
    json += "\"alt\":" + String(g_alt, 2) + ",";
    json += "\"lat\":" + String(g_lat, 6) + ",";
    json += "\"lon\":" + String(g_lon, 6) + ",";
    json += "\"sat\":" + String(g_sats) + ",";
    json += "\"gps_fix\":" + String(g_gpsFix ? "true" : "false") + ",";
    json += "\"battery_pct\":" + String(g_batt) + ",";
    json += "\"battery_mv\":" + String(g_battMv) + ",";
    json += "\"charging\":" + String(g_charging ? "true" : "false") + ",";
    json += "\"wifi_rssi\":" + String(g_wifiRssi) + ",";
    json += "\"wifi_ip\":\"" + g_wifiIp + "\",";
    json += "\"sd_ok\":" + String(sd_ok ? "true" : "false") + ",";
    json += "\"wifi_ok\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    json += "\"last_sd_write_ok\":" + String(lastSdWriteOk ? "true" : "false") + ",";
    json += "\"last_http_ok\":" + String(lastHttpOk ? "true" : "false") + ",";
    json += "\"last_http_code\":" + String(lastHttpCode);
    json += "}";
    server.send(200, "application/json; charset=utf-8", json);
}

void handleViewCSV() {
    if (!sd_ok) {
        server.send(503, "text/plain; charset=utf-8", "SD offline");
        return;
    }

    File file = SD.open("/data.csv", FILE_READ);
    if (!file) {
        sd_ok = false;
        SD.end();
        server.send(500, "text/plain; charset=utf-8", "Failed to open /data.csv");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    server.sendContent(htmlHeader("View data.csv"));
    server.sendContent("<div class='card'><div class='big'>/data.csv</div><div class='muted'>原始内容预览</div></div><div class='card'><pre>");

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

void handleDownloadCSV() {
    if (!sd_ok) {
        server.send(503, "text/plain; charset=utf-8", "SD offline");
        return;
    }

    File file = SD.open("/data.csv", FILE_READ);
    if (!file) {
        sd_ok = false;
        SD.end();
        server.send(500, "text/plain; charset=utf-8", "Failed to open /data.csv");
        return;
    }

    server.sendHeader("Content-Disposition", "attachment; filename=\"data.csv\"");
    server.streamFile(file, "text/csv");
    file.close();
}

void handleNotFound() {
    server.send(404, "text/plain; charset=utf-8", "Not found");
}

void initWebServer() {
    server.on("/", handleRoot);
    server.on("/status", handleStatusJson);
    server.on("/view", handleViewCSV);
    server.on("/download", handleDownloadCSV);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("Web: Server started.");
}

// =========================
// 统一采集函数
// =========================
void updateLiveData() {
    DateTime now = getNowTime();
    updateTimeText(now);

    // BME280
    if (bme_ok) {
        g_temp = bme.readTemperature();
        g_hum  = bme.readHumidity();
        g_pres = bme.readPressure() / 100.0F;
    } else {
        g_temp = NAN;
        g_hum  = NAN;
        g_pres = NAN;
    }

    // UV：每 2 秒采一次，不要太密
    if (uv_ok && millis() - lastUvReadMs >= 2000) {
        lastUvReadMs = millis();

        uint16_t rawA, rawB, rawC;
        if (as7331.readAllUV(&rawA, &rawB, &rawC)) {
            g_uva = as7331.readUVA();
            g_uvb = as7331.readUVB();
            g_uvc = as7331.readUVC();
        } else {
            g_uva = NAN;
            g_uvb = NAN;
            g_uvc = NAN;
        }
    }

    // GPS
    g_gpsFix = gps.location.isValid();
    g_sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    g_alt = gps.altitude.isValid() ? gps.altitude.meters() : NAN;
    g_lat = g_gpsFix ? gps.location.lat() : NAN;
    g_lon = g_gpsFix ? gps.location.lng() : NAN;

    // PMU
    if (pmu_ok) {
        g_batt = constrain(PMU.getBatteryPercent(), 0, 100);
        g_battMv = PMU.getBattVoltage();
        g_charging = PMU.isCharging();
    } else {
        g_batt = 0;
        g_battMv = 0;
        g_charging = false;
    }

    // Wi-Fi
    if (WiFi.status() == WL_CONNECTED) {
        g_wifiRssi = WiFi.RSSI();
        g_wifiIp = WiFi.localIP().toString();
    } else {
        g_wifiRssi = 0;
        g_wifiIp = "0.0.0.0";
    }
}

// =========================
// OLED
// 说明：OLED 只显示核心信息，避免过载
// 全量数据请看网页 / JSON / CSV
// =========================
static void drawDashUI(DateTime &now) {
    u8g2.clearBuffer();

    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, 128, 15);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_6x10_tf);

    if (WiFi.status() == WL_CONNECTED) u8g2.drawStr(2, 11, "W");
    if (sd_ok) u8g2.drawStr(12, 11, "SD");

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
                dumpSDToSerial();
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

    // 实时更新缓存数据（网页 / OLED 用）
    updateLiveData();

    // OLED 继续保持较高刷新率
    if (millis() - lastUiRefreshMs >= 500) {
        lastUiRefreshMs = millis();
        DateTime now = getNowTime();
        drawDashUI(now);
    }

    // 本地存储与远程发送改成每分钟一次
    if (millis() - lastLogMs >= 60000) {
        lastLogMs = millis();

        // 再更新一次，确保写入的是最新值
        updateLiveData();

        String csvLine =
            g_timestamp + "," +
            String(g_temp, 2) + "," +
            String(g_hum, 2) + "," +
            String(g_pres, 2) + "," +
            String(g_uva, 2) + "," +
            String(g_uvb, 2) + "," +
            String(g_uvc, 2) + "," +
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
            "\"timestamp\":\"" + g_timestamp + "\","
            "\"temp\":" + String(g_temp, 2) + ","
            "\"hum\":" + String(g_hum, 2) + ","
            "\"pres\":" + String(g_pres, 2) + ","
            "\"uva\":" + String(g_uva, 2) + ","
            "\"uvb\":" + String(g_uvb, 2) + ","
            "\"uvc\":" + String(g_uvc, 2) + ","
            "\"alt\":" + String(g_alt, 2) + ","
            "\"lat\":" + String(g_lat, 6) + ","
            "\"lon\":" + String(g_lon, 6) + ","
            "\"sat\":" + String(g_sats) + ","
            "\"gps_fix\":" + String(g_gpsFix ? "true" : "false") + ","
            "\"battery_pct\":" + String(g_batt) + ","
            "\"battery_mv\":" + String(g_battMv) + ","
            "\"charging\":" + String(g_charging ? "true" : "false") + ","
            "\"wifi_rssi\":" + String(g_wifiRssi) + ","
            "\"wifi_ip\":\"" + g_wifiIp + "\""
            "}";

        lastHttpOk = sendRemote(json);

        Serial.print("Data sync result -> SD: ");
        Serial.print(lastSdWriteOk ? "OK" : "FAIL");
        Serial.print(" | HTTP: ");
        Serial.println(lastHttpOk ? "OK" : "FAIL");
    }
}