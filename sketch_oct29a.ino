#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <esp_task_wdt.h>

// =========================================================
// 1. ตั้งค่า API Key และ Telegram Bot
// =========================================================
String twelveApiKey     = "5aa0d0ac06d842d099ba083d23bf4d3f";
String telegramBotToken = "9231927794:AAEhpT6FQT0zD888knqgmCd2orBHcIKXI7g";
String telegramChatId   = "7776247288";

// ตั้งค่าเริ่มต้น 0.0 ให้ตรงกับ GOLD# ใน MT5 (ปรับแก้ผ่านเว็บ 192.168.4.1 ได้)
float brokerOffset = 0.0;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

WebServer server(80);
Preferences preferences;

String savedSSID = "";
String savedPass = "";
bool enableCuteFace = false;
bool isSetupMode = false;
bool isOfflineMode = false;

unsigned long lastModeSwitch = 0;
bool showCuteFace = false;
const unsigned long clockDuration = 45000;
const unsigned long faceDuration  = 2000;
int faceEmotion = 0;
unsigned long lastDisplayKeepAlive = 0;

unsigned long toastUntil = 0;
String toastMsg = "";

// โครงสร้างข้อมูลกราฟ M15 (หาจุดเข้า) และ M30 (ดูเทรนด์หลัก)
#define MAX_M15_BARS 60
#define MAX_M30_BARS 210
float m15Close[MAX_M15_BARS], m15High[MAX_M15_BARS], m15Low[MAX_M15_BARS];
float m30Close[MAX_M30_BARS];

float liveGoldPrice = 0.0;
float lastRawPaxg   = 0.0;
float autoSpreadOffset = 0.0;
bool hasOffsetSynced   = false;

bool isMarketOpen   = false;
bool isNewsTime     = false;
unsigned long spikePauseUntil = 0;

String m30Trend     = "WAIT";
String sigSide      = "WAIT";
float sigEntry = 0.0, sigSL = 0.0, sigTP1 = 0.0, sigTP2 = 0.0, sigTP3 = 0.0, sigTP4 = 0.0;
bool tp1HitNotified = false;
bool slHitNotified  = false;

String sigTimeStr = "--:--";
unsigned long sigCreatedMillis = 0;
const unsigned long signalMaxAgeMs = 2700000UL; // อายุสัญญาณ 45 นาที (3 แท่ง M15)

unsigned long lastFastTick = 0;
const unsigned long fastTickInterval = 1000UL;     // วิ่งราคาสดทุก 1 วินาที

unsigned long lastM15Calc = 0;
const unsigned long m15CalcInterval = 900000UL;    // อัปเดตแท่ง M15 ทุก 15 นาที

unsigned long lastM30Calc = 0;
const unsigned long m30CalcInterval = 1800000UL;   // อัปเดตเทรนด์ M30 ทุก 30 นาที

unsigned long alertPopupStart = 0;
bool isAlertPopupActive = false;
const unsigned long alertPopupDuration = 10000;

WiFiClientSecure tickClient;
HTTPClient tickHttp;

// =========================================================
// ฟังก์ชันส่งข้อความเข้า Telegram โดยตรงจาก ESP32
// =========================================================
void sendTelegramMessage(String text) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure tgClient;
  tgClient.setInsecure();
  tgClient.setTimeout(5);
  HTTPClient http;
  http.setTimeout(5000);

  String url = "https://api.telegram.org/bot" + telegramBotToken + "/sendMessage";
  if (http.begin(tgClient, url)) {
    http.addHeader("Content-Type", "application/json");
    text.replace("\n", "\\n");
    String jsonBody = "{\"chat_id\":\"" + telegramChatId + "\",\"text\":\"" + text + "\",\"parse_mode\":\"HTML\"}";
    http.POST(jsonBody);
    http.end();
  }
}

bool checkXauMarketOpen(struct tm &timeinfo) {
  int wday = timeinfo.tm_wday;
  int hour = timeinfo.tm_hour;
  if (wday == 0) return false;
  if (wday == 6 && hour >= 4) return false;
  if (wday == 1 && hour < 5) return false;
  if (wday >= 2 && wday <= 5 && hour == 4) return false;
  return true;
}

