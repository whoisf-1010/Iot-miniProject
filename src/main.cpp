// Smart Water Bottle — ESP8266 + HX711 load cell + WiFiManager + MQTT
// Pure State Machine Pattern:
//    NO_BOTTLE --(น้ำหนัก >= LIFT_THRESHOLD_G)--> SETTLING
//    SETTLING  --(ค่านิ่ง: ประเมินดื่ม/เติม)-----> IDLE
//    SETTLING  --(น้ำหนัก <  LIFT_THRESHOLD_G)--> NO_BOTTLE
//    IDLE      --(น้ำหนักเปลี่ยน > DISTURB_G)---> SETTLING
//    IDLE      --(น้ำหนัก <  LIFT_THRESHOLD_G)--> NO_BOTTLE

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          
#include <PubSubClient.h>         
#include <HX711.h>
#include <EEPROM.h>
#include <time.h>

// ======================= ตั้งค่า =======================
// ---------- MQTT Broker ----------
#define MQTT_HOST "broker.emqx.io"       
#define MQTT_PORT 1883
#define MQTT_USER ""                     
#define MQTT_PASS ""
#define TOPIC_BASE "smartbottle"   

// ---------- HX711 (GPIO: 4 = D2, 5 = D1 บน NodeMCU) ----------
#define HX711_DT_PIN  D2
#define HX711_SCK_PIN D1

// ---------- พารามิเตอร์ตรวจจับการดื่ม ----------
#define DEFAULT_SCALE        721.8f  
#define DEFAULT_BOTTLE_G     16.0f   
#define DEFAULT_GOAL_ML      2000
#define LIFT_THRESHOLD_G     17.0f   
#define MIN_CHANGE_ML        5       
#define STABLE_RANGE_G       3.0f    
#define PUBLISH_INTERVAL_MS  1000

// ---------- เวลาไทย (UTC+7) ----------
#define TZ_OFFSET_SEC (7 * 3600)

#ifndef DISTURB_G
#define DISTURB_G 15.0f   
#endif

// ======================= State =======================
enum State {
  NO_BOTTLE,   
  SETTLING,    
  IDLE         
};

State state = NO_BOTTLE;
float weight = 0;          
float baselineG = 0;       
bool  baselineValid = false;
float lastStableG = 0;     
uint32_t lastDrinkTs = 0;  

// ======================= EEPROM =======================
struct Settings {
  uint32_t magic;
  float    scaleFactor;
  int32_t  offset;
  float    bottleG;
  int32_t  goalMl;
  int32_t  todayMl;
  int32_t  day;     
};
static const uint32_t SETTINGS_MAGIC = 0x5B07713E;
Settings settings;

HX711 scale;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
unsigned long lastPublish = 0, lastMqttTry = 0;

struct PendingDaily { bool valid; char date[11]; int32_t totalMl; int32_t goalMl; };
PendingDaily pendingDaily = { false, "", 0, 0 };

// ======================= ตรวจว่าค่านิ่ง =======================
static const int WIN = 8;
float winBuf[WIN];
int   winCount = 0, winIdx = 0;

void winClear() { winCount = 0; winIdx = 0; }
void winPush(float v) {
  winBuf[winIdx] = v;
  winIdx = (winIdx + 1) % WIN;
  if (winCount < WIN) winCount++;
}
bool winStable(float &avg) {
  if (winCount < WIN) return false;
  float mn = winBuf[0], mx = winBuf[0], sum = 0;
  for (int i = 0; i < WIN; i++) {
    if (winBuf[i] < mn) mn = winBuf[i];
    if (winBuf[i] > mx) mx = winBuf[i];
    sum += winBuf[i];
  }
  if (mx - mn > STABLE_RANGE_G) return false;
  avg = sum / WIN;
  return true;
}

// ======================= Function Prototypes =======================
void keepConnected();
void checkNewDay();
void flushPendingDaily();
void printStatus();
void publishStatus();
void publishEvent(const char *topic, int ml);
void evaluateStable(float avgG);

void processNoBottleState();
void processSettlingState();
void processIdleState();

