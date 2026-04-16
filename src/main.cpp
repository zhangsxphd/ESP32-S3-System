#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <XPowersLib.h>
#include <TinyGPS++.h>
#include <Adafruit_BME280.h>
#include <RTClib.h>
#include <math.h>

// =========================
// T-Beam S3 Supreme SX1262
// =========================
#define I2C_SDA        17
#define I2C_SCL        18
#define PMU_SDA        42
#define PMU_SCL        41
#define GPS_RX_PIN     9
#define GPS_TX_PIN     8
#define GPS_EN_PIN     7

static const int TZ_OFFSET_MINUTES = 8 * 60;

// =========================
// 硬件对象
// =========================
TwoWire PMUWire = TwoWire(1);
XPowersAXP2101 PMU;
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
TinyGPSPlus gps;
Adafruit_BME280 bme;
RTC_PCF8563 rtc;

// =========================
// 状态变量
// =========================
bool pmu_ok = false;
bool bme_ok = false;
bool rtc_ok = false;
bool rtc_synced = false;

uint32_t lastRtcSyncMs = 0;
uint32_t lastUiRefreshMs = 0;

// =========================
// 时间逻辑
// =========================
static DateTime gpsToLocalDateTime() {
    DateTime utc(gps.date.year(), gps.date.month(), gps.date.day(),
                 gps.time.hour(), gps.time.minute(), gps.time.second());
    return utc + TimeSpan(TZ_OFFSET_MINUTES * 60);
}

static bool gpsDateTimeValid() {
    return gps.date.isValid() && gps.time.isValid() &&
           gps.date.year() >= 2024 && gps.date.year() <= 2099;
}

static void trySyncRtcFromGps() {
    if (!rtc_ok || !gpsDateTimeValid()) return;
    if (!rtc_synced || millis() - lastRtcSyncMs > 10UL * 60UL * 1000UL) {
        rtc.adjust(gpsToLocalDateTime());
        rtc_synced = true;
        lastRtcSyncMs = millis();
    }
}

static bool getDisplayTime(DateTime &out) {
    if (rtc_ok) {
        DateTime now = rtc.now();
        if (now.year() >= 2024 && now.year() <= 2099) {
            out = now;
            return true;
        }
    }
    if (gpsDateTimeValid()) {
        out = gpsToLocalDateTime();
        return true;
    }
    return false;
}

// =========================
// UI 绘制：三分布局 + 闪电充电
// =========================
static void drawMinimalistUI(float temp, float hum, double alt, int sats, bool gpsFix, int battPercent, bool charging, bool hasBattery, DateTime &now, bool hasTime) {
    u8g2.clearBuffer();

    // ------------------------------------------------
    // 1. 顶部状态栏 (三分天下布局)
    // ------------------------------------------------
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, 128, 15); 
    u8g2.setDrawColor(0);        
    u8g2.setFont(u8g2_font_6x10_tf);
    
    if (hasTime) {
        // [左侧]: 纯日期
        char dBuf[10];
        snprintf(dBuf, sizeof(dBuf), "%d/%d", now.month(), now.day());
        u8g2.drawStr(2, 11, dBuf);

        // [中间]: 纯时间 (居中显示，冒号闪烁，稍微左移8个像素保持平衡)
        char tBuf[10];
        bool blink = (millis() / 500) % 2 == 0; 
        snprintf(tBuf, sizeof(tBuf), "%02d%s%02d", now.hour(), blink ? ":" : " ", now.minute());
        int tw = u8g2.getStrWidth(tBuf);
        int centerLeftX = 54; 
        u8g2.drawStr(centerLeftX - (tw / 2), 11, tBuf); 
    } else {
        u8g2.drawStr(2, 11, "--/--");
        int tw = u8g2.getStrWidth("--:--");
        u8g2.drawStr(54 - (tw / 2), 11, "--:--"); 
    }

    // [右侧]: 电量与电池图标
    char bBuf[8];
    if (hasBattery) snprintf(bBuf, sizeof(bBuf), "%d%%", battPercent);
    else snprintf(bBuf, sizeof(bBuf), "USB");
    
    int bw = u8g2.getStrWidth(bBuf);
    int startX = 126 - bw; 
    
    u8g2.drawStr(startX, 11, bBuf); 
    
    int bx = startX - 17; 
    int by = 3; 
    u8g2.drawFrame(bx, by, 14, 9);       // 电池外壳
    u8g2.drawBox(bx + 14, by + 2, 2, 5); // 电池正极触头
    
    if (hasBattery) {
        if (charging) {
            // ⚡ 极客微操：用三条线绘制一个完美的像素闪电
            u8g2.drawLine(bx + 7, by + 2, bx + 5, by + 4); // 闪电上半部 (左斜下)
            u8g2.drawLine(bx + 5, by + 4, bx + 8, by + 4); // 闪电中间转折 (水平向右)
            u8g2.drawLine(bx + 8, by + 4, bx + 6, by + 7); // 闪电下半部 (左斜下，穿透到底)
        } else {
            // 放电时：绘制剩余电量柱
            int fill = map(battPercent, 0, 100, 0, 10);
            if (fill > 0) u8g2.drawBox(bx + 2, by + 2, fill, 5); 
        }
    } else {
        u8g2.drawLine(bx + 2, by + 2, bx + 11, by + 6); // 无电池划斜杠
    }

    u8g2.setDrawColor(1); // 恢复白笔

    // ------------------------------------------------
    // 2. 中间数据区 
    // ------------------------------------------------
    // --- 温度 ---
    u8g2.setFont(u8g2_font_helvB18_tf); 
    u8g2.setCursor(0, 42);
    String tStr = isnan(temp) ? "--.-" : String(temp, 1);
    u8g2.print(tStr);
    
    int tNumWidth = u8g2.getStrWidth(tStr.c_str());
    
    u8g2.setFont(u8g2_font_6x10_tf); 
    u8g2.drawCircle(tNumWidth + 4, 30, 2); // 完美的度数小圆圈
    u8g2.setCursor(tNumWidth + 8, 42);     // C 跟随其后
    u8g2.print("C");

    // --- 湿度 ---
    int rightX = 70; 
    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.setCursor(rightX, 30);  
    u8g2.print("H: ");
    String hStr = isnan(hum) ? "--" : String((int)hum);
    u8g2.print(hStr);
    
    int hw = u8g2.getStrWidth(("H: " + hStr).c_str());
    
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(rightX + hw + 2, 30); 
    u8g2.print("%");

    // --- 海拔 ---
    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.setCursor(rightX, 48);  
    u8g2.print("A: ");
    String aStr = isnan(alt) ? "---" : String((int)alt);
    u8g2.print(aStr);
    
    int aw = u8g2.getStrWidth(("A: " + aStr).c_str());
    
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(rightX + aw + 2, 48); 
    u8g2.print("m");

    // ------------------------------------------------
    // 3. 黄金分割线
    // ------------------------------------------------
    u8g2.drawHLine(0, 52, 128); 

    // ------------------------------------------------
    // 4. 底栏 (Y: 53~64)
    // ------------------------------------------------
    u8g2.setFont(u8g2_font_5x8_tf);
    
    u8g2.setCursor(0, 62);
    u8g2.print("SAT:"); 
    if (sats < 10) u8g2.print("0"); 
    u8g2.print(sats);

    u8g2.drawVLine(38, 54, 10); 

    if (gpsFix) {
        double lat = gps.location.lat();
        double lng = gps.location.lng();
        
        u8g2.setCursor(43, 62); 
        u8g2.print(abs(lat), 2); 
        u8g2.print(lat >= 0 ? "N " : "S ");
        
        u8g2.print(abs(lng), 2); 
        u8g2.print(lng >= 0 ? "E" : "W");
    } else {
        u8g2.drawStr(43, 62, "WAITING FOR FIX");
    }

    u8g2.sendBuffer();
}

