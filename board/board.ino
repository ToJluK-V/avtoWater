#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include "secrets.h"

// Увеличивайте при каждой новой сборке, которую нужно раздать по воздуху.
#define FW_VERSION 1

// =====================================================
// ПИНЫ
// =====================================================

#define ONE_WIRE_BUS 14   // GPIO14 — DS18B20
#define SOIL_POWER   5    // GPIO5 — питание датчика почвы
#define SOIL_PIN     A0
#define PUMP_PIN     4    // GPIO4 — управление помпой

// GPIO16 (D0) соединить с RST для пробуждения.

// =====================================================
// ЖЁСТКИЕ ПРЕДЕЛЫ (защита от неверных настроек с сервера)
// =====================================================

const uint32_t WIFI_TIMEOUT_MS = 15000;
// Бесплатный Render просыпается около минуты.
const uint32_t HTTP_TIMEOUT_MS = 70000;

const int32_t MAX_PUMP_MS = 30000;
const int32_t MAX_SLEEP_MIN = 180;   // предел deepSleep ESP8266 около 3,5 ч

// =====================================================
// НАСТРОЙКИ (приходят с сервера, кэшируются в EEPROM)
// =====================================================

struct Config {
  uint32_t magic;
  int32_t threshold;
  int32_t pumpMs;
  int32_t sleepMin;
  int32_t minWateringSec;
  int32_t invert;          // 1: сухо, когда ADC выше порога
};

const uint32_t CONFIG_MAGIC = 0xA17A2027;

Config cfg;

void setDefaults() {
  cfg.magic = CONFIG_MAGIC;
  cfg.threshold = 150;
  cfg.pumpMs = 5000;
  cfg.sleepMin = 30;
  cfg.minWateringSec = 6L * 60L * 60L;
  cfg.invert = 0;
}

void clampConfig() {
  cfg.threshold = constrain(cfg.threshold, 1, 1023);
  cfg.pumpMs = constrain(cfg.pumpMs, 500, MAX_PUMP_MS);
  cfg.sleepMin = constrain(cfg.sleepMin, 1, MAX_SLEEP_MIN);
  cfg.minWateringSec = constrain(cfg.minWateringSec, 60, 7L * 24L * 3600L);
  cfg.invert = cfg.invert ? 1 : 0;
}

void loadConfig() {
  EEPROM.begin(64);
  EEPROM.get(0, cfg);

  if (cfg.magic != CONFIG_MAGIC) {
    setDefaults();
  }

  clampConfig();
}

