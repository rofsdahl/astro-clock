// astro-clock

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include "nexa-tx.h"

#define DEBUG_BEGIN(...)   Serial.begin(__VA_ARGS__)
#define DEBUG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
//#define DEBUG_BEGIN(...)   // Blank line - no code
//#define DEBUG_PRINTF(...)  // Blank line - no code
//#define DEBUG_PRINTLN(...) // Blank line - no code

#define PIN_LEFT_BUTTON    0
#define PIN_RIGHT_BUTTON  35
#define PIN_RF_TX         26

#define SEC            (1000UL)
#define MIN            (60*SEC)
#define HOUR           (60*MIN)
#define X_MAX          135
#define Y_MAX          240
#define MAX_NETWORKS   20
#define MAX_SSID_LEN   32

typedef void (*VoidFunc)();

struct MenuItem {
  const char *label;
  VoidFunc action;
};

struct SunTimes {
  int16_t sunriseMin, sunsetMin;
  bool valid;
};

struct Forecast {
  float pressure;
  float temp;
  float relHumidity;
  float windFromDir;
  float windSpeed;
};

struct Line {
  int8_t x1, y1, x2, y2;
};

constexpr uint32_t NEXA_ADDRESS = 0x038E8E8E;
constexpr double LAT_DEG = 59.872;
constexpr double LON_DEG = 10.797;
constexpr double SOLAR_ALT_DEG = -0.833;
constexpr int ALTITUDE_M = 120;

TFT_eSPI tft = TFT_eSPI();
NexaTx nexaTx = NexaTx(PIN_RF_TX);
Preferences preferences;
unsigned long tLastDisplayUpdate = 0;
unsigned long tLastWifiAttempt = 0;
unsigned long tLastWeatherForecast = 0;
unsigned long tLastNexaUpdate = 0;
unsigned long tLastAstroUpdate = 0;
volatile bool wifiStaGotIp = false;
volatile bool ntpSyncDone = false;
struct tm tBoot = {0};
uint32_t rebootCount;
esp_reset_reason_t resetReason;

// ===========================
// Network functions
// ===========================

void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifiStaGotIp = true;
  }
}

void onTimeSync(struct timeval *tv) {
  ntpSyncDone = true;
}

void connectWiFi() {
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  if (ssid.isEmpty()) {
    DEBUG_PRINTLN("No SSID stored");
    return;
  }
  WiFi.begin(ssid.c_str(), password.c_str());
}

Forecast getWeatherForecast() {
  DEBUG_PRINTF("Heap before HTTP: %u\n", ESP.getFreeHeap());
  DEBUG_PRINTF("Min heap so far: %u\n", ESP.getMinFreeHeap());
  size_t largestFree = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  DEBUG_PRINTF("Largest free block: %u\n", largestFree);

  //if (funny business detected)
  //  ESP.restart();

  HTTPClient http;
  char url[150];
  snprintf(url, sizeof(url),
    "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%.3f&lon=%.3f&altitude=%d",
    LAT_DEG, LON_DEG, ALTITUDE_M);
  http.begin(url);
  http.addHeader("User-Agent", "ESP32 AstroClock https://github.com/rofsdahl/astro-clock");
  int httpCode = http.GET();

  Forecast forecast{ 0 };
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DEBUG_PRINTF("Payload length: %d\n", payload.length());

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);

    DEBUG_PRINTF("Heap after JSON: %u\n", ESP.getFreeHeap());

    if (!err) {
      forecast.pressure    = doc["properties"]["timeseries"][0]["data"]["instant"]["details"]["air_pressure_at_sea_level"];
      forecast.temp        = doc["properties"]["timeseries"][0]["data"]["instant"]["details"]["air_temperature"];
      forecast.relHumidity = doc["properties"]["timeseries"][0]["data"]["instant"]["details"]["relative_humidity"];
      forecast.windFromDir = doc["properties"]["timeseries"][0]["data"]["instant"]["details"]["wind_from_direction"];
      forecast.windSpeed   = doc["properties"]["timeseries"][0]["data"]["instant"]["details"]["wind_speed"];
      DEBUG_PRINTF("Bp: %0.1f, T: %0.1f, RH: %0.1f, Ws: %0.1f, Wd: %0.1f\n",
        forecast.pressure, forecast.temp, forecast.relHumidity, forecast.windFromDir, forecast.windSpeed
      );
    }
    else {
      DEBUG_PRINTF("JSON error: %s\n", err.c_str());
    }
  }
  else {
    DEBUG_PRINTF("HTTP error: %d\n", httpCode);
  }
  http.end();

  delay(1000);
  DEBUG_PRINTF("Heap after HTTP: %u\n", ESP.getFreeHeap());

  return forecast;
}

