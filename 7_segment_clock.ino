#include <FastLED.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include "EspHtmlTemplateProcessor.h"
#include <TZ.h>
#include <PolledTimeout.h>
#include <time.h>
#include <coredecls.h> // settimeofday_cb()

// DEBUG
const bool debug = false;
const bool reset_eeprom = false; // set true if flash first time

// Networking
ESP8266WebServer server(80);
EspHtmlTemplateProcessor templateProcessor(&server);
String networkMode = "client";

// Timer
static esp8266::polledTimeout::periodicMs runEverySecond(1000);
byte reconnectWifiTimer = 0;
byte rebootTimer = 0;

// NTP
#define MYTZ TZ_Asia_Taipei
time_t ntpLastUpdate;
static uint32 ntpUpdateDelayMs = 3600000;

// LED
#define LED_PIN D5
#define LED_PWR_LIMIT 780
#define LED_DIGITS 4
#define LED_PER_DIGITS_STRIP 14
#define LED_BETWEEN_DIGITS_STRIPS 2
#define LED_COUNT (LED_DIGITS / 2) * LED_PER_DIGITS_STRIP + LED_BETWEEN_DIGITS_STRIPS * 1 + 3
CRGB leds[LED_COUNT];
byte brightness = 10;
long colorhex = 0xffffff;
bool blankLED = false;

// Segment
byte segGroups[7] = {
    5, // top, a
    4, // top right, b
    3, // bottom right, c
    2, // bottom, d
    1, // bottom left, e
    6, // top left, f
    7, // center, g
};
byte digits[14][7] = {
    // Lets define 10 numbers (0-9) with 7 segments each, 1 = segment is on, 0 = segment is off
    {1, 1, 1, 1, 1, 1, 0}, // 0 -> Show segments a - f, don't show g (center one)
    {0, 1, 1, 0, 0, 0, 0}, // 1 -> Show segments b + c (top and bottom right), nothing else
    {1, 1, 0, 1, 1, 0, 1}, // 2 -> and so on...
    {1, 1, 1, 1, 0, 0, 1}, // 3
    {0, 1, 1, 0, 0, 1, 1}, // 4
    {1, 0, 1, 1, 0, 1, 1}, // 5
    {1, 0, 1, 1, 1, 1, 1}, // 6
    {1, 1, 1, 0, 0, 0, 0}, // 7
    {1, 1, 1, 1, 1, 1, 1}, // 8
    {1, 1, 1, 1, 0, 1, 1}, // 9
    {0, 0, 0, 1, 1, 1, 1}, // t -> some letters from here on (index 10-13, so this won't interfere with using digits 0-9 by using index 0-9
    {0, 0, 0, 0, 1, 0, 1}, // r
    {0, 1, 1, 1, 0, 1, 1}, // y
    {0, 1, 1, 1, 1, 0, 1}  // d
};

// Time
#define IS_24_HOUR false
static time_t currentTime;
struct tm *localTimeInfo;

// EEPROM Config
struct ConfigData
{
  byte ledBrightness = 15;
  String ledColor = "#ffffff";

  String wifiSsid = "";
  String wifiPass = "";

  String ntpServer = "tw.pool.ntp.org";
} myConfig;

// html
String htmlFlashMshInfo;
String htmlFlashMshErr;

void setup()
{
  if (debug)
  {
    Serial.begin(115200);
    if (debug)
    {
      delay(3000);
    }
    Serial.println();
    debugLog("7 Segment Clock starting up...");
    debugLog(String("Configured for: ") + LED_COUNT + String(" leds"));
    debugLog(String("Power limited to (mA): ") + LED_PWR_LIMIT + String(" mA"));
  }
  initEEPROM();
  SPIFFS.begin();
  pinMode(LED_BUILTIN, OUTPUT);
  initBrightness();
  initLed();
  initWifiAndNTP();
  initWeb();
}

