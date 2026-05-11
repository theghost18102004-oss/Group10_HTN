#include <WiFi.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include <time.h>

// ================= CONFIG =================
const char* WIFI_SSID = "AnhNhan";
const char* WIFI_PASSWORD = "1234554321";

const char *mqtt_broker = "broker.emqx.io";
const char *topic = "tan123";
const char *mqtt_username = "tan123";
const char *mqtt_password = "123tan";
const int mqtt_port = 1883;

// TFT
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// PIN
#define LIGHT_PIN   32
#define GAS_PIN     34
#define BUZZER_PIN  25
#define LED_PIN     26

// Weather
const char* WEATHER_URL = "http://api.openweathermap.org/data/2.5/weather?q=Da%20Nang,vn&appid=e52424da53629896219c650285865edc&units=metric";

// RTOS
SemaphoreHandle_t xMutexData = NULL;
QueueHandle_t xSensorQueue = NULL;
TimerHandle_t xPublishTimer = NULL;

// DATA
typedef struct {
  float temperature;
  int gasPercent;
  bool gasAlert;
  bool ledState;
  int alarmHour;
  int alarmMinute;
  bool alarmEnabled;
} SystemData_t;

SystemData_t sysData = {NAN, 0, false, false, 7, 0, false};

const int GAS_THRESHOLD = 30;

// MQTT
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  SPI.begin(18, -1, 23, 5);
  
  tft.init(240, 320);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_WHITE);

  pinMode(LIGHT_PIN, INPUT);
  pinMode(GAS_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  // RTOS Objects
  xMutexData = xSemaphoreCreateMutex();
  xSensorQueue = xQueueCreate(10, sizeof(SystemData_t));

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  // Time
  configTime(7 * 3600, 0, "time.google.com");
  struct tm timeinfo;
  Serial.print("Sync time");
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nTime OK");

  // MQTT
  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  // Software Timer - Publish mỗi 1 giây
  xPublishTimer = xTimerCreate("PublishTimer", pdMS_TO_TICKS(1000), pdTRUE, 0, vPublishTimerCallback);
  xTimerStart(xPublishTimer, 0);

  // Tasks
  xTaskCreate(TaskSensors, "Sensors", 8192, NULL, 4, NULL);
  xTaskCreate(TaskWeather, "Weather", 8192, NULL, 3, NULL);
  xTaskCreate(TaskDisplay, "Display", 4096, NULL, 2, NULL);
  xTaskCreate(TaskAlarm, "Alarm", 4096, NULL, 2, NULL);
}

// ================= SOFTWARE TIMER CALLBACK =================
void vPublishTimerCallback(TimerHandle_t xTimer) {
  publishData();
}

// ================= MQTT =================
void mqttCallback(char* receivedTopic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) message += (char)payload[i];
  Serial.println("Received MQTT: " + message);

  if (message == "LED_ON") digitalWrite(LED_PIN, HIGH);
  else if (message == "LED_OFF") digitalWrite(LED_PIN, LOW);
  else if (message.startsWith("ALARM_SET")) {
    int h, m;
    if (sscanf(message.c_str(), "ALARM_SET %d:%d", &h, &m) == 2) {
      xSemaphoreTake(xMutexData, portMAX_DELAY);
      sysData.alarmHour = h; 
      sysData.alarmMinute = m; 
      sysData.alarmEnabled = true;
      xSemaphoreGive(xMutexData);
    }
  } 
  else if (message == "ALARM_CLEAR") {
    xSemaphoreTake(xMutexData, portMAX_DELAY);
    sysData.alarmEnabled = false;
    xSemaphoreGive(xMutexData);
  }
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str(), mqtt_username, mqtt_password)) {
      mqttClient.subscribe(topic);
    } else {
      delay(5000);
    }
  }
}

void publishData() {
  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  JsonDocument doc;
  xSemaphoreTake(xMutexData, portMAX_DELAY);
  doc["temp"] = isnan(sysData.temperature) ? 0 : round(sysData.temperature * 10) / 10.0;
  doc["gas"] = sysData.gasPercent;
  doc["led"] = sysData.ledState ? "ON" : "OFF";
  
  char alarmStr[6];
  snprintf(alarmStr, sizeof(alarmStr), "%02d:%02d", sysData.alarmHour, sysData.alarmMinute);
  doc["alarm"] = sysData.alarmEnabled ? alarmStr : "NONE";
  xSemaphoreGive(xMutexData);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char datetime[20];
    snprintf(datetime, sizeof(datetime), "%02d/%02d/%04d %02d:%02d:%02d",
             timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    doc["datetime"] = datetime;
  }

  String jsonString;
  serializeJson(doc, jsonString);
  mqttClient.publish(topic, jsonString.c_str());
}