// ===========================
// Astronomical calcuations
// ===========================

SunTimes calcSunTimes(const tm& t) {

  // Approximate solar noon in days
  double d = t.tm_yday + 0.5;
  double gamma = 2.0 * PI * d / 365.25;

  // Declination
  double decl =
      0.006918
    - 0.399912 * cos(1 * gamma)
    + 0.070257 * sin(1 * gamma)
    - 0.006758 * cos(2 * gamma)
    + 0.000907 * sin(2 * gamma)
    - 0.002697 * cos(3 * gamma)
    + 0.00148  * sin(3 * gamma);

  // Equation of Time (min)
  double eqTime = 229.18 * (
      0.000075
    + 0.001868 * cos(1 * gamma)
    - 0.032077 * sin(1 * gamma)
    - 0.014615 * cos(2 * gamma)
    - 0.040849 * sin(2 * gamma)
    + 0.000306 * cos(3 * gamma)
    + 0.00114  * sin(3 * gamma)
  );

  // Hour angle with horizon correction
  static constexpr double latitudeRad = LAT_DEG * PI / 180.0;
  static constexpr double solarAltRad = SOLAR_ALT_DEG * PI / 180.0;
  double cosH =
      (sin(solarAltRad) - sin(latitudeRad) * sin(decl)) /
      (cos(latitudeRad) * cos(decl));

  SunTimes s{ -1, -1, false };
  if (cosH < -1.0 || cosH > 1.0) {
    // Midnight sun / polar night
    return s;
  }

  double H = acos(cosH);

  // Day length (min)
  double daylight = 2.0 * H * 24.0 * 60.0 / (2.0 * PI);

  // Local solar time
  double sunriseSolar = 12.0 * 60.0 - daylight / 2.0;
  double sunsetSolar  = 12.0 * 60.0 + daylight / 2.0;

  // UTC values
  double lonCorr = 4.0 * LON_DEG;
  double sunriseUTC = sunriseSolar - eqTime - lonCorr;
  double sunsetUTC  = sunsetSolar  - eqTime - lonCorr;

  // Convert to local time (relies on configTzTime called)
  int8_t tz = 60; // CET
  if (t.tm_isdst > 0) tz += 60; // CEST

  s.sunriseMin = (int16_t)lround(sunriseUTC + tz);
  s.sunsetMin  = (int16_t)lround(sunsetUTC  + tz);
  s.valid = true;

  return s;
}

double calcMoonPhase(const tm& t) {
  constexpr double SYNODIC_MONTH = 29.53058867; // days
  constexpr time_t NEW_MOON_REF = 947182440;    // 2000-01-06 18:14 UTC

  tm tCopy = t;
  time_t now = mktime(&tCopy);
  double days = difftime(now, NEW_MOON_REF) / 86400.0;
  double phase = fmod(days, SYNODIC_MONTH) / SYNODIC_MONTH;
  if (phase < 0) phase += 1.0;

  return phase;
}

// ===========================
// Display functions
// ===========================

