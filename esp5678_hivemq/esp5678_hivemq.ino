/* ESP32 #2 - pushrods 5 to 8.  Use an 8-channel active-LOW relay board.
   Pairs: rod 5=(GPIO13,14), 6=(16,17), 7=(18,19), 8=(25,26). */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>

const char* WIFI_SSID = "SFKsh-L2";
const char* WIFI_PASSWORD = "Sfksh@2025";
const char* MQTT_HOST = "a4309d1f456042fbb2ce25305dcefbf5.s1.eu.hivemq.cloud"; // no https://
const uint16_t MQTT_PORT = 8883;
const char* MQTT_USER = "Shit6767";
const char* MQTT_PASSWORD = "Shit6767";
const char* TOPIC_ROOT = "pushrod";
const char* BOARD_ID = "esp5678";
const uint8_t FIRST_ROD = 5;

constexpr uint8_t COUNT=4, PROFILES=4, ON=LOW, OFF=HIGH;
const uint8_t extendPin[COUNT]={13,16,18,25};
const uint8_t retractPin[COUNT]={14,17,19,26};
struct Timing { uint32_t inPause,extend,outPause,retract; };
struct Stored { uint32_t magic; Timing t[PROFILES][COUNT]; } saved;
constexpr uint32_t MAGIC=0x50523831;
Timing active[COUNT]; uint8_t phase[COUNT]={0}; uint32_t phaseAt[COUNT]={0};
bool enabled[COUNT]={false},completed[COUNT]={false},running=false,rotating=false; uint8_t activeProfile=0;
WiFiClientSecure tls; PubSubClient mqtt(tls); Preferences prefs;