// ======================= EEPROM Functions =======================
void saveSettings() {
  EEPROM.put(0, settings);
  EEPROM.commit();
}

bool loadSettings() {
  EEPROM.get(0, settings);
  if (settings.magic == SETTINGS_MAGIC) return true;
  settings.magic       = SETTINGS_MAGIC;
  settings.scaleFactor = DEFAULT_SCALE;
  settings.offset      = 0;
  settings.bottleG     = DEFAULT_BOTTLE_G;
  settings.goalMl      = DEFAULT_GOAL_ML;
  settings.todayMl     = 0;
  settings.day         = 0;
  return false;
}

// ======================= MQTT publish =======================
const char *stateName() {
  switch (state) {
    case NO_BOTTLE: return "NO_BOTTLE";
    case SETTLING:  return "SETTLING";
    case IDLE:      return "IDLE";
  }
  return "?";
}

void publishAck(const char *msg) {
  if (mqtt.connected()) mqtt.publish(TOPIC_BASE "/ack", msg);
  Serial.printf("[ack] %s\n", msg);
}

void publishStatus() {
  lastPublish = millis();
  if (!mqtt.connected()) return;
  bool present = (state != NO_BOTTLE);
  float waterMl = present ? (weight - settings.bottleG) : 0;
  if (waterMl < 0) waterMl = 0;
  int percent = settings.goalMl > 0 ? (settings.todayMl * 100 / settings.goalMl) : 0;

  char buf[420];
  snprintf(buf, sizeof(buf),
          "{\"state\":\"%s\",\"weight_g\":%.1f,\"water_ml\":%.0f,\"bottle_present\":%s,"
          "\"today_ml\":%d,\"goal_ml\":%d,\"percent\":%d,"
          "\"last_drink_ts\":%lu,\"ts\":%lu,"
          "\"rssi\":%d,\"uptime_s\":%lu}",
          stateName(), weight, waterMl, present ? "true" : "false",
          (int)settings.todayMl, (int)settings.goalMl, percent,
          (unsigned long)lastDrinkTs, (unsigned long)time(nullptr),
          (int)WiFi.RSSI(), millis() / 1000UL);
  mqtt.publish(TOPIC_BASE "/status", buf, true);
}

void publishEvent(const char *topic, int ml) {
  if (!mqtt.connected()) return;
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ml\":%d,\"today_ml\":%d,\"ts\":%lu}",
          ml, (int)settings.todayMl, (unsigned long)time(nullptr));
  mqtt.publish(topic, buf);
}

void printStatus() {
  Serial.print(" | WEIGHT = ");
  Serial.print(weight, 1);
  Serial.print(" g | TODAY = ");
  Serial.print(settings.todayMl);
  Serial.print(" ml | STATE = ");
  Serial.println(stateName());
}

// ======================= คำนวณการดื่ม =======================
void evaluateStable(float avgG) {
  if (!baselineValid) {
    baselineG = avgG;
    baselineValid = true;
    Serial.printf("baseline = %.1f g\n", baselineG);
    return;
  }
  float diff = baselineG - avgG;  
  if (diff >= MIN_CHANGE_ML) {
    int ml = (int)(diff + 0.5f);
    settings.todayMl += ml;
    saveSettings();
    baselineG = avgG;
    if (time(nullptr) > 1700000000) lastDrinkTs = (uint32_t)time(nullptr);
    Serial.printf("DRINK %d ml (today %d)\n", ml, (int)settings.todayMl);
    
    // บังคับส่งข้อมูลเข้า MQTTX ทันที
    publishEvent(TOPIC_BASE "/drink", ml);
    publishStatus(); 
  } else if (diff <= -MIN_CHANGE_ML) {
    int ml = (int)(-diff + 0.5f);
    baselineG = avgG;
    Serial.printf("REFILL +%d ml\n", ml);
    
    // บังคับส่งข้อมูลเข้า MQTTX ทันที
    publishEvent(TOPIC_BASE "/refill", ml);
    publishStatus();
  }
}

void resetBottle() {
  state = NO_BOTTLE;
  baselineValid = false;
  winClear();
}