void drawWindArrow(int xc, int yc, float wd, int l) {

  float theta = (270.0 - wd) * DEG_TO_RAD;
  float dx = cos(theta);
  float dy = sin(theta);

  // Main line endpoints (through center)
  int x1 = xc - l * dx;
  int y1 = yc + l * dy;
  int x2 = xc + l * dx;
  int y2 = yc - l * dy;
  tft.drawLine(x1, y1, x2, y2, TFT_WHITE);

  // Arrowhead
  float arrowLength = l * 0.20; // 20% of length
  float arrowWidth  = l * 0.10; // 10% of length

  // Perpendicular unit vector
  float px = -dy;
  float py = dx;

  // Base of arrowhead
  float bx = x2 - arrowLength * dx;
  float by = y2 + arrowLength * dy;

  // Side points
  int x3 = bx + arrowWidth * px;
  int y3 = by - arrowWidth * py;

  int x4 = bx - arrowWidth * px;
  int y4 = by + arrowWidth * py;

  // Filled arrowhead looks nicer on TFT
  tft.fillTriangle(x2, y2, x3, y3, x4, y4, TFT_WHITE);
}

void drawSunriseIcon(int x, int y) {
  tft.fillCircle(x+21, y+17,     10, TFT_YELLOW);
  tft.fillRect  (x+11, y+18, 21, 10, TFT_BLACK);

  static constexpr Line lines[] = {
    {   0, -13,   0, -17 }, // ↑
    { -11,  -5, -16,  -7 }, // ↖ 30
    {  11,  -5,  16,  -7 }, // ↗ 30
    {  -8, -11, -11, -14 }, // ↖ 60
    {   8, -11,  11, -14 }, // ↗ 60
    { -20,   0,  20,   0 }  // horizon
  };
  for (uint8_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    tft.drawLine(x+21+lines[i].x1, y+17+lines[i].y1, x+21+lines[i].x2, y+17+lines[i].y2, TFT_YELLOW);
  }
}

void drawSunsetIcon(int x, int y) {
  tft.fillCircle(x+21, y+10,     10, TFT_ORANGE);
  tft.fillRect  (x+11, y+11, 21, 10, TFT_BLACK);

  static constexpr Line lines[] = {
    { -20,   0,  20,   0 }, // horizon
    { -13,   3,   5,   3 }, // reflection
    {  -5,   6,  13,   6 }, // reflection
    {  -6,   9,   3,   9 }  // reflection
  };
  for (uint8_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    tft.drawLine(x+21+lines[i].x1, y+10+lines[i].y1, x+21+lines[i].x2, y+10+lines[i].y2, TFT_ORANGE);
  }
}

void drawMoonPhaseIcon(int x, int y, double phase) {
  phase = fmod(phase, 1.0);
  if (phase < 0) phase += 1.0;

  // Start with full moon
  tft.fillCircle(x+10, y+10, 10, TFT_WHITE);

  // Mask half the moon
  bool waxing = phase < 0.5;
  tft.fillRect(waxing ? x : x+10, y, 10, 20, TFT_BLACK);

  // Draw ellipse with correct width (triangle wave) and color
  // 0.000 = New Moon        -> 100% width, black ellipsis
  // 0.125 = Waxing Crescent ->  50% width, black ellipsis
  // 0.250 = First Quarter   ->   0% width, white ellipsis
  // 0.375 = Waxing Gibbous  ->  50% width, white ellipsis
  // 0.500 = Full Moon       -> 100% width, white ellipsis
  // 0.625 = Waning Gibbous  ->  50% width, white ellipsis
  // 0.750 = Third Quarter   ->   0% width, white ellipsis
  // 0.875 = Waning Crescent ->  50% width, black ellipsis
  // 1.000 = New Moon        -> 100% width, black ellipsis
  double w = fabs(1.0 - 4.0 * fabs(phase - 0.5));
  int rx = (int)(10 * w);
  uint16_t col = (phase < 0.25) || (phase > 0.75) ? TFT_BLACK : TFT_WHITE;
  tft.fillEllipse(x+10, y+10, rx, 10, col);

  // Outline
  tft.drawCircle(x+10, y+10, 10, TFT_DARKGREY);
}