bool checkNewsWindow(struct tm &timeinfo) {
  int wday = timeinfo.tm_wday;
  int minsOfDay = (timeinfo.tm_hour * 60) + timeinfo.tm_min;

  if (wday >= 2 && wday <= 5) {
    if (minsOfDay >= (19 * 60 + 15) && minsOfDay <= (21 * 60 + 30)) return true;
  }
  if (wday == 4 || wday == 5) {
    if (minsOfDay >= (0 * 60 + 45) && minsOfDay <= (2 * 60 + 15)) return true;
  }
  if (millis() < spikePauseUntil) return true;
  return false;
}

float parseJsonFloat(String &str, String key, int startPos, int &nextPos) {
  int k = str.indexOf("\"" + key + "\":\"", startPos);
  if (k == -1) {
    nextPos = -1;
    return 0.0;
  }
  int valStart = k + key.length() + 4;
  int valEnd = str.indexOf("\"", valStart);
  nextPos = valEnd;
  return str.substring(valStart, valEnd).toFloat();
}

void fetchRealTimeTick1Sec() {
  if (WiFi.status() != WL_CONNECTED || !isMarketOpen) return;

  tickClient.setInsecure();
  tickClient.setTimeout(2);
  tickHttp.setTimeout(2000);
  tickHttp.setReuse(true);

  if (tickHttp.begin(tickClient, "https://api.binance.com/api/v3/ticker/price?symbol=PAXGUSDT")) {
    if (tickHttp.GET() == 200) {
      String payload = tickHttp.getString();
      int dummy = 0;
      float rawPaxg = parseJsonFloat(payload, "price", 0, dummy);
      if (rawPaxg > 100.0) {
        lastRawPaxg = rawPaxg;
        if (hasOffsetSynced) {
          liveGoldPrice = rawPaxg + autoSpreadOffset + brokerOffset;
        } else if (liveGoldPrice < 100.0) {
          liveGoldPrice = rawPaxg + brokerOffset;
        }
      }
    }
    tickHttp.end();
  }
}

// วิเคราะห์เทรนด์ใหญ่บนไทม์เฟรม M30 (EMA 50 / EMA 200)
void updateM30TrendTwelve() {
  if (WiFi.status() != WL_CONNECTED || !isMarketOpen) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(6);
  HTTPClient http;
  http.setTimeout(6000);

  String url = "https://api.twelvedata.com/time_series?symbol=XAU/USD&interval=30min&outputsize=" + String(MAX_M30_BARS) + "&apikey=" + twelveApiKey;
  if (http.begin(client, url)) {
    if (http.GET() == 200) {
      String payload = http.getString();
      float tempClose[MAX_M30_BARS];
      int pos = 0, count = 0;
      while (count < MAX_M30_BARS) {
        int nextP = -1;
        float c = parseJsonFloat(payload, "close", pos, nextP);
        if (nextP == -1) break;
        tempClose[count++] = c + brokerOffset;
        pos = nextP + 1;
      }

      if (count >= 200) {
        for (int i = 0; i < count; i++) {
          m30Close[i] = tempClose[count - 1 - i];
        }
        float k50 = 2.0 / 51.0;
        float k200 = 2.0 / 201.0;
        float ema50 = m30Close[0];
        float ema200 = m30Close[0];

        for (int i = 1; i < count; i++) {
          ema50 = (m30Close[i] - ema50) * k50 + ema50;
          ema200 = (m30Close[i] - ema200) * k200 + ema200;
        }

        float currentM30 = m30Close[count - 1];
        if (currentM30 > ema50 && ema50 > ema200) m30Trend = "UP";
        else if (currentM30 < ema50 && ema50 < ema200) m30Trend = "DN";
        else m30Trend = "WAIT";
      }
    }
    http.end();
  }
}

