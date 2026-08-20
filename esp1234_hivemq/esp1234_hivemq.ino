/*
  ESP32 #1 - pushrods 1 to 4 via an 8-channel, active-LOW relay board.
  Relay pairs: 1=(13,14), 2=(16,17), 3=(18,19), 4=(25,26).
  Change the Wi-Fi and HiveMQ settings below before uploading.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_HOST = "YOUR_CLUSTER.s1.eu.hivemq.cloud"; // no https://
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USER = "YOUR_HIVEMQ_USERNAME";
const char* MQTT_PASSWORD = "YOUR_HIVEMQ_PASSWORD";
const char* TOPIC_ROOT = "pushrod";
const char* BOARD_ID = "esp1234";
const uint8_t FIRST_ROD = 1;

constexpr uint8_t ROD_COUNT = 4;
constexpr uint8_t PROFILE_COUNT = 4;
constexpr uint8_t RELAY_ON = LOW;
constexpr uint8_t RELAY_OFF = HIGH;
const uint8_t extendPin[ROD_COUNT] = {13, 16, 18, 25};
const uint8_t retractPin[ROD_COUNT] = {14, 17, 19, 26};

struct Timing { uint32_t inPause, extend, outPause, retract; };
struct Stored { uint32_t magic; Timing timing[PROFILE_COUNT][ROD_COUNT]; } saved;
constexpr uint32_t MAGIC = 0x50523831; // "PR81" -- change forces safe defaults

Timing active[ROD_COUNT];
uint8_t phase[ROD_COUNT] = {0}; // 0 paused in, 1 extending, 2 paused out, 3 retracting
uint32_t phaseStartedAt[ROD_COUNT] = {0};
bool enabled[ROD_COUNT] = {false};
bool completed[ROD_COUNT] = {false};
bool running = false, rotatingProfiles = false;
uint8_t activeProfile = 0;

WiFiClientSecure tls;
PubSubClient mqtt(tls);
Preferences prefs;

void motor(uint8_t index, uint8_t direction) { // 0 stop, 1 extend, 2 retract
  // Writing both pins every time prevents a forward/reverse relay conflict.
  digitalWrite(extendPin[index], direction == 1 ? RELAY_ON : RELAY_OFF);
  digitalWrite(retractPin[index], direction == 2 ? RELAY_ON : RELAY_OFF);
}

void stopAll() {
  for (uint8_t i = 0; i < ROD_COUNT; ++i) { motor(i, 0); enabled[i] = false; }
  running = false;
  rotatingProfiles = false;
}

bool persist() {
  prefs.begin("pushrod", false);
  const size_t written = prefs.putBytes("settings", &saved, sizeof(saved));
  prefs.end();
  return written == sizeof(saved);
}

void status(const char* text, bool retained = true) {
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/%s/status", TOPIC_ROOT, BOARD_ID);
  mqtt.publish(topic, text, retained);
}

void loadSettings() {
  prefs.begin("pushrod", true);
  const size_t read = prefs.getBytes("settings", &saved, sizeof(saved));
  prefs.end();
  if (read == sizeof(saved) && saved.magic == MAGIC) return;
  saved.magic = MAGIC;
  for (uint8_t p = 0; p < PROFILE_COUNT; ++p)
    for (uint8_t i = 0; i < ROD_COUNT; ++i)
      saved.timing[p][i] = {1000, 3000, 1000, 3000};
  persist();
}

void beginCycle(uint8_t profile, bool rotate) {
  activeProfile = profile;
  rotatingProfiles = rotate;
  for (uint8_t i = 0; i < ROD_COUNT; ++i) {
    active[i] = saved.timing[profile][i];
    phase[i] = 0; completed[i] = false; enabled[i] = true;
    phaseStartedAt[i] = millis();
    motor(i, 0);
  }
  running = true;
  char message[32];
  snprintf(message, sizeof(message), rotate ? "rotating_%u" : "running_%u", profile + 1);
  status(message);
}

uint32_t phaseDuration(uint8_t index) {
  if (phase[index] == 0) return active[index].inPause;
  if (phase[index] == 1) return active[index].extend;
  if (phase[index] == 2) return active[index].outPause;
  return active[index].retract;
}

void updateCycle() {
  if (!running) return;
  const uint32_t now = millis();
  for (uint8_t i = 0; i < ROD_COUNT; ++i) {
    if (!enabled[i] || now - phaseStartedAt[i] < phaseDuration(i)) continue;
    phaseStartedAt[i] = now;
    const bool finished = phase[i] == 3;
    phase[i] = (phase[i] + 1) % 4;
    motor(i, phase[i] == 1 ? 1 : phase[i] == 3 ? 2 : 0);
    if (rotatingProfiles && finished) { completed[i] = true; enabled[i] = false; }
  }
  if (rotatingProfiles && completed[0] && completed[1] && completed[2] && completed[3])
    beginCycle((activeProfile + 1) % PROFILE_COUNT, true);
}

bool number(const char* input, uint32_t& output) {
  char* end;
  const unsigned long value = strtoul(input, &end, 10);
  if (*input == '\0' || *end != '\0') return false;
  output = value;
  return true;
}

void callback(char*, byte* payload, unsigned int length) {
  if (length >= 240) return;
  char message[240];
  memcpy(message, payload, length); message[length] = '\0';
  if (!strcmp(message, "stop")) { stopAll(); status("stopped"); return; }
  if (!strcmp(message, "run_all")) { beginCycle(0, true); return; }
  if (!strcmp(message, "run")) { beginCycle(0, false); return; }

  char* part[8]; uint8_t count = 0;
  char* token = strtok(message, ",");
  while (token && count < 8) { part[count++] = token; token = strtok(nullptr, ","); }

  if (count == 2 && !strcmp(part[0], "run")) {
    uint32_t profile;
    if (number(part[1], profile) && profile >= 1 && profile <= PROFILE_COUNT) beginCycle(profile - 1, false);
    return;
  }
  if (count == 3 && !strcmp(part[0], "manual")) {
    uint32_t rod;
    if (!number(part[1], rod) || rod < FIRST_ROD || rod >= FIRST_ROD + ROD_COUNT) return;
    stopAll();
    const uint8_t index = rod - FIRST_ROD;
    if (!strcmp(part[2], "extend")) motor(index, 1);
    else if (!strcmp(part[2], "retract")) motor(index, 2);
    else if (!strcmp(part[2], "stop")) motor(index, 0);
    else return;
    status("manual");
    return;
  }
  if (count == 6 && !strcmp(part[0], "test")) {
    uint32_t rod, values[4];
    if (!number(part[1], rod) || rod < FIRST_ROD || rod >= FIRST_ROD + ROD_COUNT) return;
    for (uint8_t i = 0; i < 4; ++i) if (!number(part[i + 2], values[i])) return;
    stopAll();
    const uint8_t index = rod - FIRST_ROD;
    active[index] = {values[0], values[1], values[2], values[3]};
    phase[index] = 0; phaseStartedAt[index] = millis(); enabled[index] = true; running = true;
    motor(index, 0);
    char response[32]; snprintf(response, sizeof(response), "testing_rod_%u", rod); status(response);
    return;
  }
  // config,profile,rod,inPause,extend,outPause,retract  (seven fields)
  if (count == 7 && !strcmp(part[0], "config")) {
    uint32_t profile, rod, values[4];
    if (!number(part[1], profile) || profile < 1 || profile > PROFILE_COUNT ||
        !number(part[2], rod) || rod < FIRST_ROD || rod >= FIRST_ROD + ROD_COUNT) return;
    for (uint8_t i = 0; i < 4; ++i) if (!number(part[i + 3], values[i])) return;
    saved.timing[profile - 1][rod - FIRST_ROD] = {values[0], values[1], values[2], values[3]};
    return;
  }
  if (count == 3 && !strcmp(part[0], "confirm")) {
    uint32_t profile, confirmationToken;
    if (!number(part[1], profile) || profile < 1 || profile > PROFILE_COUNT || !number(part[2], confirmationToken)) return;
    char response[56];
    snprintf(response, sizeof(response), persist() ? "profile_saved,%u,%lu" : "profile_save_failed,%u,%lu", profile, (unsigned long)confirmationToken);
    status(response, false);
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  while (!mqtt.connected()) {
    char clientId[36];
    snprintf(clientId, sizeof(clientId), "%s-%012llX", BOARD_ID, ESP.getEfuseMac());
    if (mqtt.connect(clientId, MQTT_USER, MQTT_PASSWORD)) {
      char topic[64];
      snprintf(topic, sizeof(topic), "%s/%s/cmd", TOPIC_ROOT, BOARD_ID); mqtt.subscribe(topic, 1);
      snprintf(topic, sizeof(topic), "%s/all/cmd", TOPIC_ROOT); mqtt.subscribe(topic, 1);
      status("online");
      Serial.println("HiveMQ connected");
    } else { Serial.printf("HiveMQ error %d; retrying...\n", mqtt.state()); delay(3000); }
  }
}

void setup() {
  Serial.begin(115200);
  for (uint8_t i = 0; i < ROD_COUNT; ++i) {
    pinMode(extendPin[i], OUTPUT); pinMode(retractPin[i], OUTPUT); motor(i, 0);
  }
  loadSettings();
  connectWiFi();
  tls.setInsecure(); // TLS encryption without CA validation; use a CA cert for higher-security installs.
  mqtt.setServer(MQTT_HOST, MQTT_PORT); mqtt.setCallback(callback); mqtt.setBufferSize(256);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(300); return; }
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();
  updateCycle();
}
