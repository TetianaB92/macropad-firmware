#include "image_sequence.h"
#include "frame_buffer.h"
#include <Arduino.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstring>
namespace ImageSequence {
struct Sequence {
  lv_obj_t* image;
  lv_obj_t* status;
  std::vector<std::string> ids;
  unsigned width, height, index = 0;
  uint32_t interval, shownAt = 0, retryAt = 0, requestedAt = 0, ticket = 0;
  std::vector<uint8_t*> cached;
  unsigned pendingIndex = 0;
  unsigned epoch = 0;
  std::string key;
  uint8_t* current = nullptr;
  uint8_t* next = nullptr;
  lv_img_dsc_t frame {};
};
static std::vector<Sequence*> sequences;
static uint32_t ticketCounter = 0;
// Share a bounded budget across active and inactive pages. Scene changes
// invalidate crops; switching pages retains already downloaded frames.
static size_t cachedBytes = 0;
static constexpr size_t cacheBudget = 12 * 1024 * 1024;
struct SavedFrames {
  std::string key;
  std::vector<std::string> ids;
  unsigned width, height;
  std::vector<uint8_t*> frames;
};
static std::vector<SavedFrames> savedPages;
static unsigned sceneEpoch=0;
static void releaseSaved(SavedFrames& page) {
  for(auto* bytes:page.frames) if(bytes) { releaseFrameBuffer(bytes);cachedBytes-=page.width*page.height*2; }
}
void beginScene() {
  ++sceneEpoch;
  for(auto& page:savedPages) releaseSaved(page);
  savedPages.clear();
}
static void show(Sequence* s,uint8_t* bytes);
static bool retained(Sequence* s, uint8_t* bytes) {
  return bytes && std::find(s->cached.begin(), s->cached.end(), bytes) != s->cached.end();
}
static void releaseTransient(Sequence* s, uint8_t* bytes) {
  if (!retained(s, bytes)) releaseFrameBuffer(bytes);
}
static void destroy(lv_event_t* event) {
  auto* sequence = static_cast<Sequence*>(lv_event_get_user_data(event));
  sequences.erase(std::remove(sequences.begin(), sequences.end(), sequence), sequences.end());
  lv_img_cache_invalidate_src(&sequence->frame);
  releaseTransient(sequence, sequence->current); releaseTransient(sequence, sequence->next);
  if(sequence->epoch==sceneEpoch && std::any_of(sequence->cached.begin(),sequence->cached.end(),[](uint8_t* p){return p!=nullptr;})) {
    savedPages.push_back({sequence->key,sequence->ids,sequence->width,sequence->height,std::move(sequence->cached)});
  } else {
    for (auto* bytes : sequence->cached) if (bytes) {
      releaseFrameBuffer(bytes); cachedBytes -= sequence->width * sequence->height * 2;
    }
  }
  delete sequence;
}
static void advance(lv_event_t* event) {
  auto* sequence = static_cast<Sequence*>(lv_event_get_user_data(event));
  sequence->shownAt = millis() - sequence->interval;
}
void attach(lv_obj_t* image, JsonArray ids, unsigned width, unsigned height, uint32_t intervalMs, const char* cacheKey) {
  if (sequences.size() >= 8 || width < 1 || height < 1 || width > 1024 || height > 600) return;
  auto* s = new Sequence();s->image=image;s->width=width;s->height=height;s->interval=std::max<uint32_t>(5000,std::min<uint32_t>(600000,intervalMs));
  for (JsonVariant id : ids) {
    const char* value=id.as<const char*>();if(!value || strlen(value)!=64 || strspn(value,"0123456789abcdef")!=64) continue;
    s->ids.emplace_back(value);if(s->ids.size()==24) break;
  }
  if(s->ids.empty()) { delete s;return; }
  s->epoch=sceneEpoch;s->key=cacheKey?cacheKey:"";
  s->cached.resize(s->ids.size(), nullptr);
  for(auto it=savedPages.begin();it!=savedPages.end();++it) {
    if(it->key==s->key && it->ids==s->ids && it->width==width && it->height==height) {
      s->cached=std::move(it->frames);savedPages.erase(it);break;
    }
  }
  s->frame.header.cf=LV_IMG_CF_TRUE_COLOR;s->frame.header.w=width;s->frame.header.h=height;s->frame.data_size=width*height*2;
  lv_obj_add_flag(image,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(image,destroy,LV_EVENT_DELETE,s);
  lv_obj_add_event_cb(image,advance,LV_EVENT_CLICKED,s);
  s->status=lv_label_create(image);
  lv_label_set_text(s->status,"Loading photos...");lv_obj_set_width(s->status,width>24?width-24:width);
  lv_obj_set_style_text_align(s->status,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_color(s->status,lv_color_hex(0xa1a1aa),0);lv_obj_center(s->status);
  sequences.push_back(s);
  // Initial loads need feedback; later loads retain the displayed frame.
  for(unsigned i=0;i<s->cached.size();++i) if(s->cached[i]) { s->index=i;show(s,s->cached[i]);break; }
}
static void show(Sequence* s,uint8_t* bytes) {
  uint8_t* previous=s->current;s->current=bytes;s->frame.data=bytes;
  lv_img_cache_invalidate_src(&s->frame);
  // LVGL skips work when the src pointer is unchanged, so drop then reattach
  // or the next album frame never appears.
  lv_img_set_src(s->image, nullptr);
  lv_img_set_src(s->image, &s->frame);
  lv_obj_invalidate(s->image);
  lv_obj_add_flag(s->status,LV_OBJ_FLAG_HIDDEN);
  releaseTransient(s,previous);s->shownAt=millis();
}
void tick() {
  const uint32_t now=millis();
  for(auto* s:sequences) {
    if(s->current && s->next && now-s->shownAt>=s->interval) {
      s->index=(s->index+1)%s->ids.size();show(s,s->next);s->next=nullptr;
    }
    if(s->ticket && now-s->requestedAt>45000U) {
      s->ticket=0;s->retryAt=now+5000;
      if(!s->current) {
        s->index=(s->pendingIndex+1)%s->ids.size();
        lv_label_set_text(s->status,"Photo download timed out. Retrying...");
      }
    }
    if(s->ticket || s->next || (s->current && s->ids.size()<2) || int32_t(now-s->retryAt)<0) continue;
    PhotoRequest request {};request.ticket=++ticketCounter;
    request.width=s->width;request.height=s->height;
    const unsigned target=s->current?(s->index+1)%s->ids.size():s->index;
    if (s->cached[target]) {
      if (!s->current) show(s, s->cached[target]); else s->next = s->cached[target];
      continue;
    }
    s->pendingIndex = target;
    snprintf(request.assetId,sizeof(request.assetId),"%s",s->ids[target].c_str());
    if(photoRequestQueue && xQueueSend(photoRequestQueue,&request,0)==pdTRUE) { s->ticket=request.ticket;s->requestedAt=now; }
  }
}
void receive(uint32_t ticket,uint8_t* bytes,size_t length) {
  for(auto* s:sequences) if(s->ticket==ticket) {
    s->ticket=0;
    if(!bytes || length!=s->width*s->height*2) {
      releaseFrameBuffer(bytes);s->retryAt=millis()+5000;
      if(!s->current) {
        // A missing first photo must not block every other photo in the album.
        s->index=(s->pendingIndex+1)%s->ids.size();
        lv_obj_clear_flag(s->status,LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s->status,"Photos unavailable. Retrying...");
      }
      return;
    }
    psramInvalidate(bytes,length);
    while(cachedBytes+length>cacheBudget && !savedPages.empty()) {
      releaseSaved(savedPages.front());savedPages.erase(savedPages.begin());
    }
    if (!s->cached[s->pendingIndex] && cachedBytes + length <= cacheBudget) {
      s->cached[s->pendingIndex] = bytes; cachedBytes += length;
    }
    if(!s->current) show(s,bytes);else s->next=bytes;
    return;
  }
  // A page/scene was removed or a newer request superseded this one.
  releaseFrameBuffer(bytes);
}
}