// วิเคราะห์จุดย่อตัวเข้าซื้อ/ขายบนไทม์เฟรม M15
void analyzeM15Pullback(int barCount) {
  if (barCount < 35) return;

  int lastClosed = barCount - 2;

  float atrSum = 0;
  for (int i = lastClosed - 13; i <= lastClosed; i++) {
    float tr1 = m15High[i] - m15Low[i];
    float tr2 = abs(m15High[i] - m15Close[i - 1]);
    float tr3 = abs(m15Low[i] - m15Close[i - 1]);
    atrSum += max(tr1, max(tr2, tr3));
  }
  float atrVal = atrSum / 14.0;
  if (atrVal < 1.0) atrVal = 2.5;

  float lastBarRange = m15High[lastClosed] - m15Low[lastClosed];
  float liveBarRange = m15High[barCount - 1] - m15Low[barCount - 1];
  if (lastBarRange > (atrVal * 2.2) || liveBarRange > (atrVal * 2.2)) {
    spikePauseUntil = millis() + 1800000UL;
    isNewsTime = true;
    return;
  }

  if (isNewsTime) return;

  float kFast = 2.0 / 15.0;
  float kSlow = 2.0 / 29.0;
  float emaFast = m15Close[0], emaSlow = m15Close[0];

  for (int i = 1; i < barCount; i++) {
    emaFast = (m15Close[i] - emaFast) * kFast + emaFast;
    emaSlow = (m15Close[i] - emaSlow) * kSlow + emaSlow;
  }

  float rsiArr[MAX_M15_BARS];
  for (int i = 0; i < barCount; i++) {
    if (i < 2) {
      rsiArr[i] = 50.0;
    } else {
      float gain = 0, loss = 0;
      for (int j = i - 1; j <= i; j++) {
        float diff = m15Close[j] - m15Close[j - 1];
        if (diff > 0) gain += diff;
        else loss -= diff;
      }
      if (loss == 0) rsiArr[i] = 100.0;
      else if (gain == 0) rsiArr[i] = 0.0;
      else {
        float rs = (gain / 2.0) / (loss / 2.0);
        rsiArr[i] = 100.0 - (100.0 / (1.0 + rs));
      }
    }
  }

  float envBasisNow = 0, envBasisPrev = 0;
  for (int k = 0; k < 14; k++) {
    envBasisNow += rsiArr[lastClosed - k];
    envBasisPrev += rsiArr[lastClosed - 1 - k];
  }
  envBasisNow /= 14.0;
  envBasisPrev /= 14.0;

  bool pullbackBuyZone = false;
  bool pullbackSellZone = false;
  for (int b = 0; b < 6; b++) {
    int idx = lastClosed - b;
    float basis = 0;
    for (int k = 0; k < 14; k++) basis += rsiArr[idx - k];
    basis /= 14.0;
    if ((basis * 0.85) <= 32.0 || rsiArr[idx] <= 20.0) pullbackBuyZone = true;
    if ((basis * 1.15) >= 72.0 || rsiArr[idx] >= 80.0) pullbackSellZone = true;
  }

  bool m15BuyTurn  = (envBasisNow > envBasisPrev) && (emaFast > emaSlow) && (m15Close[lastClosed] > emaFast);
  bool m15SellTurn = (envBasisNow < envBasisPrev) && (emaFast < emaSlow) && (m15Close[lastClosed] < emaFast);

  float entry = (liveGoldPrice > 100.0) ? liveGoldPrice : m15Close[lastClosed];

  struct tm timeinfo;
  char timeBuf[10] = "--:--";
  if (getLocalTime(&timeinfo)) {
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }

  if (m30Trend == "UP" && pullbackBuyZone && m15BuyTurn && sigSide != "BUY") {
    sigSide  = "BUY";
    sigEntry = entry;
    sigSL    = entry - (atrVal * 1.5);
    sigTP1   = entry + (atrVal * 1.0);
    sigTP2   = entry + (atrVal * 1.8);
    sigTP3   = entry + (atrVal * 2.6);
    sigTP4   = entry + (atrVal * 3.5);
    sigTimeStr = String(timeBuf);
    sigCreatedMillis = millis();
    tp1HitNotified = false;
    slHitNotified  = false;
    isAlertPopupActive = true;
    alertPopupStart = millis();

    char msg[320];
    snprintf(msg, sizeof(msg),
      "🟢 <b>GOLD# BUY SIGNAL (@%s)</b>\n"
      "━━━━━━━━━━━━━━\n"
      "🎯 <b>Entry:</b> %.2f\n"
      "🛑 <b>SL:</b> %.2f\n"
      "✅ <b>TP1:</b> %.2f\n"
      "✅ <b>TP2:</b> %.2f\n"
      "✅ <b>TP3:</b> %.2f\n"
      "✅ <b>TP4:</b> %.2f\n"
      "━━━━━━━━━━━━━━\n"
      "📊 M30 Trend: UP | M15 Pullback",
      sigTimeStr.c_str(), sigEntry, sigSL, sigTP1, sigTP2, sigTP3, sigTP4);
    sendTelegramMessage(String(msg));

  } else if (m30Trend == "DN" && pullbackSellZone && m15SellTurn && sigSide != "SELL") {
    sigSide  = "SELL";
    sigEntry = entry;
    sigSL    = entry + (atrVal * 1.5);
    sigTP1   = entry - (atrVal * 1.0);
    sigTP2   = entry - (atrVal * 1.8);
    sigTP3   = entry - (atrVal * 2.6);
    sigTP4   = entry - (atrVal * 3.5);
    sigTimeStr = String(timeBuf);
    sigCreatedMillis = millis();
    tp1HitNotified = false;
    slHitNotified  = false;
    isAlertPopupActive = true;
    alertPopupStart = millis();

    char msg[320];
    snprintf(msg, sizeof(msg),
      "🔴 <b>GOLD# SELL SIGNAL (@%s)</b>\n"
      "━━━━━━━━━━━━━━\n"
      "🎯 <b>Entry:</b> %.2f\n"
      "🛑 <b>SL:</b> %.2f\n"
      "✅ <b>TP1:</b> %.2f\n"
      "✅ <b>TP2:</b> %.2f\n"
      "✅ <b>TP3:</b> %.2f\n"
      "✅ <b>TP4:</b> %.2f\n"
      "━━━━━━━━━━━━━━\n"
      "📊 M30 Trend: DOWN | M15 Pullback",
      sigTimeStr.c_str(), sigEntry, sigSL, sigTP1, sigTP2, sigTP3, sigTP4);
    sendTelegramMessage(String(msg));
  }
}