// =========================
// 初始化与主循环
// =========================
static void initPMU() {
    PMUWire.begin(PMU_SDA, PMU_SCL);
    pmu_ok = PMU.begin(PMUWire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL);
    if (pmu_ok) {
        PMU.setALDO1Voltage(3300); PMU.enableALDO1();
        PMU.setALDO2Voltage(3300); PMU.enableALDO2();
        PMU.setALDO4Voltage(3300); PMU.enableALDO4();
        PMU.disableTSPinMeasure(); 
        PMU.enableBattVoltageMeasure();
        delay(80);
    }
}

static void initGPS() {
    pinMode(GPS_EN_PIN, OUTPUT);
    digitalWrite(GPS_EN_PIN, LOW); delay(50);
    digitalWrite(GPS_EN_PIN, HIGH);
    Serial1.setRxBufferSize(1024);
    Serial1.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

static void initBME() {
    Wire.begin(I2C_SDA, I2C_SCL);
    bme_ok = bme.begin(0x77, &Wire);
    if (!bme_ok) bme_ok = bme.begin(0x76, &Wire);
}

static void initRTC() {
    rtc_ok = rtc.begin(&PMUWire);
}

void setup() {
    Serial.begin(115200);
    delay(120);

    initPMU(); initGPS(); initBME(); initRTC();

    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB10_tr);
    u8g2.drawStr(10, 35, "Agri-Node Started");
    u8g2.sendBuffer();
    delay(1000);
}

void loop() {
    while (Serial1.available() > 0) {
        gps.encode(Serial1.read());
    }

    trySyncRtcFromGps();

    if (millis() - lastUiRefreshMs < 500) return;
    lastUiRefreshMs = millis();

    float temp = NAN, hum = NAN;
    if (bme_ok) {
        temp = bme.readTemperature();
        hum  = bme.readHumidity();
    }

    bool hasBattery = pmu_ok && PMU.isBatteryConnect();
    bool charging = hasBattery ? PMU.isCharging() : false;
    int battPercent = hasBattery ? constrain(PMU.getBatteryPercent(), 0, 100) : 0;

    bool gpsFix = gps.location.isValid();
    int sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    double altitudeM = gps.altitude.isValid() ? gps.altitude.meters() : NAN;

    DateTime now(2000, 1, 1, 0, 0, 0);
    bool hasTime = getDisplayTime(now);

    drawMinimalistUI(temp, hum, altitudeM, sats, gpsFix, battPercent, charging, hasBattery, now, hasTime);
}