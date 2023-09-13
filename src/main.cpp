#include <M5EPD.h>
#include <WiFi.h>
#include "my_settings.h"

//#define MY_SCREEN_WIDTH 960
//#define MY_SCREEN_HEIGHT 540
#define MY_SCREEN_WIDTH 540
#define MY_SCREEN_HEIGHT 960


// resolution of device is 960x540 (landscape format), no rotation
M5EPD_Canvas timeCanvas(&M5.EPD);
M5EPD_Canvas batteryCanvas(&M5.EPD);
M5EPD_Canvas imageCanvas(&M5.EPD);

char timeStrbuff[64];
char batteryStrbuff[64];
bool wokenByRTC;

void readRtc(rtc_date_t& rtcDate, rtc_time_t& rtcTime) {
    M5.RTC.getDate(&rtcDate);
    M5.RTC.getTime(&rtcTime);
}

void readBattery(uint32_t& batteryVoltage, uint32_t& batteryPercentage) {
    batteryVoltage = M5.getBatteryVoltage();
    float fBatteryPercent = (float)(batteryVoltage - 3300) / (float)(4350 - 3300);
    if (fBatteryPercent <= 0.01) {
        fBatteryPercent = 0.01;
    }
    if (fBatteryPercent > 1) {
        fBatteryPercent = 1;
    }
    batteryPercentage = (uint32_t)(fBatteryPercent * 100);
}

void flushTime() {
    rtc_date_t rtcDate; rtc_time_t rtcTime;
    readRtc(rtcDate, rtcTime);
    //sprintf(timeStrbuff, "%d/%02d/%02d %02d:%02d:%02d", rtcDate.year,
    //        rtcDate.mon, rtcDate.day, rtcTime.hour, rtcTime.min, rtcTime.sec);
    sprintf(timeStrbuff, "%02d/%02d %02d:%02d:%02d",
            rtcDate.mon, rtcDate.day, rtcTime.hour, rtcTime.min, rtcTime.sec);

    timeCanvas.drawString(timeStrbuff, 0, 0);
    timeCanvas.pushCanvas(
        MY_SCREEN_WIDTH - timeCanvas.width() - 10,
        MY_SCREEN_HEIGHT - timeCanvas.height() - 10,
        UPDATE_MODE_DU4);
}

void flushBattery() {
    uint32_t batteryVoltage, batteryPercentage;
    readBattery(batteryVoltage, batteryPercentage);
    // trailing spaces to overwrite any previous artifacts of 1% < 10% < 100%
    sprintf(batteryStrbuff, "%dmV (%d%%)  ", batteryVoltage, batteryPercentage);
    batteryCanvas.drawString(batteryStrbuff, 0, 0);
    batteryCanvas.pushCanvas(10, MY_SCREEN_HEIGHT - batteryCanvas.height() - 10, UPDATE_MODE_DU4);
}

void setupTime() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    rtc_date_t rtcDate; rtc_time_t rtcTime;
    readRtc(rtcDate, rtcTime);
    bool syncRequired = false;

    if (rtcDate.year < 2022) {
        ESP_LOGI("setupTime", "RTC Year is < 2022, requiring sync.");
        syncRequired = true;
    } else if ((rtcTime.hour == 0 && rtcTime.min == 0 && rtcTime.sec < 30) ||
        (rtcTime.hour == 23 && rtcTime.min == 59 && rtcTime.sec > 30)) {
        ESP_LOGI("setupTime", "Triggering daily NTP sync based on time constraints.");
        syncRequired = true;
    }
    if (!syncRequired) {
        ESP_LOGI("setupTime", "RTC seems to have been properly initialized, skipping NTP check.");
        return;
    }

    ESP_LOGI("setupTime", "Calling configTime(), waiting 5 seconds for response...");
    configTime(0, 0, "time1.google.com", "time2.google.com");

    delay(5000); // give NTP some time

    time_t t = time(NULL);
    struct tm* tm = localtime(&t);

    rtcTime.hour = tm->tm_hour;
    rtcTime.min = tm->tm_min;
    rtcTime.sec = tm->tm_sec;
    M5.RTC.setTime(&rtcTime);

    rtcDate.year = tm->tm_year + 1900;
    rtcDate.mon = tm->tm_mon + 1;
    rtcDate.day = tm->tm_mday;
    M5.RTC.setDate(&rtcDate);
}

String buildUrl() {
    rtc_date_t rtcDate; rtc_time_t  rtcTime;
    readRtc(rtcDate, rtcTime);
    uint32_t batteryVoltage, batteryPercentage;
    readBattery(batteryVoltage, batteryPercentage);

    String strVoltage, strBatteryPercent;
    strVoltage.concat(batteryVoltage);
    strBatteryPercent.concat(batteryPercentage);

    char rtcdatetimeBuf[64];
    sprintf(rtcdatetimeBuf, "%04d-%02d-%02dT%02d:%02d:%02d",
            rtcDate.year, rtcDate.mon, rtcDate.day, rtcTime.hour, rtcTime.min, rtcTime.sec);

    String url(MY_URL_TEMPLATE);
    url.replace("{mac}", WiFi.macAddress());
    url.replace("{voltage}", strVoltage);
    url.replace("{batterypercent}", strBatteryPercent);
    url.replace("{rtcdatetime}", rtcdatetimeBuf);
    url.replace("{width}", String(MY_SCREEN_WIDTH));
    url.replace("{height}", String(MY_SCREEN_HEIGHT));
    url.replace("{wokenByRTC}", wokenByRTC ? "true" : "false");
    return url;
}

