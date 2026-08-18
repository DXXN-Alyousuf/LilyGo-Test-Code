/*
 * LilyGo T5 4.7" ePaper (ESP32-S3, NON-TOUCH variant) — component test / alarm clock demo
 * Arduino IDE version — identical logic to the PlatformIO sketch in src/main.cpp.
 * See ../../README.md for full setup instructions (Boards Manager, Library
 * Manager, and exact Tools menu settings for this board).
 *
 * Exercises, on this one board, in this order:
 *   - PSRAM                (required by the EPD driver's framebuffer)
 *   - 960x540 e-paper panel (epd_driver: full-screen draws + text rendering)
 *   - I2C RTC (PCF8563)    (time keeping + hardware alarm)
 *   - microSD card         (SPI, optional — card may be absent)
 *   - battery ADC          (fuel-gauge-less voltage read)
 *   - WiFi radio           (MAC address only — no AP/credentials needed)
 *   - BUTTON_1 / GPIO21    (OPTIONAL — see note below)
 *
 * No physical button required.
 * ------------------------------
 * This board exposes one user button on GPIO21 (BUTTON_1 in utilities.h),
 * but you said your unit doesn't have one fitted/wired. The demo does NOT
 * depend on it: a test alarm is scheduled automatically a couple of minutes
 * after boot and re-schedules itself after every dismissal, so you can watch
 * the full alarm-clock behaviour (RTC alarm fires -> screen changes -> auto
 * dismiss -> reschedule) with zero input. If you ever wire a button to
 * GPIO21 + GND, it becomes a "dismiss now" / "trigger test alarm now" button
 * for free.
 *
 * Touch is intentionally not used (non-touch panel has no GT911 controller).
 *
 * E-paper note: the panel is only redrawn once per minute (plus on alarm
 * state changes). E-ink full/greyscale refreshes take the better part of a
 * second and the panels aren't rated for continuous per-second updates, so
 * a "ticking seconds hand" is not how real e-paper clocks behave. Watch the
 * serial monitor (115200 baud) if you want live per-second confirmation
 * that the RTC is actually running.
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_adc_cal.h>
#include <RtcDrv.hpp>          // SensorLib: SensorPCF8563 + RTC_DateTime
#include <Button2.h>

#include "epd_driver.h"
#include "firasans.h"
#include "utilities.h"          // board pin map (BUTTON_1, BATT_PIN, SD_*, BOARD_SDA/SCL, ...)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
#define ALARM_TEST_MINUTES   2      // minutes between each automatic test alarm
#define ALARM_DISPLAY_MS     20000  // how long the ALARM screen stays up before auto-dismiss
#define DIAG_HOLD_MS         6000   // how long the boot diagnostics screen stays up

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
uint8_t *framebuffer = NULL;

SensorPCF8563 rtc;
Button2 btn;

enum AppState { STATE_CLOCK, STATE_ALARM };
AppState state = STATE_CLOCK;

bool rtcOnline = false;
bool sdOnline = false;
double sdCardGB = 0.0;
uint32_t vref = 1100;

int lastMinuteDrawn = -1;
uint32_t alarmShownAt = 0;
volatile bool dismissRequested = false;
volatile bool manualAlarmRequested = false;
uint8_t nextAlarmMinute = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static void drawLine(int32_t x, int32_t y, const char *text)
{
    int32_t cx = x, cy = y;
    writeln((GFXfont *)&FiraSans, text, &cx, &cy, framebuffer);
}

static void clearFramebuffer()
{
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
}

static void pushFullScreen()
{
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
}

// Battery voltage via ADC2 (BATT_PIN sits on ADC2 on the S3). LilyGo boards
// typically put a 2:1 resistor divider ahead of the ADC pin — the x2 below
// assumes that; check with a multimeter once if you need lab-grade accuracy.
static float readBatteryVoltage()
{
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_2,
#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(2, 0, 17)
        ADC_ATTEN_DB_11,
#else
        ADC_ATTEN_DB_12,
#endif
        ADC_WIDTH_BIT_12,
        1100,
        &adc_chars);

    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        vref = adc_chars.vref;
    }

    uint16_t raw = analogRead(BATT_PIN);
    uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
    return (mv * 2.0f) / 1000.0f;
}

// Parses the compiler's __DATE__ / __TIME__ strings ("Mon DD YYYY", "HH:MM:SS")
// so a dead/first-power RTC can be seeded without needing WiFi/NTP.
static void seedRtcFromBuildTime()
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char monStr[4] = {0};
    int day, year, hour, minute, second;
    sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);

    uint8_t month = 1;
    for (uint8_t i = 0; i < 12; i++) {
        if (strncmp(monStr, months[i], 3) == 0) {
            month = i + 1;
            break;
        }
    }

    rtc.setDateTime((uint16_t)year, month, (uint8_t)day,
                     (uint8_t)hour, (uint8_t)minute, (uint8_t)second);
    Serial.println("[RTC] Integrity not guaranteed -> seeded from build timestamp");
}

static void scheduleNextAlarm(uint8_t minutesFromNow)
{
    RTC_DateTime now = rtc.getDateTime();
    nextAlarmMinute = (now.getMinute() + minutesFromNow) % 60;
    rtc.setAlarmByMinutes(nextAlarmMinute);
    rtc.enableAlarm();
    Serial.printf("[RTC] Next test alarm armed for minute :%02u\n", nextAlarmMinute);
}

static void onButtonPressed(Button2 &b)
{
    if (state == STATE_ALARM) {
        dismissRequested = true;
    } else {
        manualAlarmRequested = true;
    }
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
static void renderDiagnostics()
{
    clearFramebuffer();
    drawLine(30, 60, "LilyGo T5 4.7\" EPD47-S3 -- Component Test");
    epd_draw_hline(30, 75, EPD_WIDTH - 60, 0x00, framebuffer);

    int32_t y = 130;
    char buf[160];

    size_t psram = ESP.getPsramSize();
    snprintf(buf, sizeof(buf), "[%s] PSRAM       : %u KB", psram ? "OK" : "--", (unsigned)(psram / 1024));
    drawLine(30, y, buf); y += 45;

    snprintf(buf, sizeof(buf), "[%s] EPD panel   : 960x540 framebuffer allocated", framebuffer ? "OK" : "--");
    drawLine(30, y, buf); y += 45;

    snprintf(buf, sizeof(buf), "[%s] RTC PCF8563 : %s (I2C SDA=%d SCL=%d)",
             rtcOnline ? "OK" : "--", rtcOnline ? "detected" : "NOT FOUND", BOARD_SDA, BOARD_SCL);
    drawLine(30, y, buf); y += 45;

    if (sdOnline) {
        snprintf(buf, sizeof(buf), "[OK] microSD     : %.2f GB", sdCardGB);
    } else {
        snprintf(buf, sizeof(buf), "[--] microSD     : not detected (card optional)");
    }
    drawLine(30, y, buf); y += 45;

    float vbat = readBatteryVoltage();
    snprintf(buf, sizeof(buf), "[%s] Battery ADC : %.2f V (raw, GPIO%d)", vbat > 0.5f ? "OK" : "--", vbat, BATT_PIN);
    drawLine(30, y, buf); y += 45;

    String mac = WiFi.macAddress();
    snprintf(buf, sizeof(buf), "[OK] WiFi radio  : MAC %s", mac.c_str());
    drawLine(30, y, buf); y += 45;

    snprintf(buf, sizeof(buf), "[--] Button      : GPIO%d wired but optional (none fitted per user)", BUTTON_1);
    drawLine(30, y, buf); y += 60;

    drawLine(30, y, "Starting alarm-clock demo...");

    pushFullScreen();
}

static void renderClockScreen(const RTC_DateTime &now)
{
    clearFramebuffer();

    char buf[64];
    snprintf(buf, sizeof(buf), "%02u:%02u", now.getHour(), now.getMinute());
    {
        int32_t cx = 300, cy = 260;
        FontProperties props = { .fg_color = 0, .bg_color = 15, .fallback_glyph = 0, .flags = 0 };
        write_mode((GFXfont *)&FiraSans, buf, &cx, &cy, framebuffer, BLACK_ON_WHITE, &props);
    }

    snprintf(buf, sizeof(buf), "%04u-%02u-%02u  (updates once/min)", now.getYear(), now.getMonth(), now.getDay());
    drawLine(300, 310, buf);

    epd_draw_hline(30, 420, EPD_WIDTH - 60, 0x00, framebuffer);

    snprintf(buf, sizeof(buf), "RTC: %s   SD: %s   Batt: %.2fV   Next test alarm at :%02u",
             rtcOnline ? "ok" : "fail",
             sdOnline ? "ok" : "none",
             readBatteryVoltage(),
             nextAlarmMinute);
    drawLine(30, 460, buf);

    drawLine(30, 500, "No button required -- alarm fires and clears itself automatically.");

    pushFullScreen();
}

static void renderAlarmScreen(const RTC_DateTime &now)
{
    clearFramebuffer();

    char buf[64];
    {
        int32_t cx = 260, cy = 220;
        FontProperties props = { .fg_color = 15, .bg_color = 0, .fallback_glyph = 0, .flags = 0 };
        epd_fill_rect(0, 120, EPD_WIDTH, 160, 0x00, framebuffer);
        write_mode((GFXfont *)&FiraSans, "ALARM !", &cx, &cy, framebuffer, WHITE_ON_BLACK, &props);
    }

    snprintf(buf, sizeof(buf), "Fired at %02u:%02u:%02u", now.getHour(), now.getMinute(), now.getSecond());
    drawLine(300, 340, buf);

    drawLine(200, 420, "Auto-dismisses in ~20s (press GPIO21 button if fitted)");

    pushFullScreen();
    Serial.println("[ALARM] Fired.");
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== LilyGo T5 4.7 EPD47-S3 component test ===");

#ifndef BOARD_HAS_PSRAM
#error "Enable PSRAM: Tools -> PSRAM -> OPI PSRAM"
#endif

    // --- EPD panel + framebuffer ---
    framebuffer = (uint8_t *)ps_calloc(1, EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("FATAL: framebuffer allocation failed (PSRAM missing?)");
        while (1) delay(1000);
    }
    clearFramebuffer();
    epd_init();

    // --- RTC (PCF8563) over I2C ---
    Wire.begin(BOARD_SDA, BOARD_SCL);
    rtcOnline = rtc.begin(Wire, BOARD_SDA, BOARD_SCL);
    if (rtcOnline) {
        if (!rtc.isClockIntegrityGuaranteed()) {
            seedRtcFromBuildTime();
        }
        scheduleNextAlarm(ALARM_TEST_MINUTES);
    } else {
        Serial.println("[RTC] PCF8563 not found on I2C bus!");
    }

    // --- microSD (optional) ---
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    sdOnline = SD.begin(SD_CS, SPI);
    if (sdOnline) {
        sdCardGB = SD.cardSize() / 1024.0 / 1024.0 / 1024.0;
        Serial.printf("[SD] Card detected: %.2f GB\n", sdCardGB);
    } else {
        Serial.println("[SD] No card detected (this is fine if none is inserted)");
    }

    // --- WiFi radio smoke test (MAC only, no AP needed) ---
    WiFi.mode(WIFI_STA);
    Serial.printf("[WiFi] MAC address: %s\n", WiFi.macAddress().c_str());

    // --- optional button on GPIO21 ---
    btn.begin(BUTTON_1);
    btn.setPressedHandler(onButtonPressed);

    renderDiagnostics();
    delay(DIAG_HOLD_MS);
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop()
{
    btn.loop();

    if (!rtcOnline) {
        // Nothing more we can safely demo without a working RTC.
        delay(1000);
        return;
    }

    RTC_DateTime now = rtc.getDateTime();

    if (state == STATE_ALARM) {
        if (dismissRequested || (millis() - alarmShownAt > ALARM_DISPLAY_MS)) {
            dismissRequested = false;
            rtc.resetAlarm();
            state = STATE_CLOCK;
            lastMinuteDrawn = -1; // force redraw
            scheduleNextAlarm(ALARM_TEST_MINUTES);
        }
    } else {
        if (manualAlarmRequested) {
            manualAlarmRequested = false;
            state = STATE_ALARM;
            alarmShownAt = millis();
            renderAlarmScreen(now);
        } else if (rtc.isAlarmActive()) {
            state = STATE_ALARM;
            alarmShownAt = millis();
            renderAlarmScreen(now);
        } else if ((int)now.getMinute() != lastMinuteDrawn) {
            lastMinuteDrawn = now.getMinute();
            renderClockScreen(now);
        }
    }

    // Cheap heartbeat so you can confirm the RTC is ticking without
    // needing a per-second screen refresh.
    Serial.printf("[RTC] %04u-%02u-%02u %02u:%02u:%02u\n",
                   now.getYear(), now.getMonth(), now.getDay(),
                   now.getHour(), now.getMinute(), now.getSecond());

    delay(1000);
}
