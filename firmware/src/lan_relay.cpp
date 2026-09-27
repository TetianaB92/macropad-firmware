#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "lan_relay.h"
#include "bridge_queue.h"
#include "worker_transport.h"
#include "clock_runtime.h"
#include <time.h>

namespace {
struct Credentials { char api[256]{}; char token[80]{}; bool paired=false; };
QueueHandle_t mailbox=nullptr;
WebSocketsClient* connection=nullptr;
bool connected=false;
class BoundedBody : public Stream {
public:
  String value;
  size_t write(uint8_t byte) override { return write(&byte,1); }
  size_t write(const uint8_t* data,size_t size) override {
    if(value.length()+size>32768) return 0;
    return value.concat(reinterpret_cast<const char*>(data),size) ? size : 0;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};
bool privateIp(const IPAddress& ip) {
  return ip[0]==10 || (ip[0]==172 && ip[1]>=16 && ip[1]<=31) || (ip[0]==192 && ip[1]==168);
}
void execute(uint8_t* data,size_t length) {
  if(length>4096) return;
  JsonDocument input;
  if(deserializeJson(input,data,length) || input["type"]!="http.request") return;
  const char* id=input["id"] | "";
  if(!id[0] || strlen(id)>64) return;
  String url=input["url"] | "", method=input["method"] | "GET", body=input["body"] | "";
  JsonDocument reply;reply["type"]="http.response";reply["id"]=id;reply["status"]=0;
  reply["body"]="Invalid LAN HTTP destination";
  // Resolve once, validate, then connect to that exact IP (no DNS rebinding).
  if(url.startsWith("http://") && url.length()<=512 && body.length()<=2048 && (method=="GET" || method=="POST")) {
    const int slash=url.indexOf('/',7);
    String authority=slash<0?url.substring(7):url.substring(7,slash);
    String host=authority;int port=80;
    const int colon=host.indexOf(':');
    if(colon>=0) { port=host.substring(colon+1).toInt();host=host.substring(0,colon); }
    IPAddress ip;
    if(host.length()>0 && authority.indexOf('@')<0 && authority.indexOf('#')<0 && authority.indexOf('?')<0 &&
       port>0 && port<=65535 && (ip.fromString(host) || WiFi.hostByName(host.c_str(),ip)) && privateIp(ip) && ip!=WiFi.localIP()) {
      HTTPClient http;WiFiClient client;
      const String target=String("http://")+ip.toString()+":"+port+(slash<0?"/":url.substring(slash));
      http.setConnectTimeout(1500);http.setTimeout(2500);http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
      if(http.begin(client,target)) {
        http.addHeader("Host",authority);http.addHeader("Content-Type","application/json");
        const int status=method=="POST"?http.POST(body):http.GET();
        BoundedBody output;
        if(status>0 && http.getSize()<=32768 && http.writeToStream(&output)>=0) {
          reply["status"]=status;reply["body"]=output.value;
        } else reply["body"]="LAN request failed or response too large";
        http.end();
      }
    }
  }
  String result;serializeJson(reply,result);connection->sendTXT(result);
}
void task(void*) {
  WebSocketsClient socket;connection=&socket;
  Credentials current{},incoming{};
  uint32_t attempted=0,issued=0;
  socket.onEvent([](WStype_t type,uint8_t* data,size_t length) {
    if(type==WStype_CONNECTED) { connected=true;Serial.println("LAN relay connected"); }
    else if(type==WStype_DISCONNECTED) connected=false;
    else if(type==WStype_TEXT && length<=50000) {
      JsonDocument notice;
      if(!deserializeJson(notice,data,length) && notice["type"]=="state.patch" && notice["patch"]["type"]=="patch") {
        String patch;serializeJson(notice["patch"],patch);
        UiEvent event {UiEventType::ApplySduiPatch,reinterpret_cast<uint8_t*>(strdup(patch.c_str())),patch.length(),RenderedScreenFormat::Jpeg};
        if(event.imageBytes && (!uiQueue || xQueueSend(uiQueue,&event,0)!=pdTRUE)) free(event.imageBytes);
      } else execute(data,length);
    }
  });
  socket.setReconnectInterval(5000);socket.enableHeartbeat(20000,5000,2);
  for(;;) {
    if(xQueueReceive(mailbox,&incoming,0)==pdTRUE) {
      current=incoming;socket.disconnect();connected=false;attempted=0;issued=0;
    }
    const uint32_t now=millis();
    const bool ready=current.paired && current.token[0] && WiFi.status()==WL_CONNECTED && ClockRuntime::valid(uint64_t(time(nullptr))*1000);
    if(!ready) { if(connected) socket.disconnect();vTaskDelay(pdMS_TO_TICKS(250));continue; }
    socket.loop();
    if((!connected && (!attempted || now-attempted>60000)) || (issued && now-issued>3000000)) {
      attempted=now;HTTPClient http;WiFiClientSecure secure;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
      secure.setCACertBundle(worker_ca_start,worker_ca_end-worker_ca_start);
#endif
      http.setConnectTimeout(2000);http.setTimeout(3000);
      const String endpoint=String(current.api)+"/api/devices/relay";
      bool opened=endpoint.startsWith("https://") ? http.begin(secure,endpoint) : http.begin(endpoint);
      if(opened) {
        http.addHeader("Authorization",String("Bearer ")+current.token);
        if(http.GET()==200 && http.getSize()<=2048) {
          BoundedBody body;
          if(http.writeToStream(&body)>=0 && body.value.length()<=2048) {
            JsonDocument config;
            if(!deserializeJson(config,body.value)) {
              String url=config["url"] | "";const int slash=url.indexOf('/',6);
              if(url.startsWith("wss://") && slash>6) {
                String host=url.substring(6,slash),path=url.substring(slash);
                if(host.indexOf('@')<0 && host.indexOf(':')<0) {
                  socket.disconnect();
#if defined(CONFIG_IDF_TARGET_ESP32P4)
                  socket.beginSslWithBundle(host.c_str(),443,path.c_str(),worker_ca_start,worker_ca_end-worker_ca_start);
#endif
                  issued=now;
                }
              }
            }
          }
        }
        http.end();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
}
void LanRelay::configure(const char* api,const char* token,bool paired) {
  static Credentials previous{};
  if(!mailbox) {
    mailbox=xQueueCreate(1,sizeof(Credentials));
    if(!mailbox) return;
    if(xTaskCreatePinnedToCore(task,"lan-relay",12288,nullptr,1,nullptr,1)!=pdPASS) { vQueueDelete(mailbox);mailbox=nullptr;return; }
  }
  if(!api || !token || strlen(api)>=sizeof(previous.api) || strlen(token)>=sizeof(previous.token)) return;
  if(!strcmp(previous.api,api) && !strcmp(previous.token,token) && previous.paired==paired) return;
  snprintf(previous.api,sizeof(previous.api),"%s",api);snprintf(previous.token,sizeof(previous.token),"%s",token);previous.paired=paired;
  xQueueOverwrite(mailbox,&previous);
}