void drawTransmitIcon(int x, int y, uint16_t color, bool on, bool tx) {
  tft.fillRect(x, y, 30, 17, TFT_BLACK);
  if (on) tft.fillCircle(x+8, y+8, 8, color);
  else    tft.drawCircle(x+8, y+8, 8, color);
  if (tx) {
    tft.drawArc(x+8, y+8, 11, 11, 248, 292, color, TFT_BLACK);
    tft.drawArc(x+8, y+8, 14, 14, 242, 298, color, TFT_BLACK);
    tft.drawArc(x+8, y+8, 17, 17, 240, 300, color, TFT_BLACK);
  }
}

void updateDisplay(
    const tm& t,
    Forecast forecast,
    SunTimes sun,
    double moonPhase,
    bool wifiConnected,
    bool nexaOn,
    bool drawIcons) {

  // Time
  constexpr uint8_t yTime = 0;
  int w = 0;
  char buf[25];
  strftime(buf, sizeof(buf), " %H:%M ", &t);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(buf, X_MAX/2, yTime, 6);

  // Date
  constexpr uint8_t yDate = 48;
  strftime(buf, sizeof(buf), " %a %d.%m ", &t);
  tft.drawString(buf, X_MAX/2, yDate, 4);

  // Temp
  constexpr uint8_t yTemp = 78;
  tft.drawLine(0, yTemp-3, X_MAX, yTemp-3, TFT_LIGHTGREY);
  tft.setTextDatum(TL_DATUM);

  snprintf(buf, sizeof(buf), "%0.1f", forecast.temp);
  tft.setTextColor(forecast.temp>=0.0 ? TFT_YELLOW : TFT_SKYBLUE, TFT_BLACK);
  w = tft.drawString(buf, 0, yTemp, 4);
  tft.fillRect(w, yTemp, (X_MAX/2)-w, 26, TFT_BLACK);

  snprintf(buf, sizeof(buf), "%0.1f", forecast.relHumidity);
  tft.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  w = tft.drawString(buf, X_MAX/2, yTemp, 4);
  tft.fillRect((X_MAX/2)+w, yTemp, (X_MAX/2)-w, 26, TFT_BLACK);

  // Sunrise
  constexpr uint8_t ySunrise = 146;
  tft.drawLine(0, ySunrise-4, X_MAX, ySunrise-4, TFT_DARKGREY);
  snprintf(buf, sizeof(buf), "%02d:%02d", sun.sunriseMin / 60, sun.sunriseMin % 60);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  w = tft.drawString(buf, 43, ySunrise, 4);
  tft.fillRect(43+w, ySunrise, X_MAX-20-43-w, 26, TFT_BLACK);

  // Sunset
  constexpr uint8_t ySunset = 174;
  snprintf(buf, sizeof(buf), "%02d:%02d", sun.sunsetMin / 60, sun.sunsetMin % 60);
  w = tft.drawString(buf, 43, ySunset, 4);
  tft.fillRect(43+w, ySunset, X_MAX-20-43-w, 26, TFT_BLACK);

  // Status
  constexpr uint8_t yStatus = 203;
  tft.drawLine(0, yStatus-4, X_MAX, yStatus-4, TFT_DARKGREY);
  tft.setTextDatum(TR_DATUM);
  snprintf(buf, sizeof(buf), "Astro Clock %d:%d", rebootCount, resetReason);
  tft.drawString(buf, X_MAX, yStatus+9, 2);
  strftime(buf, sizeof(buf), "%d.%m.%y %H:%M", &tBoot);
  tft.drawString(buf, X_MAX, yStatus+24, 2);

  if (drawIcons) {
    drawSunriseIcon  (0, ySunrise);
    drawSunsetIcon   (0, ySunset);
    drawMoonPhaseIcon(X_MAX-21, ySunrise, moonPhase);
    drawTransmitIcon (0, yStatus, TFT_BLUE, wifiConnected, false);
    drawTransmitIcon (0, yStatus+20, TFT_YELLOW, nexaOn, false);
  }
}

// ===========================
// Nexa functions
// ===========================