void fetchM15CandlesTwelve() {
  if (WiFi.status() != WL_CONNECTED || !isMarketOpen) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);
  HTTPClient http;
  http.setTimeout(5000);

  String url = "https://api.twelvedata.com/time_series?symbol=XAU/USD&interval=15min&outputsize=" + String(MAX_M15_BARS) + "&apikey=" + twelveApiKey;
  if (http.begin(client, url)) {
    if (http.GET() == 200) {
      String payload = http.getString();
      float tHigh[MAX_M15_BARS], tLow[MAX_M15_BARS], tClose[MAX_M15_BARS];
      int pos = 0, count = 0;

      while (count < MAX_M15_BARS) {
        int nextH = -1, nextL = -1, nextC = -1;
        float h = parseJsonFloat(payload, "high", pos, nextH);
        if (nextH == -1) break;
        float l = parseJsonFloat(payload, "low", nextH, nextL);
        float c = parseJsonFloat(payload, "close", nextL, nextC);
        if (nextC == -1) break;

        tHigh[count]  = h + brokerOffset;
        tLow[count]   = l + brokerOffset;
        tClose[count] = c + brokerOffset;
        count++;
        pos = nextC + 1;
      }

      if (count > 35) {
        for (int i = 0; i < count; i++) {
          m15High[i]  = tHigh[count - 1 - i];
          m15Low[i]   = tLow[count - 1 - i];
          m15Close[i] = tClose[count - 1 - i];
        }
        float rawXauSpot = m15Close[count - 1] - brokerOffset;
        liveGoldPrice = m15Close[count - 1];

        if (lastRawPaxg > 100.0) {
          autoSpreadOffset = rawXauSpot - lastRawPaxg;
          hasOffsetSynced = true;
        }

        analyzeM15Pullback(count);
      }
    }
    http.end();
  }
}

void showStepScreen(int stepNum, String title, String subText, int percent) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("STEP %d/3: %s", stepNum, title.c_str());
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setCursor(10, 22);
  display.println(subText);
  display.setCursor(10, 34);
  display.printf("Progress: %d%%", percent);
  display.drawRect(0, 50, 128, 12, SSD1306_WHITE);
  display.fillRect(2, 52, map(percent, 0, 100, 0, 124), 8, SSD1306_WHITE);
  display.display();
}

