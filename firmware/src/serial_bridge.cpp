#include "serial_bridge.h"
#include <Arduino.h>
#include <freertos/task.h>
#include "frame_buffer.h"
namespace SerialBridge {
void task(void*) {
  std::string line;line.reserve(4096);bool discard=false;
  for(;;) {
    char* outbound=nullptr;
    while(serialTxQueue && xQueueReceive(serialTxQueue,&outbound,0)==pdTRUE) { Serial.write(reinterpret_cast<uint8_t*>(outbound),strlen(outbound));free(outbound); }
    for(int budget=0;budget<8192 && Serial.available();budget++) {
      const char c=Serial.read();
      if(c!='\n') { if(!discard && line.size()<8192) line+=c;else discard=true;continue; }
      if(!discard) {
        JsonDocument packet;
        if(!deserializeJson(packet,line) && packet["protocol"]=="sdui" && packet["type"]=="patch") {
          patches++;
          UiEvent event {UiEventType::ApplySduiPatch,reinterpret_cast<uint8_t*>(strdup(line.c_str())),line.size(),RenderedScreenFormat::Jpeg};
          if(event.imageBytes && (!uiQueue || xQueueSend(uiQueue,&event,0)!=pdTRUE)) { dropped++;free(event.imageBytes); }
        } else if(packet["protocol"]=="macropad.media" && packet["type"]=="hello") {
          UiEvent event {UiEventType::DescribeChannels,nullptr,0,RenderedScreenFormat::Jpeg};if(uiQueue) xQueueSend(uiQueue,&event,0);
        } else if(packet["protocol"]=="sdui" && packet["type"]=="navigate" && packet["step"].is<int>()) {
          const int step=packet["step"].as<int>();
          if(step==-1 || step==1) {
            UiEvent event {UiEventType::NavigatePage,nullptr,0,RenderedScreenFormat::Jpeg}; event.status=step;
            if(uiQueue) xQueueSend(uiQueue,&event,0);
          }
        } else if(line.rfind("enc+",0)==0 || line.rfind("enc-",0)==0 || line.rfind("encp",0)==0) {
          UiEvent event {UiEventType::EncoderInput,reinterpret_cast<uint8_t*>(strdup(line.c_str())),line.size(),RenderedScreenFormat::Jpeg};
          if(event.imageBytes && (!uiQueue || xQueueSend(uiQueue,&event,0)!=pdTRUE)) free(event.imageBytes);
        } else if(line=="factory_reset" || line=="factory") {
          char action[64]="factory_reset";if(actionQueue) xQueueSend(actionQueue,&action,0);
        } else invalid++;
      }
      if(discard) invalid++;
      line.clear();discard=false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
}