void transmitNexa(bool activation) {
  DEBUG_PRINTF("Transmit Nexa: %s\n", activation ? "ON" : "OFF");
  drawTransmitIcon(0, 223, TFT_YELLOW, activation, true);
  nexaTx.transmit(NEXA_ADDRESS, 1, activation);
  drawTransmitIcon(0, 223, TFT_YELLOW, activation, false);
}

void transmitNexaOn() {
  transmitNexa(true);
}

void transmitNexaOff() {
  transmitNexa(false);
}

bool updateNexa(uint16_t dayMin, SunTimes sun) {
  bool isNight = sun.valid &&
    (dayMin >= sun.sunsetMin || dayMin < sun.sunriseMin+30);
  bool isQuiet = dayMin < 6*60;
  bool isNexaOn = isNight && !isQuiet;
  transmitNexa(isNexaOn);
  return isNexaOn;
}

// ===========================
// Menu functions
// ===========================

void waitUntilBothReleased() {
  while (digitalRead(PIN_LEFT_BUTTON) == LOW || digitalRead(PIN_RIGHT_BUTTON) == LOW);
  delay(50);
}

void displayItems(const MenuItem items[],
                  int itemCount,
                  int selected,
                  int y,
                  uint8_t font,
                  uint8_t fontHeight) {

  int maxVisibleItems = (Y_MAX-y) / fontHeight;
  int firstVisibleItem = selected-maxVisibleItems+1;
  if (firstVisibleItem < 0) firstVisibleItem = 0;

  for (int i = 0; i < itemCount; i++) {
    if (i < firstVisibleItem) continue;
    if (y > Y_MAX-fontHeight) break;
    uint16_t fgCol = selected == i ? TFT_BLACK : TFT_LIGHTGREY;
    uint16_t bgCol = selected == i ? TFT_LIGHTGREY : TFT_BLACK;
    tft.setTextColor(fgCol, bgCol);
    int x = tft.drawString(items[i].label, 0, y, font);
    tft.fillRect(x, y, X_MAX-x, fontHeight, bgCol);
    y += fontHeight;
  }
}

int menuSelect(const MenuItem items[],
               int itemCount,
               int y,
               uint8_t font,
               uint8_t fontHeight) {

  tft.fillRect(0, y, X_MAX, Y_MAX-y, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);

  int selected = 0;
  displayItems(items, itemCount, selected, y, font, fontHeight);

  waitUntilBothReleased();

  while (true) {
    if (digitalRead(PIN_LEFT_BUTTON) == LOW) {
      delay(50);
      selected = (selected+1) % itemCount;
      displayItems(items, itemCount, selected, y, font, fontHeight);
      waitUntilBothReleased();
    }

    if (digitalRead(PIN_RIGHT_BUTTON) == LOW) {
      delay(50);

      DEBUG_PRINTF("Selected: %d\n", selected);
      if (items[selected].action == nullptr) {
        waitUntilBothReleased();
        return selected;
      }
      else {
        items[selected].action();
        displayItems(items, itemCount, selected, y, font, fontHeight);
        waitUntilBothReleased();
      }
    }
  }
  DEBUG_PRINTLN("This should never happen");
  ESP.restart();
  return -1;
}

void selectWiFiNetwork() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Networks:", 0, 0, 4);
  tft.drawString("Please wait...", 0, 30, 2);

  DEBUG_PRINTLN("Disconnecting WiFi...");
  WiFi.disconnect(true);
  delay(200);

  DEBUG_PRINTLN("Scan for WiFi networks...");
  int itemCount = WiFi.scanNetworks(false, true);
  if (itemCount > MAX_NETWORKS)
    itemCount = MAX_NETWORKS;

  static char ssids[MAX_NETWORKS][MAX_SSID_LEN+1];
  static MenuItem menuItems[MAX_NETWORKS+1];
  for (int i = 0; i < itemCount; i++) {
    strncpy(ssids[i], WiFi.SSID(i).c_str(), MAX_SSID_LEN);
    ssids[i][MAX_SSID_LEN] = '\0';
    menuItems[i] = {ssids[i], nullptr};
  }
  menuItems[itemCount++] = {"Exit (no selection)", nullptr};

  WiFi.scanDelete();

  int selected = menuSelect(menuItems, itemCount, 30, 2, 16);
  if (selected < itemCount-1) {
    DEBUG_PRINTF("Network selected: %s\n", ssids[selected]);
    preferences.putString("ssid", ssids[selected]);
  }
  tft.fillScreen(TFT_BLACK);
}