// ======================= รายวัน =======================
void dayToDate(int32_t days, char *out) {
  int32_t z = days + 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t y = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) y++;
  snprintf(out, 11, "%04d-%02u-%02u", (int)y, (unsigned)m, (unsigned)d);
}

void flushPendingDaily() {
  if (!pendingDaily.valid || !mqtt.connected()) return;
  char topic[80], buf[110];
  snprintf(topic, sizeof(topic), TOPIC_BASE "/daily/%s", pendingDaily.date);
  snprintf(buf, sizeof(buf), "{\"date\":\"%s\",\"total_ml\":%d,\"goal_ml\":%d}",
          pendingDaily.date, (int)pendingDaily.totalMl, (int)pendingDaily.goalMl);
  if (mqtt.publish(topic, buf, true)) {
    Serial.printf("daily summary sent: %s\n", buf);
    pendingDaily.valid = false;
  }
}

void checkNewDay() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  int32_t day = (int32_t)((now + TZ_OFFSET_SEC) / 86400);
  if (day != settings.day) {
    if (settings.day > 0) {
      pendingDaily.valid = true;
      dayToDate(settings.day, pendingDaily.date);
      pendingDaily.totalMl = settings.todayMl;
      pendingDaily.goalMl = settings.goalMl;
    }
    settings.day = day;
    settings.todayMl = 0;
    saveSettings();
    Serial.println("new day -> reset today total");
    publishStatus();
  }
}

// ======================= คำสั่งผ่าน MQTT =======================
void onMessage(char *topic, byte *payload, unsigned int len) {
  char msg[32];
  size_t n = len < sizeof(msg) - 1 ? len : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = 0;

  const char *prefix = TOPIC_BASE "/cmd/";
  size_t plen = strlen(prefix);
  if (strncmp(topic, prefix, plen) != 0) return;
  const char *cmd = topic + plen;
  Serial.printf("cmd: %s '%s'\n", cmd, msg);

  if (strcmp(cmd, "tare") == 0) {
    scale.tare(10);
    settings.offset = scale.get_offset();
    saveSettings();
    resetBottle();
    publishAck("tare ok");
  } else if (strcmp(cmd, "calibrate") == 0) {
    float known = atof(msg);
    if (known <= 0) { publishAck("calibrate: send known weight in grams"); return; }
    double raw = scale.get_value(10);
    if (raw == 0) { publishAck("calibrate: no reading"); return; }
    settings.scaleFactor = (float)(raw / known);
    scale.set_scale(settings.scaleFactor);
    saveSettings();
    resetBottle();
    char buf[64];
    snprintf(buf, sizeof(buf), "calibrate ok, factor=%.2f", settings.scaleFactor);
    publishAck(buf);
  } else if (strcmp(cmd, "bottle") == 0) {
    float g = (n == 0 || strcmp(msg, "now") == 0) ? weight : atof(msg);
    if (g < 0) g = 0;
    settings.bottleG = g;
    saveSettings();
    char buf[48];
    snprintf(buf, sizeof(buf), "bottle weight=%.1f g", g);
    publishAck(buf);
  } else if (strcmp(cmd, "goal") == 0) {
    int g = atoi(msg);
    if (g <= 0) { publishAck("goal: send ml > 0"); return; }
    settings.goalMl = g;
    saveSettings();
    publishAck("goal ok");
  } else if (strcmp(cmd, "reset") == 0) {
    settings.todayMl = 0;
    saveSettings();
    publishAck("today reset");
  } else if (strcmp(cmd, "status") != 0) {
    publishAck("unknown command");
    return;
  }
  publishStatus();
}

// ======================= MQTT Connection =======================
void keepConnected() {
  if (WiFi.status() != WL_CONNECTED) return; 

  if (mqtt.connected()) {
    mqtt.loop(); 
    return;
  }

  if (millis() - lastMqttTry < 5000) return;
  lastMqttTry = millis();

  String id = "smartbottle-" + String(ESP.getChipId(), HEX);
  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, TOPIC_BASE "/online", 0, true, "offline"); 
  } else {
    ok = mqtt.connect(id.c_str(), nullptr, nullptr, TOPIC_BASE "/online", 0, true, "offline"); 
  }

  if (ok) {
    Serial.println("MQTT connected!");
    mqtt.publish(TOPIC_BASE "/online", "online", true);
    mqtt.subscribe(TOPIC_BASE "/cmd/#");
    publishStatus();
  } else {
    Serial.printf("MQTT failed, rc=%d\n", mqtt.state());
  }
}