void motor(uint8_t i,uint8_t action) { // 0 stop, 1 extend, 2 retract; never energises both relays
  digitalWrite(extendPin[i],action==1?ON:OFF);
  digitalWrite(retractPin[i],action==2?ON:OFF);
}
void stopAll(){for(uint8_t i=0;i<COUNT;i++){motor(i,0);enabled[i]=false;}running=false;rotating=false;}
bool persist(){prefs.begin("pushrod",false);size_t n=prefs.putBytes("settings",&saved,sizeof(saved));prefs.end();return n==sizeof(saved);}
void status(const char* msg,bool retain=true){char topic[64];snprintf(topic,sizeof(topic),"%s/%s/status",TOPIC_ROOT,BOARD_ID);mqtt.publish(topic,msg,retain);}
void loadSettings(){prefs.begin("pushrod",true);size_t n=prefs.getBytes("settings",&saved,sizeof(saved));prefs.end();if(n==sizeof(saved)&&saved.magic==MAGIC)return;saved.magic=MAGIC;for(uint8_t p=0;p<PROFILES;p++)for(uint8_t i=0;i<COUNT;i++)saved.t[p][i]={1000,3000,1000,3000};persist();}
void beginCycle(uint8_t p,bool rotate){activeProfile=p;rotating=rotate;for(uint8_t i=0;i<COUNT;i++){active[i]=saved.t[p][i];phase[i]=0;completed[i]=false;enabled[i]=true;phaseAt[i]=millis();motor(i,0);}running=true;char s[32];snprintf(s,sizeof(s),rotate?"rotating_%u":"running_%u",p+1);status(s);}
uint32_t duration(uint8_t i){if(phase[i]==0)return active[i].inPause;if(phase[i]==1)return active[i].extend;if(phase[i]==2)return active[i].outPause;return active[i].retract;}
void updateCycle(){if(!running)return;uint32_t now=millis();for(uint8_t i=0;i<COUNT;i++){if(!enabled[i]||now-phaseAt[i]<duration(i))continue;phaseAt[i]=now;bool finished=phase[i]==3;phase[i]=(phase[i]+1)%4;motor(i,phase[i]==1?1:phase[i]==3?2:0);if(rotating&&finished){completed[i]=true;enabled[i]=false;}}if(rotating&&completed[0]&&completed[1]&&completed[2]&&completed[3])beginCycle((activeProfile+1)%PROFILES,true);}
bool number(const char* s,uint32_t& out){char* end;unsigned long n=strtoul(s,&end,10);if(*s=='\0'||*end!='\0')return false;out=n;return true;}
void callback(char*,byte* payload,unsigned int len){
  if(len>=240)return;char b[240];memcpy(b,payload,len);b[len]='\0';
  if(!strcmp(b,"stop")){stopAll();status("stopped");return;}if(!strcmp(b,"run_all")){beginCycle(0,true);return;}if(!strcmp(b,"run")){beginCycle(0,false);return;}
  char* a[8];uint8_t n=0;char* x=strtok(b,",");while(x&&n<8){a[n++]=x;x=strtok(nullptr,",");}
  if(n==2&&!strcmp(a[0],"run")){uint32_t p;if(number(a[1],p)&&p>=1&&p<=PROFILES)beginCycle(p-1,false);return;}
  if(n==3&&!strcmp(a[0],"manual")){uint32_t rod;if(!number(a[1],rod)||rod<FIRST_ROD||rod>=FIRST_ROD+COUNT)return;stopAll();uint8_t i=rod-FIRST_ROD;if(!strcmp(a[2],"extend"))motor(i,1);else if(!strcmp(a[2],"retract"))motor(i,2);else if(!strcmp(a[2],"stop"))motor(i,0);else return;status("manual");return;}
  if(n==6&&!strcmp(a[0],"test")){uint32_t rod,v[4];if(!number(a[1],rod)||rod<FIRST_ROD||rod>=FIRST_ROD+COUNT)return;for(uint8_t i=0;i<4;i++)if(!number(a[i+2],v[i]))return;stopAll();uint8_t i=rod-FIRST_ROD;active[i]={v[0],v[1],v[2],v[3]};phase[i]=0;phaseAt[i]=millis();enabled[i]=true;running=true;motor(i,0);char s[32];snprintf(s,sizeof(s),"testing_rod_%u",rod);status(s);return;}
  if(n==7&&!strcmp(a[0],"config")){uint32_t p,rod,v[4];if(!number(a[1],p)||p<1||p>PROFILES||!number(a[2],rod)||rod<FIRST_ROD||rod>=FIRST_ROD+COUNT)return;for(uint8_t i=0;i<4;i++)if(!number(a[i+3],v[i]))return;saved.t[p-1][rod-FIRST_ROD]={v[0],v[1],v[2],v[3]};return;}
  if(n==3&&!strcmp(a[0],"confirm")){uint32_t p,token;if(!number(a[1],p)||p<1||p>PROFILES||!number(a[2],token))return;char s[56];snprintf(s,sizeof(s),persist()?"profile_saved,%u,%lu":"profile_save_failed,%u,%lu",p,(unsigned long)token);status(s,false);}
}
void connectWiFi(){WiFi.mode(WIFI_STA);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);Serial.print("Connecting Wi-Fi");while(WiFi.status()!=WL_CONNECTED){delay(500);Serial.print('.');}Serial.printf("\nWi-Fi connected: %s\n",WiFi.localIP().toString().c_str());}
void connectMQTT(){while(!mqtt.connected()){char id[36];snprintf(id,sizeof(id),"%s-%012llX",BOARD_ID,ESP.getEfuseMac());if(mqtt.connect(id,MQTT_USER,MQTT_PASSWORD)){char topic[64];snprintf(topic,sizeof(topic),"%s/%s/cmd",TOPIC_ROOT,BOARD_ID);mqtt.subscribe(topic,1);snprintf(topic,sizeof(topic),"%s/all/cmd",TOPIC_ROOT);mqtt.subscribe(topic,1);status("online");Serial.println("HiveMQ connected");}else{Serial.printf("HiveMQ error %d; retrying...\n",mqtt.state());delay(3000);}}}
void setup(){Serial.begin(115200);for(uint8_t i=0;i<COUNT;i++){pinMode(extendPin[i],OUTPUT);pinMode(retractPin[i],OUTPUT);motor(i,0);}loadSettings();connectWiFi();tls.setInsecure();mqtt.setServer(MQTT_HOST,MQTT_PORT);mqtt.setCallback(callback);mqtt.setBufferSize(256);}
void loop(){if(WiFi.status()!=WL_CONNECTED){WiFi.reconnect();delay(300);return;}if(!mqtt.connected())connectMQTT();mqtt.loop();updateCycle();}
