#pragma once
#include <lvgl.h>
#include <string>
#include <cstring>
// Bounded RGB565 thumbnail primitive, transported as a normal state binding.
namespace InlineImage {
struct Data { lv_img_dsc_t frame {}; lv_obj_t* target = nullptr; uint8_t pixels[40*40*2] {}; std::string previous; };
inline void destroy(lv_event_t* e) {
  auto* d=static_cast<Data*>(lv_event_get_user_data(e));
  lv_img_cache_invalidate_src(&d->frame);delete d;
}
inline void attach(lv_obj_t* obj, unsigned width) {
  auto* d=new Data();d->frame.header.cf=LV_IMG_CF_TRUE_COLOR;
  d->frame.header.w=40;d->frame.header.h=40;d->frame.data_size=sizeof(d->pixels);d->frame.data=d->pixels;
  lv_obj_set_user_data(obj,d);lv_obj_add_event_cb(obj,destroy,LV_EVENT_DELETE,d);
  d->target=lv_img_create(obj);lv_obj_set_size(d->target,40,40);lv_obj_center(d->target);
  lv_img_set_pivot(d->target,20,20);lv_img_set_zoom(d->target,width*256/40);
  lv_obj_add_flag(d->target,LV_OBJ_FLAG_HIDDEN);
}
inline int digit(char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1; }
inline void update(lv_obj_t* obj, const char* hex) {
  auto* d=static_cast<Data*>(lv_obj_get_user_data(obj));if(!d || !d->target) return;
  if(!hex || !hex[0]) { lv_obj_add_flag(d->target,LV_OBJ_FLAG_HIDDEN);d->previous.clear();return; }
  if(d->previous==hex || strlen(hex)!=sizeof(d->pixels)*2) return;
  for(size_t i=0;i<sizeof(d->pixels)*2;i++) if(digit(hex[i])<0) return;
  for(size_t i=0;i<sizeof(d->pixels);i++) d->pixels[i]=(digit(hex[i*2])<<4)|digit(hex[i*2+1]);
  d->previous=hex;lv_img_cache_invalidate_src(&d->frame);lv_img_set_src(d->target,&d->frame);
  lv_obj_clear_flag(d->target,LV_OBJ_FLAG_HIDDEN);lv_obj_invalidate(d->target);
}
}
