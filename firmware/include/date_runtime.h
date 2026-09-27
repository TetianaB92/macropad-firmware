#pragma once
#include <ArduinoJson.h>
#include <stdio.h>
#include <time.h>

// Gregorian date operations for bound text/buttons; no networking or widget UI.
namespace DateRuntime {
struct Date { int year, month, day; };
inline int days(int year, int month) {
  static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  return lengths[month-1] + (month == 2 && year%4 == 0 && (year%100 != 0 || year%400 == 0));
}
inline void shift(int& year, int& month, int delta) {
  int total = year*12 + month-1 + delta;
  if (total < 1900*12 || total >= 2200*12) return;
  year=total/12; month=total%12+1;
}
inline void ensure(JsonObject date) {
  if ((date["year"] | 0) >= 1900 && (date["year"] | 0) < 2200 &&
      (date["month"] | 0) >= 1 && (date["month"] | 0) <= 12) return;
  time_t now=time(nullptr); struct tm local {}; localtime_r(&now, &local);
  date["year"]=local.tm_year+1900; date["month"]=local.tm_mon+1;
}
inline Date cell(JsonObject date, int index) {
  ensure(date);
  if(index<0 || index>=42) return {date["year"],date["month"],1};
  int year=date["year"], month=date["month"];
  // Sakamoto's weekday calculation: Sunday=0, independent of timezone/DST.
  static const int offsets[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  int y=year-(month<3);
  int weekday=(y+y/4-y/100+y/400+offsets[month-1]+1)%7;
  int day=index-weekday+1;
  if (day<1) { if (--month==0) { --year;month=12; } day+=days(year,month); }
  else if (day>days(year,month)) { day-=days(year,month); if(++month==13) { ++year;month=1; } }
  return {year,month,day};
}
inline void iso(Date d, char* out, size_t size) { snprintf(out,size,"%04d-%02d-%02d",d.year,d.month,d.day); }
inline void move(JsonObject date, int delta) {
  ensure(date); int y=date["year"], m=date["month"]; shift(y,m,delta); date["year"]=y;date["month"]=m;
}
inline void select(JsonObject date, int index) {
  if(index<0 || index>=42) return;
  const auto d=cell(date,index); char out[16];iso(d,out,sizeof(out));
  if(d.year<1900 || d.year>=2200) return;
  date["selectedDate"]=out;date["year"]=d.year;date["month"]=d.month;
}
inline void title(JsonObject date, char* out, size_t size) {
  ensure(date);
  static const char* months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
  snprintf(out,size,"%s %d",months[date["month"].as<int>()-1],date["year"].as<int>());
}
}
