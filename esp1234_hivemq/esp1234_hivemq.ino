/*
  ESP32 #1 — physical pushrods 1–4.  Keeps the original active-LOW relay pins:
  1=(GPIO13,14), 2=(16,17), 3=(18,19), 4=(25,26).
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>

const char* WIFI_SSID = "SFKsh-L2";
const char* WIFI_PASSWORD = "Sfksh@2025";
const char* MQTT_HOST = "a4309d1f456042fbb2ce25305dcefbf5.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USER = "Shit6767";
const char* MQTT_PASSWORD = "Shit6767";
const char* TOPIC_ROOT = "pushrod";
#ifdef PUSHROD_SECOND_BOARD
const char* BOARD_ID = "esp5678";
const uint8_t FIRST_ROD = 5;
#else
const char* BOARD_ID = "esp1234";
const uint8_t FIRST_ROD = 1;
#endif

constexpr uint8_t ROD_COUNT = 4, PROFILE_COUNT = 4, RELAY_ON = LOW, RELAY_OFF = HIGH;
constexpr uint32_t MAX_DURATION_MS = 3600000UL, MAGIC = 0x50523831;
const uint8_t extendPin[ROD_COUNT] = {13, 16, 18, 25};
const uint8_t retractPin[ROD_COUNT] = {14, 17, 19, 26};
struct Timing { uint32_t inPause, extend, outPause, retract; };
struct Stored { uint32_t magic; Timing timing[PROFILE_COUNT][ROD_COUNT]; } saved;

Timing active[ROD_COUNT], staged[ROD_COUNT];
uint8_t phase[ROD_COUNT] = {0}, stagedMask = 0, stagedProfile = 0;
uint32_t phaseAt[ROD_COUNT] = {0}, stagedToken = 0, sequenceToken = 0;
bool enabled[ROD_COUNT] = {false}, completed[ROD_COUNT] = {false}, testing[ROD_COUNT] = {false};
bool running = false, sequenceMode = false, stopAfterCycle = false;
uint8_t activeProfile = 0;
WiFiClientSecure tls; PubSubClient mqtt(tls); Preferences prefs;
uint32_t lastWiFiAttempt = 0, lastMqttAttempt = 0;
bool wifiLostLogged = false, mqttLostLogged = false;

void motor(uint8_t i, uint8_t direction) {
  digitalWrite(extendPin[i], direction == 1 ? RELAY_ON : RELAY_OFF);
  digitalWrite(retractPin[i], direction == 2 ? RELAY_ON : RELAY_OFF);
}
bool anyEnabled() { for (uint8_t i=0;i<ROD_COUNT;i++) if (enabled[i]) return true; return false; }
void stopAll() {
  for (uint8_t i=0;i<ROD_COUNT;i++) { motor(i,0); enabled[i]=completed[i]=testing[i]=false; }
  running=false; sequenceMode=false; stopAfterCycle=false;
}
bool validTiming(const Timing& t) {
  return t.inPause<=MAX_DURATION_MS && t.extend<=MAX_DURATION_MS && t.outPause<=MAX_DURATION_MS && t.retract<=MAX_DURATION_MS;
}
bool persist() {
  prefs.begin("pushrod",false); size_t n=prefs.putBytes("settings",&saved,sizeof(saved)); prefs.end(); return n==sizeof(saved);
}
void loadSettings() {
  prefs.begin("pushrod",true); size_t n=prefs.getBytes("settings",&saved,sizeof(saved)); prefs.end();
  if (n==sizeof(saved) && saved.magic==MAGIC) {
    bool valid=true; for(uint8_t p=0;p<PROFILE_COUNT;p++)for(uint8_t i=0;i<ROD_COUNT;i++)valid &= validTiming(saved.timing[p][i]);
    if(valid) return;
  }
  saved.magic=MAGIC;
  for(uint8_t p=0;p<PROFILE_COUNT;p++)for(uint8_t i=0;i<ROD_COUNT;i++)saved.timing[p][i]={1000,3000,1000,3000};
  persist();
}
void publishStatus(const char* text, bool retained=true) {
  char topic[64]; snprintf(topic,sizeof(topic),"%s/%s/status",TOPIC_ROOT,BOARD_ID); mqtt.publish(topic,text,retained);
}
void eventStatus(const char* text) { publishStatus(text,false); }
void actionAck(const char* action,uint32_t token) {
  char s[56]; snprintf(s,sizeof(s),"ack,%s,%lu",action,(unsigned long)token); eventStatus(s);
}
void beginCycle(uint8_t profile,bool oneShot,uint32_t token) {
  activeProfile=profile; sequenceMode=oneShot; stopAfterCycle=false; sequenceToken=token;
  for(uint8_t i=0;i<ROD_COUNT;i++){active[i]=saved.timing[profile][i];phase[i]=0;completed[i]=testing[i]=false;enabled[i]=true;phaseAt[i]=millis();motor(i,0);}
  running=true; char s[32]; snprintf(s,sizeof(s),oneShot?"sequence_%u":"running_%u",profile+1); publishStatus(s);
}
uint32_t duration(uint8_t i) { return phase[i]==0?active[i].inPause:phase[i]==1?active[i].extend:phase[i]==2?active[i].outPause:active[i].retract; }
void updateCycle() {
  if(!running)return; uint32_t now=millis();
  for(uint8_t i=0;i<ROD_COUNT;i++) {
    if(!enabled[i] || now-phaseAt[i]<duration(i))continue;
    phaseAt[i]=now; bool finished=phase[i]==3; phase[i]=(phase[i]+1)%4; motor(i,phase[i]==1?1:phase[i]==3?2:0);
    if((sequenceMode||stopAfterCycle)&&finished){completed[i]=true;enabled[i]=false;}
  }
  bool allDone=true;for(uint8_t i=0;i<ROD_COUNT;i++)allDone &= completed[i];
  if(sequenceMode&&allDone){running=false;sequenceMode=false;char s[64];snprintf(s,sizeof(s),"sequence_done,%u,%lu",activeProfile+1,(unsigned long)sequenceToken);publishStatus(s);}
  else if(stopAfterCycle&&!anyEnabled()){running=false;stopAfterCycle=false;}
}
bool number(const char* s,uint32_t& out) {
  if(!s||!*s)return false; uint32_t n=0;
  for(const char* p=s;*p;p++){if(*p<'0'||*p>'9')return false;uint8_t d=*p-'0';if(n>(UINT32_MAX-d)/10UL)return false;n=n*10UL+d;}
  out=n;return true;
}
void sendConfigDump(uint32_t token) {
  char s[112];
  for(uint8_t p=0;p<PROFILE_COUNT;p++)for(uint8_t i=0;i<ROD_COUNT;i++){const Timing& t=saved.timing[p][i];snprintf(s,sizeof(s),"config_value,%u,%u,%lu,%lu,%lu,%lu,%lu",p+1,FIRST_ROD+i,(unsigned long)t.inPause,(unsigned long)t.extend,(unsigned long)t.outPause,(unsigned long)t.retract,(unsigned long)token);eventStatus(s);}
  snprintf(s,sizeof(s),"config_dump_done,%lu",(unsigned long)token);eventStatus(s);
}
void callback(char*,byte* payload,unsigned int length) {
  if(length>=220)return; char b[220]; memcpy(b,payload,length);b[length]=0;
  if(!strcmp(b,"stop")){stopAll();publishStatus("stopped");return;}
  char* part[12];uint8_t count=0;char* x=strtok(b,",");while(x&&count<12){part[count++]=x;x=strtok(nullptr,",");}
  if(count==2&&!strcmp(part[0],"stop")){uint32_t t;if(!number(part[1],t))return;stopAll();publishStatus("stopped");actionAck("stop",t);return;}
  if(count==2&&!strcmp(part[0],"get_config")){uint32_t t;if(!number(part[1],t))return;sendConfigDump(t);actionAck("get_config",t);return;}
  if(count==2&&!strcmp(part[0],"run")){uint32_t p;if(number(part[1],p)&&p>=1&&p<=PROFILE_COUNT)beginCycle(p-1,false,0);return;}
  if(count==3&&!strcmp(part[0],"run")){uint32_t p,t;if(!number(part[1],p)||!number(part[2],t)||p<1||p>PROFILE_COUNT)return;beginCycle(p-1,false,t);actionAck("run",t);return;}
  if(count==3&&!strcmp(part[0],"sequence")){uint32_t p,t;if(!number(part[1],p)||!number(part[2],t)||p<1||p>PROFILE_COUNT)return;beginCycle(p-1,true,t);actionAck("sequence",t);return;}
  if(count==4&&!strcmp(part[0],"manual")) {
    uint32_t rod,t;if(!number(part[1],rod)||!number(part[3],t)||rod<FIRST_ROD||rod>=FIRST_ROD+ROD_COUNT)return;
    uint8_t i=rod-FIRST_ROD;if(sequenceMode){sequenceMode=false;stopAfterCycle=true;}motor(i,0);enabled[i]=testing[i]=completed[i]=false;
    if(!strcmp(part[2],"extend"))motor(i,1);else if(!strcmp(part[2],"retract"))motor(i,2);else if(strcmp(part[2],"stop"))return;
    running=anyEnabled();if(!running)stopAfterCycle=false;actionAck("manual",t);return;
  }
  if(count==7&&!strcmp(part[0],"test")) {
    uint32_t rod,v[4],t;if(!number(part[1],rod)||!number(part[6],t)||rod<FIRST_ROD||rod>=FIRST_ROD+ROD_COUNT)return;
    for(uint8_t i=0;i<4;i++)if(!number(part[i+2],v[i])||v[i]>MAX_DURATION_MS)return;
    stopAll();uint8_t i=rod-FIRST_ROD;active[i]={v[0],v[1],v[2],v[3]};phase[i]=0;phaseAt[i]=millis();enabled[i]=testing[i]=true;running=true;motor(i,0);actionAck("test",t);return;
  }
  if(count==8&&!strcmp(part[0],"config")) {
    uint32_t p,rod,v[4],t;if(!number(part[1],p)||!number(part[2],rod)||!number(part[7],t)||p<1||p>PROFILE_COUNT||rod<FIRST_ROD||rod>=FIRST_ROD+ROD_COUNT)return;
    for(uint8_t i=0;i<4;i++)if(!number(part[i+3],v[i])||v[i]>MAX_DURATION_MS)return;
    if(stagedToken!=t||stagedProfile!=p){stagedToken=t;stagedProfile=p;stagedMask=0;}uint8_t i=rod-FIRST_ROD;staged[i]={v[0],v[1],v[2],v[3]};stagedMask|=(1U<<i);return;
  }
  if(count==3&&!strcmp(part[0],"confirm")) {
    uint32_t p,t;if(!number(part[1],p)||!number(part[2],t)||p<1||p>PROFILE_COUNT)return;char s[64];
    if(stagedToken!=t||stagedProfile!=p||stagedMask!=0x0F){snprintf(s,sizeof(s),"profile_save_incomplete,%u,%lu",p,(unsigned long)t);eventStatus(s);return;}
    Stored candidate=saved;for(uint8_t i=0;i<ROD_COUNT;i++)candidate.timing[p-1][i]=staged[i];saved=candidate;bool ok=persist();stagedMask=0;
    snprintf(s,sizeof(s),ok?"profile_saved,%u,%lu":"profile_save_failed,%u,%lu",p,(unsigned long)t);eventStatus(s);return;
  }
}
void connectMQTTOnce() {
  char id[36],willTopic[64];snprintf(id,sizeof(id),"%s-%012llX",BOARD_ID,ESP.getEfuseMac());snprintf(willTopic,sizeof(willTopic),"%s/%s/status",TOPIC_ROOT,BOARD_ID);
  if(!mqtt.connect(id,MQTT_USER,MQTT_PASSWORD,willTopic,1,true,"offline")){Serial.printf("HiveMQ error %d\n",mqtt.state());return;}
  char topic[64];snprintf(topic,sizeof(topic),"%s/%s/cmd",TOPIC_ROOT,BOARD_ID);mqtt.subscribe(topic,1);snprintf(topic,sizeof(topic),"%s/all/cmd",TOPIC_ROOT);mqtt.subscribe(topic,1);mqttLostLogged=false;publishStatus("online");
}
void setup() {
  Serial.begin(115200);for(uint8_t i=0;i<ROD_COUNT;i++){pinMode(extendPin[i],OUTPUT);pinMode(retractPin[i],OUTPUT);motor(i,0);}loadSettings();
  WiFi.mode(WIFI_STA);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);tls.setInsecure();mqtt.setServer(MQTT_HOST,MQTT_PORT);mqtt.setCallback(callback);mqtt.setBufferSize(384);
}
void loop() {
  uint32_t now=millis();
  if(WiFi.status()!=WL_CONNECTED){if(!wifiLostLogged){stopAll();wifiLostLogged=true;Serial.println("Wi-Fi disconnected: rods stopped.");}if(now-lastWiFiAttempt>=3000){lastWiFiAttempt=now;WiFi.reconnect();}delay(5);return;}
  wifiLostLogged=false;
  if(!mqtt.connected()){if(!mqttLostLogged){stopAll();mqttLostLogged=true;Serial.println("MQTT disconnected: rods stopped.");}if(now-lastMqttAttempt>=3000){lastMqttAttempt=now;connectMQTTOnce();}delay(5);return;}
  mqtt.loop();updateCycle();
}