void enterWiFiPassword() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Password:", 0, 0, 4);

  char password[64] = {0};
  char ch = 0;
  uint8_t index = 0;

  int x = 0;
  int y = 30;
  int w = 11;
  tft.fillRect(x, y, w, 26, TFT_LIGHTGREY);

  waitUntilBothReleased();

  while (true) {
    // M: Cycle character
    if (digitalRead(PIN_LEFT_BUTTON) == LOW) {
      delay(50);
      if      (ch == 0)   ch = 'a';
      else if (ch == '/') ch = 'a';
      else if (ch == 'z') ch = 'A';
      else if (ch == 'Z') ch = '0';
      else if (ch == '@') ch = '[';
      else if (ch == '`') ch = '{';
      else if (ch >= '~') ch = ' ';
      else                ch++;

      tft.setTextColor(TFT_BLACK, TFT_LIGHTGREY);
      tft.fillRect(x, y, w, 26, TFT_BLACK);
      if (ch == ' ') w = tft.drawString("sp", x, y, 4);
      else           w = tft.drawChar(ch, x, y, 4);
      // TODO: Improve auto-scroll speed (slow first, then increase)
      delay(200);
    }
    // +: Next position / finish (TODO: Erase by long press?)
    if (digitalRead(PIN_RIGHT_BUTTON) == LOW) {
      delay(50);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.fillRect(x, y, w, 26, TFT_BLACK);

      password[index++] = ch;
      if (ch == 0 || index >= sizeof(password)-1) {
        break;
      }
      x += tft.drawChar('*', x, y, 4);
      if (x > 110) {
        x = 0;
        y += 26;
      }
      ch = 0;
      w = 11;
      tft.fillRect(x, y, w, 26, TFT_LIGHTGREY);
      waitUntilBothReleased();
    }
  }

  password[index] = 0;
  DEBUG_PRINTF("Password entered: %s\n", password);
  preferences.putString("password", password);
  DEBUG_PRINTLN("Disconnecting WiFi...");
  WiFi.disconnect(true);
  waitUntilBothReleased();
  tft.fillScreen(TFT_BLACK);
}

void wifiConfigMenu() {
  static const MenuItem menuItems[] = {
    {"Network...", selectWiFiNetwork},
    {"Password...", enterWiFiPassword},
    {"Exit", nullptr}
  };
  static const int itemCount = sizeof(menuItems) / sizeof(menuItems[0]);
  menuSelect(menuItems, itemCount, 0, 4, 26);
  tft.fillScreen(TFT_BLACK);
}

void triggerWeatherUpdate() {
  tLastWeatherForecast = 0;
  drawTransmitIcon(0, 223, TFT_MAGENTA, false, false);
}

void triggerReboot() {
  preferences.putUInt("rebootCount", 0);
  ESP.restart();
}

void mainMenu() {
  static const MenuItem menuItems[] = {
    {"Nexa ON", transmitNexaOn},
    {"Nexa OFF", transmitNexaOff},
    {"WiFi config", wifiConfigMenu},
    {"Get weather", triggerWeatherUpdate},
    {"Reboot", triggerReboot},
    {"Exit", nullptr}
  };
  static const int itemCount = sizeof(menuItems) / sizeof(menuItems[0]);
  menuSelect(menuItems, itemCount, 0, 4, 26);
  tft.fillScreen(TFT_BLACK);
}

// ===========================
// Main functions
// ===========================

