
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <UniversalTelegramBot.h>
#include <ThingSpeak.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
#include <stdlib.h>
#include <time.h>

// =====================================================
// НАСТРОЙКИ 
// =====================================================

const char* ssid = "ZTE11_plus";
const char* password = "998202222";

const char* botToken = "8820933851:AAE1Y7nfhdOlJjvv1fwwH1MRtDMEXKH_kQA";

// ID группы или канала, куда бот отправляет статус.
// Бот должен иметь право публиковать и редактировать сообщения.
const char* channelId = "-1004266229903";

unsigned long tsChannelNumber = 3553830;
const char* tsApiKey = "NYI6KXFOJV8TX4VF";

// =====================================================
// ПИНЫ
// =====================================================

#define ONE_WIRE_BUS 14   // GPIO14 — DS18B20
#define SOIL_POWER   5    // GPIO5 — питание датчика почвы
#define SOIL_PIN     A0
#define PUMP_PIN     4    // GPIO4 — управление помпой

// GPIO16 (D0) соединить с RST для пробуждения.

// =====================================================
// ЭНЕРГОСБЕРЕЖЕНИЕ И ПОЛИВ
// =====================================================

const uint64_t SLEEP_US = 30ULL * 60ULL * 1000000ULL;

const uint32_t THINGSPEAK_INTERVAL = 30UL * 60UL;
const uint32_t MIN_WATERING_INTERVAL = 6UL * 60UL * 60UL;
const uint8_t CHART_RESULTS = 48;

const uint32_t WIFI_TIMEOUT_MS = 15000;
const uint32_t NTP_TIMEOUT_MS = 8000;

const unsigned long PUMP_TIME_MS = 5000;
const int DEFAULT_THRESHOLD = 150;

// =====================================================
// ОБЪЕКТЫ
// =====================================================

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

WiFiClientSecure telegramClient;
WiFiClient tsClient;

UniversalTelegramBot bot(botToken, telegramClient);

// =====================================================
// СОХРАНЯЕМЫЕ НАСТРОЙКИ
// =====================================================

struct SavedData {
  uint32_t magic;
  int32_t threshold;
  int32_t telegramMessageId;
  uint32_t lastWateringEpoch;
  uint32_t lastThingSpeakEpoch;
};

const uint32_t EEPROM_MAGIC = 0xA17A2026;

SavedData saved;

int DRY_THRESHOLD = DEFAULT_THRESHOLD;

uint32_t currentEpoch = 0;

float temperature = DEVICE_DISCONNECTED_C;
int soilValue = -1;
int postWaterSoilValue = -1;

bool wateringThisCycle = false;
bool wifiConnected = false;

// =====================================================
// EEPROM
// =====================================================

void saveData() {
  saved.magic = EEPROM_MAGIC;
  saved.threshold = DRY_THRESHOLD;

  EEPROM.put(0, saved);
  EEPROM.commit();
}

void loadData() {
  EEPROM.begin(128);
  EEPROM.get(0, saved);

  if (saved.magic != EEPROM_MAGIC ||
      saved.threshold < 1 ||
      saved.threshold > 1023) {

    saved.magic = EEPROM_MAGIC;
    saved.threshold = DEFAULT_THRESHOLD;
    saved.telegramMessageId = 0;
    saved.lastWateringEpoch = 0;
    saved.lastThingSpeakEpoch = 0;

    DRY_THRESHOLD = DEFAULT_THRESHOLD;
    saveData();
  } else {
    DRY_THRESHOLD = saved.threshold;
  }
}

// =====================================================
// ДАТЧИК ВЛАЖНОСТИ
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

// =====================================================
// ВРЕМЯ
// =====================================================

