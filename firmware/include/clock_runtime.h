#pragma once
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
namespace ClockRuntime {
static constexpr const char* kDefaultTimezone="EET-2EEST,M3.5.0/3,M10.5.0/4";
inline bool valid(uint64_t epochMs) { return epochMs>=1704067200000ULL && epochMs<4102444800000ULL; }
inline int offsetMinutes(time_t now) {
  struct tm local {};localtime_r(&now,&local);
  char offset[8]={};if(!strftime(offset,sizeof(offset),"%z",&local)) return 0;
  const int hours=(offset[1]-'0')*10+offset[2]-'0',minutes=(offset[3]-'0')*10+offset[4]-'0';
  return (offset[0]=='-'?1:-1)*(hours*60+minutes);
}
}