void setup() {
    // check if wakeup due to RTC -> https://community.m5stack.com/post/21806
    // check power on reason before calling M5.begin()
    // which calls RTC.begin() which clears the timer flag.
    Wire.begin(21, 22);
    uint8_t reason = M5.RTC.readReg(0x01);
    wokenByRTC = (reason & 0b0000101) == 0b0000101;

    M5.begin(false, false, false, true, true);
    M5.EPD.SetRotation(90);
    M5.EPD.Clear(false); // false, since background image is written with highest quality first
    // init RTC, clear all pending alarms
    M5.RTC.begin();
    M5.RTC.setAlarmIRQ(-1);
    M5.RTC.setAlarmIRQ(RTC_Date(-1, -1, -1, -1), RTC_Time(-1, -1, -1)); // see https://github.com/m5stack/M5EPD/issues/26 why we need to use this version

    WiFi.begin(MY_WIFI_SSID, MY_WIFI_PASSWORD);
    int retry = 10; // try up to 5 seconds
    while (WiFi.status() != WL_CONNECTED && retry-- > 0) {
        delay(500);
        Serial.print(".");
    }

    setupTime(); // sync NTP time if required and connected

    if (WiFi.status() == WL_CONNECTED) {    
        String url = buildUrl();
        ESP_LOGV("setup", "Fetching image from %s", url.c_str());
        imageCanvas.createCanvas(MY_SCREEN_WIDTH, MY_SCREEN_HEIGHT);
        imageCanvas.drawJpgUrl(url);
        M5.EPD.Clear(true);
        imageCanvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    } else {
        M5.EPD.Clear(true);
    }

    WiFi.disconnect(true, true);

    timeCanvas.createCanvas(260, 35);
    timeCanvas.setTextFont(1);
    timeCanvas.setTextSize(3);

    batteryCanvas.createCanvas(230, 35);
    batteryCanvas.setTextFont(1);
    batteryCanvas.setTextSize(3);
}

int waitTimeToNextWakeupInSeconds() {
    rtc_time_t rtcTime;
    rtc_date_t rtcDate;
    M5.RTC.getTime(&rtcTime);
    M5.RTC.getDate(&rtcDate);

    int waitTime = 60 - rtcTime.sec; // to next full minute
    waitTime += (15 - (rtcTime.min + 1) % 15) * 60; // to next full quarter hour
    return waitTime;
}

void rtcSleepByWaitTime(int waitTimeSeconds) {
    if (waitTimeSeconds > 255 * 60) {
        ESP_LOGW("loop", "RTC does only support up 255 minutes, strange things are going to happen since you exceed the 255 minutes!");
    } else if (waitTimeSeconds > 255) {
        ESP_LOGW("loop", "RTC does only support up 255 seconds, going to fallback to minute resolution!");
    }

    if (waitTimeSeconds < 0) {
        ESP_LOGW("loop", "waitTime is %d", waitTimeSeconds);
    } else if (waitTimeSeconds > 10) {
        ESP_LOGI("loop", "waitTime is %d, going to shutdown", waitTimeSeconds);
        // give the screen one second to finish what it is doing
        delay(1000);
        M5.shutdown(waitTimeSeconds - 1);
        ESP_LOGI("loop", "shutdown fallthrough to deep sleep", waitTimeSeconds);
        esp_deep_sleep((waitTimeSeconds - 1) * (uint64_t)1000000);
        ESP_LOGI("loop", "shutdown fallthrough to delay", waitTimeSeconds);
    }
    ESP_LOGI("loop", "waitTime is %d, going wait via delay", waitTimeSeconds);
    delay(waitTimeSeconds * 1000);
}

void rtcSleepByWakeupTime() {
    rtc_time_t rtcTime;
    M5.RTC.getTime(&rtcTime);

    if (rtcTime.sec >= 55) {
        // in the last 5 seconds of a minute we make sure that we already pretend
        // it's the next minute to avoid race conditions so that RTC already expired
        rtcTime.min++;
    }

    rtcTime.sec = 0;
    rtcTime.min = (rtcTime.min / 15 + 1) * 15; // next full 15 min
    while (rtcTime.min >= 60) {
        rtcTime.hour++;
        rtcTime.min -= 60;
    }
    rtcTime.hour = rtcTime.hour % 24;

    ESP_LOGI("loop", "rtcWakeUp on %02d:%02d:%02d", rtcTime.hour, rtcTime.min, rtcTime.sec);
    // give the screen one second to finish what it is doing
    delay(1000);
    // see https://github.com/m5stack/M5EPD/issues/26 why we need to use this version
    M5.shutdown(RTC_Date(-1, -1, -1, -1), rtcTime);
    // during development shutdown does not work, need to deep-sleep
    ESP_LOGW("loop", "---------------------");
    ESP_LOGW("loop", "Fallback to rtcSleepByWaitTime");
    rtcSleepByWaitTime(waitTimeToNextWakeupInSeconds());
}

void loop() {
    ESP_LOGD("loop", "Looping...");

    flushTime();
    flushBattery();

    // shut down and go to sleep. need to re-read time since the above could have taken some time
    int waitTimeSeconds = waitTimeToNextWakeupInSeconds();
    if (waitTimeSeconds < 255) {
        // second precision is only available for 255 seconds,
        // afterwards fallback to up to 255 minutes
        rtcSleepByWaitTime(waitTimeSeconds);
    } else {
        // wake up by time
        // why not always? because there are nasty race conditions if
        // if the wait time is too short
        rtcSleepByWakeupTime();
    }
}