// ======================= STATE PROCESSORS =======================

void processNoBottleState() {
  // 1. งานเบื้องหลังและเชื่อมต่อ Network
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    mqtt.loop();
  }
  keepConnected();       
  checkNewDay();
  flushPendingDaily();

  // 2. อ่านค่าเซนเซอร์
  weight = scale.get_units(2);

  // 3. Logic สำหรับ State NO_BOTTLE
  if (weight >= LIFT_THRESHOLD_G) {   
    winClear();
    winPush(weight);
    state = SETTLING;
  }

  // 4. พิมพ์ Log และ Publish Status ตามรอบเวลา
  printStatus();
  if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) publishStatus();
  delay(100);
}

void processSettlingState() {
  // 1. งานเบื้องหลังและเชื่อมต่อ Network
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    mqtt.loop();
  }
  keepConnected();       
  checkNewDay();
  flushPendingDaily();

  // 2. อ่านค่าเซนเซอร์
  weight = scale.get_units(2);

  // 3. Logic สำหรับ State SETTLING
  if (weight < LIFT_THRESHOLD_G) {    
    state = NO_BOTTLE;
    publishStatus();
  } else {
    winPush(weight);
    float avg;
    if (winStable(avg)) {               
      evaluateStable(avg);
      lastStableG = avg;
      state = IDLE;
      publishStatus();
    }
  }

  // 4. พิมพ์ Log และ Publish Status ตามรอบเวลา
  printStatus();
  if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) publishStatus();
  delay(100);
}

void processIdleState() {
  // 1. งานเบื้องหลังและเชื่อมต่อ Network
  if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
    mqtt.loop();
  }
  keepConnected();       
  checkNewDay();
  flushPendingDaily();

  // 2. อ่านค่าเซนเซอร์
  weight = scale.get_units(2);

  // 3. Logic สำหรับ State IDLE
  if (weight < LIFT_THRESHOLD_G) {    
    state = NO_BOTTLE;
    publishStatus();
  } else if (fabsf(weight - lastStableG) > DISTURB_G) {
    winClear();                       
    winPush(weight);
    state = SETTLING;
  }

  // 4. พิมพ์ Log และ Publish Status ตามรอบเวลา
  printStatus();
  if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) publishStatus();
  delay(100);
}

// ======================= setup / loop =======================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n== Smart Water Bottle Starting ==");

  EEPROM.begin(sizeof(Settings));
  bool hadSaved = loadSettings();

  // ---------- 1. เชื่อมต่อ Wi-Fi ตรงเข้า Hotspot ----------
  const char* ssid = "fsocool"; // ใส่ชื่อ Hotspot บน Notebook (หรือชื่อที่คุณตั้ง)
  const char* password = "10102005";     // ใส่รหัสผ่าน Hotspot

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 30) {
    delay(1000);
    Serial.print(".");
    timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[ERROR] Connection Failed!");
  }

  // ---------- 2. ตั้งค่า HX711 ----------
  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
  scale.set_scale(settings.scaleFactor);
  if (hadSaved) {
    scale.set_offset(settings.offset);
  } else {
    Serial.println("First boot: taring (keep scale empty)...");
    delay(1000);
    scale.tare(10);
    settings.offset = scale.get_offset();
    saveSettings();
  }

  // ---------- 3. ตั้งค่าเวลาและ MQTT ----------
  configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  mqtt.setServer(MQTT_HOST, MQTT_PORT); 
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(3);
  mqtt.setCallback(onMessage); 
}

void loop() {
  switch (state) {
    case NO_BOTTLE:
      processNoBottleState();
      break;

    case SETTLING:
      processSettlingState();
      break;

    case IDLE:
      processIdleState();
      break;
  }
}