void loop() {

  // Time
  struct tm t;
  getLocalTime(&t, 100);

  // State variables
  static Forecast forecast = { 0 };
  static SunTimes sun = { -1, -1, false };
  static double moonPhase = 0.0;
  static bool isWifiConnected = false;
  static bool isNexaOn = false;

  // Display
  if (tLastDisplayUpdate == 0 || millis()-tLastDisplayUpdate > 1*SEC) {
    bool drawIcons = tLastDisplayUpdate == 0;
    updateDisplay(t, forecast, sun, moonPhase, isWifiConnected, isNexaOn, drawIcons);
    tLastDisplayUpdate = millis();
  }

  // WiFi
  if (WiFi.status() != WL_CONNECTED) {
    isWifiConnected = false;
    if (tLastWifiAttempt == 0 || millis()-tLastWifiAttempt > 10*SEC) {
      DEBUG_PRINTLN("Connect to WiFi...");
      connectWiFi();
      tLastWifiAttempt = millis();
    }
  }
  else if (wifiStaGotIp == true) {
    DEBUG_PRINTF("WiFi connected, IP: %s\n", WiFi.localIP().toString());
    tLastWifiAttempt = millis(); // Avoid immediate reconnect
    tLastDisplayUpdate = 0; // Full display refresh
    isWifiConnected = true;
    wifiStaGotIp = false;
  }

  // Weather weather forecast
  if (WiFi.status() == WL_CONNECTED) {
    if (tLastWeatherForecast == 0 || millis()-tLastWeatherForecast >= 10*MIN) {
      DEBUG_PRINTLN("Weather...");
      forecast = getWeatherForecast();
      tLastWeatherForecast = millis();
    }
  }

  // Nexa update
  if (tLastNexaUpdate == 0 || millis()-tLastNexaUpdate >= 10*MIN) {
    DEBUG_PRINTLN("Nexa...");
    uint16_t dayMin = t.tm_hour * 60+t.tm_min;
    isNexaOn = updateNexa(dayMin, sun);
    tLastDisplayUpdate = 0; // Full display refresh
    tLastNexaUpdate = millis();
  }

  // Astro calculations
  if (tLastAstroUpdate == 0 || millis()-tLastAstroUpdate >= 1*HOUR) {
    DEBUG_PRINTLN("Astro...");
    sun = calcSunTimes(t);
    moonPhase = calcMoonPhase(t);
    tLastNexaUpdate = 0; // Nexa state may have changed
    tLastDisplayUpdate = 0; // Full display refresh
    tLastAstroUpdate = millis();
  }

  // NTP sync
  if (ntpSyncDone == true) {
    DEBUG_PRINTLN("NTP...");
    if (tBoot.tm_year == 0) getLocalTime(&tBoot, 100);
    tLastAstroUpdate = 0; // Recalculate after time sync
    ntpSyncDone = false;
  }

  // Menu
  if (digitalRead(PIN_LEFT_BUTTON) == LOW) {
    DEBUG_PRINTLN("Menu system...");
    mainMenu();
    DEBUG_PRINTLN("Menu exit...");
    tLastWifiAttempt = 0; // Immediate reconnect (if wifi config changed)
    tLastAstroUpdate = 0; // Recalculate after potential time change
    tLastDisplayUpdate = 0; // Full display refresh
  }
}

void setup() {
  DEBUG_BEGIN(115200);
  delay(2000);
  DEBUG_PRINTLN("---");
  DEBUG_PRINTF("%s %s %s\n", "Astro Clock", __DATE__, __TIME__);

  preferences.begin("astro-clock", false);
  rebootCount = preferences.getUInt("rebootCount", 0) + 1;
  preferences.putUInt("rebootCount", rebootCount);
  DEBUG_PRINTF("Reboot count: %d\n", rebootCount);
  resetReason = esp_reset_reason();
  DEBUG_PRINTF("Reset reason: %d\n", resetReason);

  pinMode(PIN_LEFT_BUTTON, INPUT);
  pinMode(PIN_RIGHT_BUTTON, INPUT);
  btStop();
  tft.init();
  tft.setRotation(0);
  tft.setTextSize(1);
  tft.fillScreen(TFT_BLACK);

  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  sntp_set_time_sync_notification_cb(onTimeSync);
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
}