void loop()
{
  // run every second
  if (runEverySecond)
  {
    blankLED = !blankLED;
    if (networkMode == "client")
    {
      currentTime = time(nullptr);
      localTimeInfo = localtime(&currentTime);
      displayTime(localTimeInfo);
    }
    else if (networkMode == "AP")
    {
      digitalWrite(LED_BUILTIN, blankLED ? LOW : HIGH);
      leds[0] = blankLED ? colorhex : CRGB::Black;
      leds[1] = blankLED ? colorhex : CRGB::Black;
      leds[2] = blankLED ? colorhex : CRGB::Black;
      showDigit(4, 0);
      showDigit(3, 1);
      showDigit(2, 2);
      showDigit(1, 3);
    }

    if (reconnectWifiTimer > 0)
    {
      reconnectWifiTimer--;
      debugLog(String("reconnectWifiTimer: ") + reconnectWifiTimer);
      if (reconnectWifiTimer == 0)
      {
        initWifiAndNTP();
      }
    }

    if (rebootTimer > 0)
    {
      rebootTimer--;
      debugLog(String("rebootTimer: ") + rebootTimer);
      if (rebootTimer == 0)
      {
        ESP.restart();
      }
    }
  }

  FastLED.show();
  server.handleClient();
}

void displayTime(const tm *t)
{
  for (int i = 0; i < LED_COUNT; i++)
  {
    leds[i] = CRGB::Black;
  }

  int tHour = 0;
  if (IS_24_HOUR)
  {
    tHour = t->tm_hour;
  }
  else
  {
    tHour = t->tm_hour;
    if (tHour == 0)
      tHour = 12;
    else
      tHour = tHour > 12 ? tHour - 12 : tHour;
  }
  int tMinute = t->tm_min;
  int tSecond = t->tm_sec;

  if (tHour / 10 > 0)
  {
    showDigit((tHour / 10), 0);
  }
  showDigit((tHour % 10), 1);
  showDigit((tMinute / 10), 2);
  showDigit((tMinute % 10), 3);
  // showDigit((tSecond / 10), 2);
  // showDigit((tSecond % 10), 3);
  leds[0] = blankLED ? colorhex : CRGB::Black;
  leds[1] = blankLED ? colorhex : CRGB::Black;
  if (t->tm_hour > 12)
  {
    leds[2] = colorhex;
  }

  debugLog(String("[displayTime] ") + tHour + String(":") + tMinute + String(":") + tSecond);
}

void showDigit(byte digit, byte timePos)
{
  // HH:MM, pos: 01:23
  // debugLog(String("[showDigit] digit: ") + digit + String("pos: ") + timePos);
  for (byte segPos = 0; segPos < 7; segPos++)
  {
    if (digits[digit][segPos] == 0)
    {
      continue;
    }
    byte ledOffsetPos = timePos * 7;
    byte ledPos = segGroups[segPos] + ledOffsetPos - 1;
    // debugLog(String("ledPos: ") + ledPos);
    leds[ledPos + 3] = colorhex;
  }
}

void initEEPROM()
{
  EEPROM.begin(512);
  loadConfigFromEEPROM();

  debugLog("[initEEPROM] get config from EEPROM:");
  debugLog(String("led brightness: ") + String(myConfig.ledBrightness));
  debugLog(String("led color: ") + myConfig.ledColor);
  debugLog(String("wifi ssid: ") + myConfig.wifiSsid);
  debugLog(String("wifi pass: ") + myConfig.wifiPass);
  debugLog(String("ntp server: ") + myConfig.ntpServer);
}

void initBrightness()
{
  debugLog(String("[initBrightness] EEPROM value: ") + myConfig.ledBrightness);
  debugLog("[initBrightness] init...");
  if (myConfig.ledBrightness >= 0 && myConfig.ledBrightness <= 255)
  {
    brightness = myConfig.ledBrightness;
  }
}

void initLed()
{
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setTemperature(DirectSunlight);
  FastLED.setDither(1);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, LED_PWR_LIMIT);
  FastLED.setBrightness(brightness);
  FastLED.clear();
  FastLED.show();
}