void saveConfigIfChanged(const Config& before) {
  if (memcmp(&before, &cfg, sizeof(Config)) == 0) {
    return;
  }

  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// =====================================================
// СЧЁТЧИК ВРЕМЕНИ С ПОСЛЕДНЕГО ПОЛИВА (RTC-память)
// Часы и NTP не нужны. После полной потери питания
// счётчик обнуляется, поэтому полив ждёт полный интервал.
// =====================================================

struct RtcData {
  uint32_t magic;
  uint32_t secondsSinceWatering;
};

const uint32_t RTC_MAGIC = 0xC0FFEE01;
const uint32_t RTC_OFFSET = 0;

RtcData rtc;

void loadRtc() {
  ESP.rtcUserMemoryRead(RTC_OFFSET, (uint32_t*)&rtc, sizeof(rtc));

  if (rtc.magic != RTC_MAGIC) {
    rtc.magic = RTC_MAGIC;
    rtc.secondsSinceWatering = 0;
  }
}

void saveRtc() {
  ESP.rtcUserMemoryWrite(RTC_OFFSET, (uint32_t*)&rtc, sizeof(rtc));
}

// =====================================================
// СОСТОЯНИЕ ЦИКЛА
// =====================================================

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

float temperature = DEVICE_DISCONNECTED_C;
int soilValue = -1;
int postWaterSoilValue = -1;
bool watered = false;
long remoteFwVersion = 0;

// =====================================================
// ДАТЧИКИ
// =====================================================

int readSoilMoisture() {
  digitalWrite(SOIL_POWER, LOW);
  delay(5);

  digitalWrite(SOIL_POWER, HIGH);
  delay(30);

  int value = analogRead(SOIL_PIN);

  digitalWrite(SOIL_POWER, LOW);

  return value;
}

void readTemperature() {
  sensors.requestTemperatures();
  delay(800);

  temperature = sensors.getTempCByIndex(0);

  if (temperature == 85.0 ||
      temperature == DEVICE_DISCONNECTED_C ||
      temperature < -55.0 ||
      temperature > 125.0) {
    Serial.println("Ошибка измерения DS18B20!");
    temperature = DEVICE_DISCONNECTED_C;
  }
}

// =====================================================
// ПОМПА
// =====================================================

void pumpOff() {
  digitalWrite(PUMP_PIN, LOW);
}

bool soilIsDry() {
  if (soilValue < 0) {
    return false;
  }

  return cfg.invert ? soilValue > cfg.threshold
                    : soilValue < cfg.threshold;
}

void waterPlant() {
  if (!soilIsDry()) {
    return;
  }

  if (rtc.secondsSinceWatering < (uint32_t)cfg.minWateringSec) {
    Serial.println("Полив пропущен: интервал не прошёл.");
    return;
  }

  Serial.println("Почва сухая. Начинаю полив.");

  // Сбрасываем счётчик до включения помпы: после сбоя питания
  // полив не повторится сразу.
  rtc.secondsSinceWatering = 0;
  saveRtc();

  watered = true;

  digitalWrite(PUMP_PIN, HIGH);
  delay(cfg.pumpMs);
  pumpOff();

  delay(1500);

  postWaterSoilValue = readSoilMoisture();
}

// =====================================================
// СЕРВЕР
// =====================================================

bool jsonInt(const String& body, const char* key, long& out) {
  String needle = String("\"") + key + "\":";
  int i = body.indexOf(needle);

  if (i < 0) {
    return false;
  }

  out = body.substring(i + needle.length()).toInt();
  return true;
}

bool fetchConfig() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 512);

  HTTPClient http;
  String url = String("https://") + SERVER_HOST +
               "/api/v1/config?deviceId=" + DEVICE_ID;

  if (!http.begin(client, url)) {
    return false;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("X-Device-Key", DEVICE_KEY);

  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.print("Конфиг: HTTP ");
    Serial.println(code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  Config before = cfg;
  long v;

  if (jsonInt(body, "threshold", v)) cfg.threshold = v;
  if (jsonInt(body, "pumpMs", v)) cfg.pumpMs = v;
  if (jsonInt(body, "sleepMin", v)) cfg.sleepMin = v;
  if (jsonInt(body, "minWateringSec", v)) cfg.minWateringSec = v;
  if (jsonInt(body, "invert", v)) cfg.invert = v;
  if (jsonInt(body, "fwVersion", v)) remoteFwVersion = v;

  clampConfig();
  saveConfigIfChanged(before);

  return true;
}

void checkFirmwareUpdate() {
  if (remoteFwVersion <= FW_VERSION) {
    return;
  }

  Serial.println("Найдена новая прошивка, обновляюсь.");

  WiFiClientSecure client;
  client.setInsecure();

  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(true);
  ESPhttpUpdate.update(
    client,
    String("https://") + SERVER_HOST + "/firmware.bin?key=" + DEVICE_KEY
  );
}

void sendReport() {
  String json = String("{\"deviceId\":\"") + DEVICE_ID + "\"";

  json += ",\"temperature\":";
  json += (temperature == DEVICE_DISCONNECTED_C) ? String("null")
                                                  : String(temperature, 1);
  json += ",\"soil\":" + String(soilValue);
  json += ",\"postWaterSoil\":";
  json += (postWaterSoilValue >= 0) ? String(postWaterSoilValue)
                                    : String("null");
  json += String(",\"watered\":") + (watered ? "true" : "false");
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"fw\":" + String(FW_VERSION);
  json += "}";

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 512);

  HTTPClient http;

  if (!http.begin(client, String("https://") + SERVER_HOST + "/api/v1/report")) {
    return;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("X-Device-Key", DEVICE_KEY);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(json);

  Serial.print("Отчёт: HTTP ");
  Serial.println(code);

  http.end();
}

// =====================================================
// SETUP — ОДИН ЦИКЛ НА КАЖДОЕ ПРОБУЖДЕНИЕ
// =====================================================

void setup() {
  Serial.begin(115200);

  digitalWrite(PUMP_PIN, LOW);
  pinMode(PUMP_PIN, OUTPUT);
  pumpOff();

  digitalWrite(SOIL_POWER, LOW);
  pinMode(SOIL_POWER, OUTPUT);

  loadConfig();
  loadRtc();

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(true);

  readTemperature();
  soilValue = readSoilMoisture();

  Serial.print("Температура: ");
  Serial.println(temperature);
  Serial.print("Влажность ADC: ");
  Serial.println(soilValue);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(100);
  }

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    // Без ответа сервера работаем по сохранённым настройкам.
    fetchConfig();
    checkFirmwareUpdate();
  }

  waterPlant();

  if (wifiConnected) {
    sendReport();
  }

  pumpOff();
  digitalWrite(SOIL_POWER, LOW);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  uint64_t sleepSec = (uint64_t)cfg.sleepMin * 60ULL;

  rtc.secondsSinceWatering += (uint32_t)sleepSec + millis() / 1000;
  saveRtc();

  Serial.print("Deep Sleep, минут: ");
  Serial.println(cfg.sleepMin);
  Serial.flush();

  ESP.deepSleep(sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
}

void loop() {
  // Не используется: после каждого пробуждения выполняется setup().
}
