#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>

// Repeating local interval with acknowledgement, snooze and a quiet window.
// Clock corrections cannot change the elapsed time. State survives page changes.
namespace IntervalRuntime {
inline bool quiet(JsonObject v,int hour) {
  if(v["active"] | false) return false;
  if(!(v["quietEnabled"] | false)) return false;
  if(hour<0) return true; // Wait for time sync before emitting a night-time alarm.
  const int start=v["quietStart"] | 22,end=v["quietEnd"] | 8;
  return start==end?false:start<end?(hour>=start && hour<end):(hour>=start || hour<end);
}
inline void reset(JsonObject v,uint64_t now,bool snooze=false) {
  const unsigned seconds=snooze?(v["snoozeSec"] | 600U):(v["durationSec"] | 3600U);
  v["remainingMs"]=uint64_t(seconds)*1000;
  v["cycleSec"]=seconds;v["active"]=false;
  v["_lastMs"]=now;v["alerting"]=false;
}
inline void action(JsonObject v,uint64_t now,const char* name) {
  if(strcmp(name,"interval.primary")==0 && !(v["active"] | false)) {
    const unsigned seconds=v["activitySec"] | 120U;
    v["active"]=true;v["alerting"]=false;v["quiet"]=false;
    v["cycleSec"]=seconds;v["remainingMs"]=uint64_t(seconds)*1000;v["_lastMs"]=now;
  } else if(strcmp(name,"interval.primary")==0 || strcmp(name,"interval.reset")==0 || strcmp(name,"interval.snooze")==0) {
    reset(v,now,strcmp(name,"interval.snooze")==0);
  }
}
inline int progress(JsonObject v) {
  const auto duration=uint64_t(v["cycleSec"] | (v["durationSec"] | 3600U))*1000;
  const auto remaining=v["remainingMs"].as<uint64_t>();
  return !duration || remaining>=duration?0:int((duration-remaining)*100/duration);
}
inline void format(JsonObject v,const char* kind,char* out,size_t size) {
  const bool active=v["active"] | false,alerting=v["alerting"] | false;
  if(strcmp(kind,"interval.primary")==0) snprintf(out,size,"%s",active?"Done":"Start");
  else if(strcmp(kind,"interval.status")==0) {
    if(active) snprintf(out,size,"%s",alerting?"Session complete":"Stretch in progress");
    else if(v["quiet"] | false) snprintf(out,size,"Paused for quiet hours");
    else if(alerting) snprintf(out,size,"%s",v["message"] | "Time to stand up & stretch");
    else snprintf(out,size,"Every %u min",(v["durationSec"] | 3600U)/60);
  } else {
    const unsigned seconds=(v["remainingMs"].as<uint64_t>()+999)/1000;
    snprintf(out,size,"%02u:%02u",seconds/60,seconds%60);
  }
}
inline void tick(JsonObject v,uint64_t now,int hour) {
  if(!v["remainingMs"].is<uint64_t>()) reset(v,now);
  const uint64_t last=v["_lastMs"].as<uint64_t>();
  v["_lastMs"]=now;
  const bool muted=quiet(v,hour);v["quiet"]=muted;
  if(muted || (v["alerting"] | false)) return;
  auto left=v["remainingMs"].as<uint64_t>();
  const uint64_t elapsed=now>=last?now-last:0;
  left=elapsed<left?left-elapsed:0;
  v["remainingMs"]=left;
  if(!left) v["alerting"]=true;
}
}