void initWifiAndNTP()
{
  // conect Wifi
  debugLog(String("[initWifiAndNTP] Wifi connecting to ") + myConfig.wifiSsid);
  bool clientSuccess = false;

  if (myConfig.wifiSsid.length() > 0 && myConfig.wifiPass.length() > 0)
  {
    digitalWrite(LED_BUILTIN, LOW);
    WiFi.mode(WIFI_STA);
    WiFi.begin(myConfig.wifiSsid.c_str(), myConfig.wifiPass.c_str());
    clientSuccess = testWifi();
  }
  else
  {
    debugLog("[initWifiAndNTP] ssid or password is empty.");
  }

  if (clientSuccess)
  {
    digitalWrite(LED_BUILTIN, HIGH);
    networkMode = "client";
    debugLog("[initWifiAndNTP] Wifi connected");
    debugLog(String("[initWifiAndNTP] IP address: ") + WiFi.localIP().toString());
    // NTP sync
    debugLog("[initWifiAndNTP] NTP begin");
    configTime(MYTZ, myConfig.ntpServer.c_str());
    settimeofday_cb(handleSetTime);
  }
  else
  {
    setupAP();
  }
}

// OPTIONAL: change SNTP update delay
// a weak function is already defined and returns 1 hour
// it can be redefined:
uint32_t sntp_update_delay_MS_rfc_not_less_than_15000()
{
  //info_sntp_update_delay_MS_rfc_not_less_than_15000_has_been_called = true;
  return ntpUpdateDelayMs;
}

void handleSetTime()
{
  ntpLastUpdate = time(nullptr);
  debugLog("[handleSetTime] time: " + getFormattedTime(localtime(&ntpLastUpdate)));
}

bool testWifi(void)
{
  byte retry = 60;
  while (retry--)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("");
      return true;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  debugLog("Connect timed out, opening AP");
  return false;
}