bool syncTime() {
  configTime(3 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");

  unsigned long start = millis();

  while (millis() - start < NTP_TIMEOUT_MS) {
    time_t now = time(nullptr);

    if (now > 1700000000) {
      currentEpoch = (uint32_t)now;
      return true;
    }

    delay(100);
  }

  currentEpoch = 0;
  return false;
}

String formatTime(uint32_t epoch) {
  if (epoch == 0) {
    return "нет данных";
  }

  time_t raw = (time_t)epoch;
  struct tm timeinfo;

  localtime_r(&raw, &timeinfo);

  char buffer[24];

  strftime(
    buffer,
    sizeof(buffer),
    "%d.%m.%Y %H:%M UTC+3",
    &timeinfo
  );

  return String(buffer);
}

// =====================================================
// БЕЗОПАСНОЕ УПРАВЛЕНИЕ ПОМПОЙ
// =====================================================

void pumpOff() {
  digitalWrite(PUMP_PIN, LOW);
}

bool wateringAllowed() {
  // Без точного времени не включаем помпу:
  // иначе нельзя гарантировать минимальный интервал.
  if (currentEpoch == 0) {
    return false;
  }

  if (saved.lastWateringEpoch == 0) {
    return true;
  }

  if (currentEpoch < saved.lastWateringEpoch) {
    return false;
  }

  return (currentEpoch - saved.lastWateringEpoch)
         >= MIN_WATERING_INTERVAL;
}

void waterPlant() {
  if (soilValue >= DRY_THRESHOLD) {
    return;
  }

  if (!wateringAllowed()) {
    Serial.println("Полив пропущен: интервал или время.");
    return;
  }

  Serial.println("Почва сухая. Начинаю полив.");

  // Сначала сохраняем отметку, чтобы после сброса
  // питания полив не запустился немедленно повторно.
  saved.lastWateringEpoch = currentEpoch;
  saveData();

  wateringThisCycle = true;

  digitalWrite(PUMP_PIN, HIGH);
  delay(PUMP_TIME_MS);
  pumpOff();

  // Даем воде впитаться.
  delay(1500);

  postWaterSoilValue = readSoilMoisture();

  Serial.print("После полива: ");
  Serial.println(postWaterSoilValue);
}

// =====================================================
// TELEGRAM — ОДНО РЕДАКТИРУЕМОЕ СООБЩЕНИЕ
// =====================================================

String buildStatus() {
  String msg;

  msg += "🌱 АВТОПОЛИВ\n\n";

  msg += "🌡 Температура: ";

  if (temperature == DEVICE_DISCONNECTED_C) {
    msg += "ошибка датчика\n";
  } else {
    msg += String(temperature, 1) + " °C\n";
  }

  msg += "💧 Влажность (ADC): ";

  if (soilValue < 0) {
    msg += "ошибка\n";
  } else {
    msg += String(soilValue) + "\n";
  }

  msg += "🎯 Порог полива: ";
  msg += String(DRY_THRESHOLD) + "\n\n";

  msg += "🪴 Последний полив: ";
  msg += formatTime(saved.lastWateringEpoch) + "\n";

  msg += "⏱ Длительность: ";

  if (saved.lastWateringEpoch == 0) {
    msg += "нет данных\n";
  } else {
    msg += "до 5 секунд\n";
  }

  if (wateringThisCycle && postWaterSoilValue >= 0) {
    msg += "🌱 После полива (ADC): ";
    msg += String(postWaterSoilValue) + "\n";
  }

  msg += "\n🕒 Обновление: ";
  msg += formatTime(currentEpoch) + "\n";

  if (soilValue >= 0 && soilValue < DRY_THRESHOLD) {
    msg += "⚠️ Почва сухая";
  } else if (soilValue >= 0) {
    msg += "✅ Влажность выше порога";
  } else {
    msg += "⚠️ Нет показаний датчика";
  }

  return msg;
}

void updateTelegram() {
  if (!wifiConnected) {
    return;
  }

  String status = buildStatus();

  bool success = false;

  if (saved.telegramMessageId > 0) {
    // Четвертый аргумент sendMessage в библиотеке
    // UniversalTelegramBot — ID редактируемого сообщения.
    success = bot.sendMessage(
      channelId,
      status,
      "",
      saved.telegramMessageId
    );
  }

  if (!success) {
    // Первое сообщение или восстановление, если
    // старое было удалено либо недоступно.
    if (bot.sendMessage(channelId, status, "")) {
      int newMessageId = bot.last_sent_message_id;

      if (newMessageId > 0) {
        saved.telegramMessageId = newMessageId;
        saveData();
      }
    } else {
      Serial.println("Не удалось обновить Telegram.");
    }
  }
}

// =====================================================
// THINGSPEAK
// =====================================================

bool readThingSpeakHistory(
  float temperatures[],
  int soilValues[],
  uint8_t& count
) {
  HTTPClient http;
  String url = "http://api.thingspeak.com/channels/";
  url += String(tsChannelNumber);
  url += "/feeds.csv?results=";
  url += String(CHART_RESULTS);

  if (!http.begin(tsClient, url)) {
    Serial.println("Не удалось открыть ThingSpeak для чтения графика.");
    return false;
  }

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("Ошибка чтения истории ThingSpeak: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  String csv = http.getString();
  http.end();

  count = 0;
  int rowStart = 0;

  while (rowStart < csv.length() && count < CHART_RESULTS) {
    int rowEnd = csv.indexOf('\n', rowStart);

    if (rowEnd < 0) {
      rowEnd = csv.length();
    }

    String row = csv.substring(rowStart, rowEnd);
    row.trim();
    rowStart = rowEnd + 1;

    int firstComma = row.indexOf(',');
    int secondComma = row.indexOf(',', firstComma + 1);
    int thirdComma = row.indexOf(',', secondComma + 1);

    if (firstComma < 0 || secondComma < 0 || thirdComma < 0) {
      continue;
    }

    int fourthComma = row.indexOf(',', thirdComma + 1);
    String temperatureText = row.substring(secondComma + 1, thirdComma);
    String soilText = fourthComma < 0
      ? row.substring(thirdComma + 1)
      : row.substring(thirdComma + 1, fourthComma);

    temperatureText.trim();
    soilText.trim();

    if (temperatureText == "field1" || soilText == "field2" ||
        temperatureText.length() == 0 || soilText.length() == 0) {
      continue;
    }

    const char* temperatureStart = temperatureText.c_str();
    char* temperatureEnd;
    float rowTemperature = strtof(temperatureStart, &temperatureEnd);

    const char* soilStart = soilText.c_str();
    char* soilEnd;
    long parsedSoilValue = strtol(soilStart, &soilEnd, 10);

    if (temperatureEnd == temperatureStart || *temperatureEnd != '\0' ||
        soilEnd == soilStart || *soilEnd != '\0' ||
        !(rowTemperature >= -55.0 && rowTemperature <= 125.0) ||
        parsedSoilValue < 0 || parsedSoilValue > 1023) {
      continue;
    }

    temperatures[count] = rowTemperature;
    soilValues[count] = static_cast<int>(parsedSoilValue);
    count++;
  }

  if (count < 2) {
    Serial.println("Недостаточно данных ThingSpeak для построения графиков.");
    return false;
  }

  return true;
}

String encodeUrlParameter(const String& value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);

  for (size_t i = 0; i < value.length(); i++) {
    uint8_t c = static_cast<uint8_t>(value[i]);

    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

String buildQuickChartUrl(
  const float temperatures[],
  const int soilValues[],
  uint8_t count,
  bool temperatureChart
) {
  String config;
  config.reserve(1200);
  config = "{\"type\":\"line\",\"data\":{\"labels\":[";

  for (uint8_t i = 0; i < count; i++) {
    if (i > 0) {
      config += ',';
    }
    config += String(i + 1);
  }

  config += "],\"datasets\":[{\"label\":\"";
  config += temperatureChart ? "Temperature (C)" : "Soil moisture (ADC)";
  config += "\",\"data\":[";

  for (uint8_t i = 0; i < count; i++) {
    if (i > 0) {
      config += ',';
    }
    config += temperatureChart
      ? String(temperatures[i], 1)
      : String(soilValues[i]);
  }

  config += temperatureChart
    ? "],\"borderColor\":\"#e74c3c\",\"fill\":false,\"pointRadius\":2}]},"
      "\"options\":{\"plugins\":{\"legend\":{\"display\":false},"
      "\"title\":{\"display\":true,\"text\":\"Temperature (C)\"}},"
      "\"scales\":{\"y\":{\"beginAtZero\":false}}}}"
    : "],\"borderColor\":\"#3498db\",\"fill\":false,\"pointRadius\":2}]},"
      "\"options\":{\"plugins\":{\"legend\":{\"display\":false},"
      "\"title\":{\"display\":true,\"text\":\"Soil moisture (ADC)\"}},"
      "\"scales\":{\"y\":{\"beginAtZero\":false}}}}";

  String url = "https://quickchart.io/chart?width=640&height=360&format=png&version=3&c=";
  url += encodeUrlParameter(config);
  return url;
}

bool sendThingSpeakCharts() {
  float temperatures[CHART_RESULTS];
  int soilValues[CHART_RESULTS];
  uint8_t count = 0;

  if (!readThingSpeakHistory(temperatures, soilValues, count)) {
    return false;
  }

  String temperatureUrl =
    buildQuickChartUrl(temperatures, soilValues, count, true);
  String response = bot.sendPhoto(
    channelId,
    temperatureUrl,
    "Температура — последние измерения"
  );
  bool temperatureSent = bot.checkForOkResponse(response);

  if (!temperatureSent) {
    Serial.print("Не удалось отправить график температуры: ");
    Serial.println(response);
  }

  String soilUrl =
    buildQuickChartUrl(temperatures, soilValues, count, false);
  response = bot.sendPhoto(
    channelId,
    soilUrl,
    "Влажность почвы — последние измерения"
  );
  bool soilSent = bot.checkForOkResponse(response);

  if (!soilSent) {
    Serial.print("Не удалось отправить график влажности: ");
    Serial.println(response);
  }

  return temperatureSent && soilSent;
}

void updateThingSpeak() {
  Serial.println("=== ThingSpeak ===");

  if (!wifiConnected) {
    Serial.println("Ошибка: Wi-Fi не подключен.");
    return;
  }

  if (currentEpoch == 0) {
    Serial.println("Ошибка: время NTP не получено.");
    return;
  }

  if (temperature == DEVICE_DISCONNECTED_C ||
      temperature == 85.0 ||
      temperature < -55.0 ||
      temperature > 125.0) {
    Serial.println("Ошибка: неверная температура DS18B20.");
    return;
  }

  if (soilValue < 0 || soilValue > 1023) {
    Serial.println("Ошибка: неверное значение датчика почвы.");
    return;
  }

  if (saved.lastThingSpeakEpoch != 0) {
    if (currentEpoch < saved.lastThingSpeakEpoch) {
      Serial.println("Ошибка: время меньше времени предыдущей отправки.");
      return;
    }

    uint32_t elapsed =
        currentEpoch - saved.lastThingSpeakEpoch;

    Serial.print("С последней отправки прошло, секунд: ");
    Serial.println(elapsed);

    if (elapsed < THINGSPEAK_INTERVAL) {
      Serial.println("Отправка пока не требуется.");
      return;
    }
  }

  ThingSpeak.setField(1, temperature);
  ThingSpeak.setField(2, soilValue);

  Serial.print("Отправляем температуру: ");
  Serial.println(temperature);

  Serial.print("Отправляем влажность ADC: ");
  Serial.println(soilValue);

  int response = ThingSpeak.writeFields(
    tsChannelNumber,
    tsApiKey
  );

  Serial.print("Ответ ThingSpeak: ");
  Serial.println(response);

  if (response == 200) {
    saved.lastThingSpeakEpoch = currentEpoch;
    saveData();

    Serial.println("Успех: данные записаны в ThingSpeak.");
    if (!sendThingSpeakCharts()) {
      Serial.println("Не удалось отправить все графики в Telegram.");
    }
  } else {
    Serial.println("Ошибка отправки. Следующая попытка");
    Serial.println("будет при следующем пробуждении.");
  }
}


// =====================================================
// SETUP — ОДИН ЦИКЛ НА КАЖДОЕ ПРОБУЖДЕНИЕ
// =====================================================

void setup() {
  Serial.begin(115200);

  // Сначала выключаем исполнительные устройства.
  digitalWrite(PUMP_PIN, LOW);
  pinMode(PUMP_PIN, OUTPUT);
  pumpOff();

  digitalWrite(SOIL_POWER, LOW);
  pinMode(SOIL_POWER, OUTPUT);

  loadData();

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(true);

  // Не ждем бесконечно подключения к Wi-Fi.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(ssid, password);

  unsigned long wifiStart = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(100);
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    telegramClient.setInsecure();

    ThingSpeak.begin(tsClient);

    syncTime();
  }
  
// Запрашиваем измерение температуры
sensors.requestTemperatures();

// Ждём завершения преобразования
delay(800);

// Только теперь читаем результат
temperature = sensors.getTempCByIndex(0);

// Проверяем результат
if (temperature == 85.0 ||
    temperature == DEVICE_DISCONNECTED_C ||
    temperature < -55.0 ||
    temperature > 125.0) {

  Serial.println("Ошибка измерения DS18B20!");
  temperature = DEVICE_DISCONNECTED_C;
}


  soilValue = readSoilMoisture();

  Serial.print("Температура: ");
  Serial.println(temperature);

  Serial.print("Влажность ADC: ");
  Serial.println(soilValue);

  // Полив возможен только при достоверном времени.
  // Это необходимо для соблюдения интервала между поливами.
  waterPlant();

  // После полива статус содержит контрольный замер.
  // Отправляем только одно обновление за цикл.
  updateTelegram();

  // Редкая отправка на графики.
  updateThingSpeak();

  // Перед сном обязательно выключаем помпу.
  pumpOff();
  digitalWrite(SOIL_POWER, LOW);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  telegramClient.stop();
  tsClient.stop();

  Serial.println("Переход в Deep Sleep на 30 минут.");

  Serial.flush();

  ESP.deepSleep(SLEEP_US, WAKE_RF_DEFAULT);
}

void loop() {
  // Не используется: после каждого пробуждения
  // выполняется setup(), затем снова Deep Sleep.
}
