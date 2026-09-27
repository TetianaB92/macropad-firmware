#include "remote_image.h"
#include "frame_buffer.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>
namespace RemoteImage {
struct Image {
  lv_obj_t* object; lv_obj_t* label; lv_img_dsc_t frame{};
  SnapshotRequest request{};
  uint8_t* current=nullptr;
  uint32_t ticket=0, requestedAt=0, due=0, interval=1000;
};
static std::vector<Image*> images;
static std::atomic<uint32_t> epoch{0};
static uint32_t serial=0;
void beginPage() { ++epoch; }
bool isCurrent(uint32_t value) { return value==epoch.load(); }
static void destroy(lv_event_t* event) {
  auto* image=static_cast<Image*>(lv_event_get_user_data(event));
  images.erase(std::remove(images.begin(),images.end(),image),images.end());
  lv_img_cache_invalidate_src(&image->frame);releaseFrameBuffer(image->current);delete image;
}
void attach(lv_obj_t* object,const char* url,unsigned width,unsigned height,uint32_t refreshMs,bool cover) {
  if(images.size()>=4 || !url || !url[0] || strlen(url)>=512 ||
     (strncmp(url,"http://",7) && strncmp(url,"https://",8)) || !width || !height || width>1024 || height>600) return;
  auto* image=new Image();image->object=object;image->request.epoch=epoch.load();
  image->request.width=width;image->request.height=height;image->request.cover=cover;
  snprintf(image->request.url,sizeof(image->request.url),"%s",url);
  image->interval=std::max<uint32_t>(1000,std::min<uint32_t>(10000,refreshMs));
  image->frame.header.cf=LV_IMG_CF_TRUE_COLOR;image->frame.header.w=width;image->frame.header.h=height;image->frame.data_size=width*height*2;
  image->label=lv_label_create(object);lv_label_set_text(image->label,"Connecting...");
  lv_obj_set_width(image->label,width>24?width-24:width);lv_obj_set_style_text_align(image->label,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_style_text_color(image->label,lv_color_hex(0xa1a1aa),0);lv_obj_center(image->label);
  lv_obj_add_event_cb(object,destroy,LV_EVENT_DELETE,image);images.push_back(image);
}
void tick() {
  const auto now=millis();
  for(auto* image:images) {
    if(image->ticket && now-image->requestedAt>12000) { image->ticket=0;image->due=now+5000; }
    if(image->ticket || int32_t(now-image->due)<0 || !isCurrent(image->request.epoch)) continue;
    image->request.ticket=++serial;
    if(snapshotRequestQueue && xQueueSend(snapshotRequestQueue,&image->request,0)==pdTRUE) {
      image->ticket=image->request.ticket;image->requestedAt=now;
    }
  }
}
void receive(uint32_t ticket,uint8_t* bytes,size_t length) {
  for(auto* image:images) if(image->ticket==ticket) {
    image->ticket=0;
    if(!bytes || length!=image->frame.data_size) {
      releaseFrameBuffer(bytes);image->due=millis()+5000;
      lv_label_set_text(image->label,"Image unavailable. Retrying...");
      lv_obj_clear_flag(image->label,LV_OBJ_FLAG_HIDDEN);return;
    }
    psramInvalidate(bytes,length);auto* old=image->current;image->current=bytes;image->frame.data=bytes;
    lv_img_cache_invalidate_src(&image->frame);lv_img_set_src(image->object,&image->frame);releaseFrameBuffer(old);
    lv_obj_add_flag(image->label,LV_OBJ_FLAG_HIDDEN);image->due=millis()+image->interval;return;
  }
  releaseFrameBuffer(bytes); // Scene/page replaced while the frame was in flight.
}
}