void setupAP()
{
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(4, 3, 2, 1), IPAddress(4, 3, 2, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("7SegmentClock", "");
  networkMode = "AP";
  debugLog(String("[setupAP] wifi ssid: 7SegmentClock"));
  debugLog(String("[setupAP] AP IP address: ") + WiFi.softAPIP().toString());
}

String indexKeyProcessor(const String &key)
{
  if (key == "AP_IP")
    return WiFi.softAPIP().toString();
  else if (key == "LOCAL_IP")
    return WiFi.localIP().toString();
  else if (key == "WIFI_SSID")
    return myConfig.wifiSsid;
  else if (key == "WIFI_PASSWORD")
    return myConfig.wifiPass;
  else if (key == "LED_BRIGHTNESS")
    return String(myConfig.ledBrightness);
  else if (key == "LED_COLOR")
    return myConfig.ledColor;
  else if (key == "NTP_SERVER")
    return myConfig.ntpServer;
  else if (key == "CURRENT_TIME")
    return getFormattedTime(localTimeInfo);
  else if (key == "NTP_LAST_UPDATE")
    return getFormattedDateTime(localtime(&ntpLastUpdate));

  return "Key not found";
}

void initWeb()
{
  server.on("/", []()
            { templateProcessor.processAndSend("/index.html", indexKeyProcessor); });
  server.on("/bootstrap.min.css", []()
            {
              File file = SPIFFS.open("/bootstrap.min.css", "r");
              server.streamFile(file, "text/css");
              file.close();
            });
  server.on("/reboot", []()
            {
              server.sendHeader("Location", String("/"), true);
              server.send(302, "text/plain", "");
              rebootTimer = 1;
            });
  server.on("/setting", []()
            {
              bool updateEEPROM = false;
              if (server.hasArg("ssid") && server.hasArg("ssid_pwd") && (myConfig.wifiSsid != server.arg("ssid") || myConfig.wifiPass != server.arg("ssid_pwd")))
              {
                debugLog(String("update ssid: ") + server.arg("ssid") + String(", pass: ") + server.arg("ssid_pwd"));
                myConfig.wifiSsid = server.arg("ssid");
                myConfig.wifiPass = server.arg("ssid_pwd");
                reconnectWifiTimer = 1;
                updateEEPROM = true;
              }
              if (server.hasArg("led_brightness") && myConfig.ledBrightness != server.arg("led_brightness").toInt())
              {
                debugLog(String("update led brightness: ") + server.arg("led_brightness"));
                myConfig.ledBrightness = server.arg("led_brightness").toInt();
                FastLED.setBrightness(myConfig.ledBrightness);
                updateEEPROM = true;
              }
              if (server.hasArg("led_color") && myConfig.ledColor != server.arg("led_color"))
              {
                myConfig.ledColor = server.arg("led_color");
                colorhex = strtol(myConfig.ledColor.substring(1).c_str(), NULL, 16);
                updateEEPROM = true;
              }
              if (server.hasArg("ntp_server") && myConfig.ntpServer != server.arg("ntp_server"))
              {
                myConfig.ntpServer = server.arg("ntp_server");
                configTime(MYTZ, server.arg("ntp_server").c_str());
                updateEEPROM = true;
              }
              if (updateEEPROM)
              {
                debugLog("[http] writing eeprom");
                saveConfigToEEPROM();
              }
              else
              {
                debugLog("[http] eeprom not change");
              }
              server.sendHeader("Location", String("/"), true);
              server.send(302, "text/plain", "");
            });

  server.begin();

  debugLog("[init Web] HTTP server started");
}

String getFormattedTime(const tm *tm)
{
  char result[8];
  os_sprintf(result, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
  return String(result);
}

String getFormattedDateTime(const tm *tm)
{
  char result[19];
  os_sprintf(result, "%02d/%02d/%02d %02d:%02d:%02d",
             1900 + tm->tm_year, 1 + tm->tm_mon, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
  return String(result);
}

/*
  EEPROM helper
*/
void loadConfigFromEEPROM()
{
  if (reset_eeprom)
  {
    saveConfigToEEPROM();
  }
  else
  {
    myConfig.ledBrightness = EEPROM.read(0);
    int offset = 1;
    offset = readStringFromEEPROM(offset, &myConfig.ledColor);
    offset = readStringFromEEPROM(offset, &myConfig.wifiSsid);
    offset = readStringFromEEPROM(offset, &myConfig.wifiPass);
    offset = readStringFromEEPROM(offset, &myConfig.ntpServer);
    colorhex = strtol(myConfig.ledColor.substring(1).c_str(), NULL, 16);
  }
}

void saveConfigToEEPROM()
{
  EEPROM.write(0, myConfig.ledBrightness);
  int offset = 1;
  offset = writeStringToEEPROM(offset, myConfig.ledColor);
  offset = writeStringToEEPROM(offset, myConfig.wifiSsid);
  offset = writeStringToEEPROM(offset, myConfig.wifiPass);
  offset = writeStringToEEPROM(offset, myConfig.ntpServer);
  EEPROM.commit();
}

int writeStringToEEPROM(int offset, const String &strToWrite)
{
  byte len = strToWrite.length();
  EEPROM.write(offset, len);

  for (int i = 0; i < len; i++)
  {
    EEPROM.write(offset + 1 + i, strToWrite[i]);
  }

  return offset + 1 + len;
}

int readStringFromEEPROM(int offset, String *strToRead)
{
  int strLen = EEPROM.read(offset);
  char data[strLen + 1];

  for (int i = 0; i < strLen; i++)
  {
    data[i] = EEPROM.read(offset + 1 + i);
  }
  data[strLen] = '\0';

  *strToRead = String(data);
  return offset + 1 + strLen;
}

/*
  debug helper
*/
void debugLog(String writeSomething)
{
  if (debug)
    Serial.println(writeSomething);
}