// ================= TASK SENSORS =================
void TaskSensors(void *pvParameters) {
  SystemData_t sensorData;

  for (;;) {
    int lightValue = analogRead(LIGHT_PIN);
    int gasADC = analogRead(GAS_PIN);
    int gasPercent = map(gasADC, 0, 4095, 0, 100);
    bool isDark = (lightValue > 1800);

    sensorData.gasPercent = constrain(gasPercent, 0, 100);
    sensorData.gasAlert = (gasPercent >= GAS_THRESHOLD);
    sensorData.ledState = isDark;

    xQueueSend(xSensorQueue, &sensorData, pdMS_TO_TICKS(100));

    digitalWrite(LED_PIN, isDark ? HIGH : LOW);
    digitalWrite(BUZZER_PIN, sensorData.gasAlert ? HIGH : LOW);

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ================= TASK DISPLAY =================
void TaskDisplay(void *pvParameters) {
  SystemData_t receivedData;

  for (;;) {
    if (xQueueReceive(xSensorQueue, &receivedData, pdMS_TO_TICKS(200)) == pdPASS) {
      xSemaphoreTake(xMutexData, portMAX_DELAY);
      sysData.gasPercent = receivedData.gasPercent;
      sysData.gasAlert = receivedData.gasAlert;
      sysData.ledState = receivedData.ledState;
      xSemaphoreGive(xMutexData);
    }

    struct tm timeinfo;
    char timeStr[9] = "--:--:--";
    char dateStr[12] = "--/--/----";

    if (getLocalTime(&timeinfo)) {
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", 
               timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d", 
               timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
    }

    xSemaphoreTake(xMutexData, portMAX_DELAY);
    tft.fillScreen(ST77XX_WHITE);
    int centerX = 160;

    tft.setTextSize(4);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(centerX - 95, 18);
    tft.print(timeStr);

    tft.setTextSize(2);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(centerX - 60, 75);
    tft.print(dateStr);

    tft.drawFastHLine(30, 105, 260, 0x8410);

    tft.setTextSize(2);
    tft.setTextColor(0xFB80);
    tft.setCursor(35, 125);
    if (!isnan(sysData.temperature))
      tft.printf("Nhiet do: %.1f C", sysData.temperature);
    else
      tft.print("Nhiet do: -- C");

    tft.setTextColor(ST77XX_RED);
    tft.setCursor(35, 155);
    tft.printf("Gas: %d%%", sysData.gasPercent);

    tft.setTextColor(sysData.ledState ? ST77XX_RED : 0x03E0);
    tft.setCursor(35, 185);
    tft.printf("Den: %s", sysData.ledState ? "TOI - ON" : "SANG - OFF");

    tft.setTextColor(ST77XX_MAGENTA);
    tft.setCursor(35, 215);
    if (sysData.alarmEnabled)
      tft.printf("Bao thuc: %02d:%02d", sysData.alarmHour, sysData.alarmMinute);
    else
      tft.print("Bao thuc: --:--");

    xSemaphoreGive(xMutexData);

    vTaskDelay(800 / portTICK_PERIOD_MS);
  }
}

// ================= TASK WEATHER =================
void TaskWeather(void *pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(WEATHER_URL);
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
          float temp = doc["main"]["temp"];
          xSemaphoreTake(xMutexData, portMAX_DELAY);
          sysData.temperature = temp;
          xSemaphoreGive(xMutexData);
        }
      }
      http.end();
    }
    vTaskDelay(10 * 60 * 1000 / portTICK_PERIOD_MS); // 10 phút
  }
}

// ================= TASK ALARM =================
void TaskAlarm(void *pvParameters) {
  for (;;) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      xSemaphoreTake(xMutexData, portMAX_DELAY);
      bool shouldAlarm = sysData.alarmEnabled &&
                         timeinfo.tm_hour == sysData.alarmHour &&
                         timeinfo.tm_min == sysData.alarmMinute &&
                         timeinfo.tm_sec < 10;
      xSemaphoreGive(xMutexData);

      if (shouldAlarm) {
        for (int i = 0; i < 8; i++) {
          digitalWrite(BUZZER_PIN, HIGH); delay(400);
          digitalWrite(BUZZER_PIN, LOW);  delay(300);
        }
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ================= LOOP =================
void loop() {
  vTaskDelay(portMAX_DELAY);
}