void handleRoot() {
  int n = WiFi.scanNetworks();
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;padding:20px;max-width:400px;margin:auto;}";
  html += "select,input,button{width:100%;padding:12px;margin:8px 0;font-size:16px;box-sizing:border-box;border-radius:6px;}";
  html += ".btn-save{background:#007BFF;color:white;border:none;cursor:pointer;}";
  html += ".btn-off{background:#6c757d;color:white;border:none;cursor:pointer;margin-top:15px;}</style></head><body>";
  html += "<h2>ESP32 Desk Bot Setup</h2>";
  html += "<form action='/save' method='POST'><label>Select Wi-Fi (2.4GHz):</label><select name='ssid'>";
  for (int i = 0; i < n; ++i) {
    html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  html += "</select><label>Password:</label><input type='password' name='pass' placeholder='Wi-Fi Password'>";
  html += "<label>Broker Price Offset (+/- USD):</label><input type='number' step='0.1' name='offset' value='" + String(brokerOffset, 1) + "'>";
  html += "<button type='submit' class='btn-save'>Save & Connect</button></form>";
  html += "<form action='/offline' method='POST'><button type='submit' class='btn-off'>Use Offline Mode</button></form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  float offset = server.arg("offset").toFloat();

  preferences.begin("wifi-config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putFloat("boffset", offset);
  preferences.putBool("offline", false);
  preferences.end();

  server.send(200, "text/html", "<html><body style='font-family:sans-serif;text-align:center;padding:40px;'><h2>Saved!</h2><p>Closing Hotspot & Connecting...</p></body></html>");
  delay(1500);
  WiFi.softAPdisconnect(true);
  ESP.restart();
}

void handleOffline() {
  preferences.begin("wifi-config", false);
  preferences.putBool("offline", true);
  preferences.end();
  server.send(200, "text/html", "<html><body style='font-family:sans-serif;text-align:center;padding:40px;'><h2>Offline Mode</h2><p>Closing Hotspot...</p></body></html>");
  delay(1500);
  WiFi.softAPdisconnect(true);
  ESP.restart();
}

void startSetupPortal() {
  isSetupMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_Clock_Setup");
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("== SETUP WI-FI ==");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setCursor(0, 16);
  display.println("1. Wi-Fi: ESP32_Clock_Setup");
  display.setCursor(0, 32);
  display.println("2. Web: 192.168.4.1");
  display.display();
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/offline", HTTP_POST, handleOffline);
  server.begin();
}

void drawCuteBotFace(unsigned long elapsed) {
  display.clearDisplay();
  int leftEyeX = 36, rightEyeX = 92, eyeY = 26;
  bool isBlinking = (elapsed >= 700 && elapsed < 950) || (elapsed >= 1500 && elapsed < 1750);
  bool isHappy = (elapsed >= 1750);

  if (isHappy || faceEmotion == 1) {
    for (int i = -14; i <= 14; i++) {
      int yOffset = (i * i) / 24;
      display.fillCircle(leftEyeX + i, eyeY - 4 + yOffset, 2, SSD1306_WHITE);
      display.fillCircle(rightEyeX + i, eyeY - 4 + yOffset, 2, SSD1306_WHITE);
    }
  } else if (isBlinking) {
    display.fillRoundRect(leftEyeX - 16, eyeY - 2, 32, 5, 2, SSD1306_WHITE);
    display.fillRoundRect(rightEyeX - 16, eyeY - 2, 32, 5, 2, SSD1306_WHITE);
  } else {
    display.fillRoundRect(leftEyeX - 15, eyeY - 14, 30, 28, 8, SSD1306_WHITE);
    display.fillRoundRect(rightEyeX - 15, eyeY - 14, 30, 28, 8, SSD1306_WHITE);
    int lookShift = (elapsed > 950) ? 4 : -3;
    display.fillCircle(leftEyeX + lookShift - 3, eyeY - 4, 4, SSD1306_BLACK);
    display.fillCircle(rightEyeX + lookShift - 3, eyeY - 4, 4, SSD1306_BLACK);
  }

  display.drawLine(14, 38, 18, 34, SSD1306_WHITE);
  display.drawLine(18, 38, 22, 34, SSD1306_WHITE);
  display.drawLine(106, 38, 110, 34, SSD1306_WHITE);
  display.drawLine(110, 38, 114, 34, SSD1306_WHITE);
  display.drawCircle(60, 46, 4, SSD1306_WHITE);
  display.drawCircle(68, 46, 4, SSD1306_WHITE);
  display.fillRect(54, 40, 20, 6, SSD1306_BLACK);
  display.display();
}

void drawTradeAlertPopup() {
  display.clearDisplay();
  display.fillRect(0, 0, 128, 13, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(3, 3);
  display.printf("%s @%s (SENT TG!)", sigSide.c_str(), sigTimeStr.c_str());

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 17);
  display.printf("IN : %.1f", sigEntry);
  display.setCursor(66, 17);
  display.printf("SL:%.1f", sigSL);

  display.drawLine(0, 29, 128, 29, SSD1306_WHITE);
  display.setCursor(0, 34);
  display.printf("TP1:%.1f", sigTP1);
  display.setCursor(66, 34);
  display.printf("TP2:%.1f", sigTP2);

  display.setCursor(0, 48);
  display.printf("TP3:%.1f", sigTP3);
  display.setCursor(66, 48);
  display.printf("TP4:%.1f", sigTP4);
  display.display();
}

void handleBootButton() {
  if (digitalRead(0) == LOW) {
    unsigned long pressStart = millis();
    while (digitalRead(0) == LOW) {
      esp_task_wdt_reset();
      if (millis() - pressStart >= 3000) {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(10, 25);
        display.println("Resetting Wi-Fi...");
        display.display();
        preferences.begin("wifi-config", false);
        preferences.clear();
        preferences.end();
        delay(1000);
        ESP.restart();
      }
      delay(20);
    }

    if (millis() - pressStart >= 50) {
      enableCuteFace = !enableCuteFace;
      showCuteFace = false;
      lastModeSwitch = millis();

      preferences.begin("wifi-config", false);
      preferences.putBool("cuteface", enableCuteFace);
      preferences.end();

      toastMsg = enableCuteFace ? "FACE ANIM: ON" : "FACE ANIM: OFF";
      toastUntil = millis() + 1500;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(100000);
  pinMode(0, INPUT_PULLUP);

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 15000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) for (;;);
  }
  display.ssd1306_command(SSD1306_DISPLAYON);

  showStepScreen(1, "SYSTEM BOOT", "Starting Desk Bot", 33);
  delay(400);

  preferences.begin("wifi-config", true);
  savedSSID      = preferences.getString("ssid", "");
  savedPass      = preferences.getString("pass", "");
  brokerOffset   = preferences.getFloat("boffset", 0.0);
  enableCuteFace = preferences.getBool("cuteface", false);
  isOfflineMode  = preferences.getBool("offline", false);
  preferences.end();

  if (isOfflineMode) {
    WiFi.mode(WIFI_OFF);
    lastModeSwitch = millis();
    return;
  }

  if (savedSSID == "") {
    startSetupPortal();
    return;
  }

  showStepScreen(2, "WI-FI CONNECT", savedSSID.substring(0, 15), 66);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 25) {
    esp_task_wdt_reset();
    delay(500);
    retry++;
  }

  showStepScreen(3, "M30/M15 + TG", "Syncing Live Gold...", 100);
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    int timeRetry = 0;
    while (!getLocalTime(&timeinfo) && timeRetry < 15) {
      esp_task_wdt_reset();
      delay(500);
      timeRetry++;
    }
    isMarketOpen = checkXauMarketOpen(timeinfo);
    isNewsTime   = checkNewsWindow(timeinfo);
    if (isMarketOpen) {
      fetchRealTimeTick1Sec();
      esp_task_wdt_reset();
      updateM30TrendTwelve();
      esp_task_wdt_reset();
      fetchM15CandlesTwelve();
      lastM30Calc = millis();
      lastM15Calc = millis();
      lastFastTick = millis();
    }
    sendTelegramMessage("🤖 <b>ESP32 Gold Bot Online (M30/M15)!</b>\nราคาปัจจุบัน: " + String(liveGoldPrice, 2) + "\nพร้อมเฝ้ากราฟ M30/M15 และแจ้งเตือน 24 ชม. ครับ");
  }
  lastModeSwitch = millis();
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  if (now - lastDisplayKeepAlive >= 30000) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    lastDisplayKeepAlive = now;
  }

  handleBootButton();

  if (isSetupMode) {
    server.handleClient();
    return;
  }

  if (isOfflineMode) {
    if (enableCuteFace) {
      unsigned long elapsed = now - lastModeSwitch;
      if (!showCuteFace && elapsed >= clockDuration) {
        showCuteFace = true;
        lastModeSwitch = now;
        elapsed = 0;
        faceEmotion = random(0, 2);
      } else if (showCuteFace && elapsed >= faceDuration) {
        showCuteFace = false;
        lastModeSwitch = now;
        elapsed = 0;
      }
      if (showCuteFace) {
        drawCuteBotFace(elapsed);
        delay(50);
        return;
      }
    }

    unsigned long totalSec = millis() / 1000;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("OFFLINE DESK BOT");
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
    display.setTextSize(3);
    display.setCursor(6, 20);
    display.printf("%02d:%02d", (int)((totalSec / 3600) % 24), (int)((totalSec / 60) % 60));
    display.setTextSize(1);
    display.setCursor(104, 34);
    display.printf(":%02d", (int)(totalSec % 60));
    display.drawLine(0, 50, 128, 50, SSD1306_WHITE);
    display.setCursor(4, 54);
    display.print("BOOT 1x:Face 3s:WiFi");
    display.display();
    delay(100);
    return;
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    if (WiFi.status() != WL_CONNECTED && (now - lastFastTick >= 5000)) {
      WiFi.reconnect();
      lastFastTick = now;
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 20);
    display.println("Reconnecting WiFi...");
    display.setCursor(10, 38);
    display.println("Hold BOOT 3s: Change");
    display.display();
    delay(200);
    return;
  }

  isMarketOpen = checkXauMarketOpen(timeinfo);
  isNewsTime   = checkNewsWindow(timeinfo);

  if (isMarketOpen) {
    if (WiFi.status() != WL_CONNECTED) {
      if (now - lastFastTick >= 5000) {
        WiFi.reconnect();
        lastFastTick = now;
      }
    } else {
      if (now - lastFastTick >= fastTickInterval) {
        fetchRealTimeTick1Sec();
        lastFastTick = now;
      }
      if (now - lastM30Calc >= m30CalcInterval) {
        updateM30TrendTwelve();
        lastM30Calc = now;
      }
      if (now - lastM15Calc >= m15CalcInterval) {
        fetchM15CandlesTwelve();
        lastM15Calc = now;
      }
    }
  }

  // เช็กผลการชน TP1 หรือ SL แบบเรียลไทม์แล้วแจ้งเตือนเข้า Telegram
  if ((sigSide == "BUY" || sigSide == "SELL") && liveGoldPrice > 100.0) {
    if (sigSide == "BUY") {
      if (!tp1HitNotified && liveGoldPrice >= sigTP1) {
        tp1HitNotified = true;
        sendTelegramMessage("💰 <b>TP1 HIT! (BUY M15)</b>\nราคาถึงเป้าหมาย TP1: " + String(sigTP1, 2) + " เรียบร้อยครับ");
        sigSide = "WAIT";
      } else if (!slHitNotified && liveGoldPrice <= sigSL) {
        slHitNotified = true;
        sendTelegramMessage("⚠️ <b>SL HIT (BUY M15)</b>\nราคาตัดขาดทุนที่: " + String(sigSL, 2));
        sigSide = "WAIT";
      }
    } else if (sigSide == "SELL") {
      if (!tp1HitNotified && liveGoldPrice <= sigTP1) {
        tp1HitNotified = true;
        sendTelegramMessage("💰 <b>TP1 HIT! (SELL M15)</b>\nราคาถึงเป้าหมาย TP1: " + String(sigTP1, 2) + " เรียบร้อยครับ");
        sigSide = "WAIT";
      } else if (!slHitNotified && liveGoldPrice >= sigSL) {
        slHitNotified = true;
        sendTelegramMessage("⚠️ <b>SL HIT (SELL M15)</b>\nราคาตัดขาดทุนที่: " + String(sigSL, 2));
        sigSide = "WAIT";
      }
    }

    if (now - sigCreatedMillis > signalMaxAgeMs) {
      sigSide = "WAIT";
    }
  }

  if (isAlertPopupActive && !isNewsTime) {
    if (now - alertPopupStart < alertPopupDuration) {
      drawTradeAlertPopup();
      delay(100);
      return;
    } else {
      isAlertPopupActive = false;
      lastModeSwitch = now;
    }
  }

  if (enableCuteFace) {
    unsigned long elapsed = now - lastModeSwitch;
    if (!showCuteFace && elapsed >= clockDuration) {
      showCuteFace = true;
      lastModeSwitch = now;
      elapsed = 0;
      faceEmotion = random(0, 2);
    } else if (showCuteFace && elapsed >= faceDuration) {
      showCuteFace = false;
      lastModeSwitch = now;
      elapsed = 0;
    }
    if (showCuteFace) {
      drawCuteBotFace(elapsed);
      delay(50);
      return;
    }
  } else {
    showCuteFace = false;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  char dayShort[5];
  strftime(dayShort, sizeof(dayShort), "%a", &timeinfo);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("%s %02d/%02d/%02d", dayShort, timeinfo.tm_mday, timeinfo.tm_mon + 1, (timeinfo.tm_year + 1900) % 100);

  if (isMarketOpen) {
    if (liveGoldPrice > 0) {
      display.setCursor(74, 0);
      display.printf("AU:%.1f", liveGoldPrice);
    } else {
      display.setCursor(80, 0);
      display.print("AU:OPEN");
    }
  } else {
    display.setCursor(74, 0);
    display.print("AU:CLOSE");
  }
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  display.setTextSize(3);
  display.setCursor(6, 15);
  display.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  display.setTextSize(1);
  display.setCursor(104, 29);
  display.printf(":%02d", timeinfo.tm_sec);

  display.drawLine(0, 41, 128, 41, SSD1306_WHITE);
  display.setTextSize(1);

  if (now < toastUntil) {
    display.setCursor(0, 45);
    display.print(toastMsg);
    display.setCursor(0, 56);
    display.print("Saved to Memory");
  } else if (!isMarketOpen) {
    display.setCursor(0, 45);
    display.print("XAU MARKET CLOSED");
    display.setCursor(0, 56);
    display.print("Open Mon 05:00 TH");
  } else if (isNewsTime) {
    display.setCursor(0, 45);
    display.print("NEWS GUARD ACTIVE");
    display.setCursor(0, 56);
    display.print("Pause Signal (Safe)");
  } else if (sigSide == "BUY" || sigSide == "SELL") {
    int minsAgo = (now - sigCreatedMillis) / 60000;
    display.setCursor(0, 45);
    display.printf("@%s IN:%.0f", sigTimeStr.c_str(), sigEntry);
    display.setCursor(0, 56);
    display.printf("SL:%.0f TP:%.0f %dm", sigSL, sigTP1, minsAgo);
  } else {
    display.setCursor(0, 45);
    display.printf("M30 TREND: %s", m30Trend.c_str());
    display.setCursor(0, 56);
    display.print("Wait M15 Pullback");
  }

  if (!isMarketOpen) {
    display.drawRoundRect(101, 44, 26, 19, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(105, 50);
    display.print("OFF");
  } else if (isNewsTime) {
    display.drawRoundRect(101, 44, 26, 19, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(102, 50);
    display.print("NEWS");
  } else if (sigSide == "BUY") {
    display.fillRoundRect(101, 44, 26, 19, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(2);
    display.setCursor(109, 46);
    display.print("B");
  } else if (sigSide == "SELL") {
    display.drawRoundRect(101, 44, 26, 19, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(109, 46);
    display.print("S");
  } else {
    display.drawRoundRect(101, 44, 26, 19, 3, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(106, 50);
    display.print("--");
  }

  display.display();
  delay(50);
}