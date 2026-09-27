#include "flip_text.h"
#include "fonts/digits_256.h"
#include "fonts/pixel_digits.h"
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

namespace FlipText {
struct Face {
  lv_obj_t *obj, *canvas, *image;
  lv_color_t *oldPixels=nullptr, *nextPixels=nullptr, *frame=nullptr;
  lv_img_dsc_t descriptor {};
  int w=0, h=0;
  int fontSize=0;
  bool pixel=true;
};

void setPixelFont(lv_obj_t* obj,bool pixel) {
  auto* f=static_cast<Face*>(lv_obj_get_user_data(obj));
  if(f) f->pixel=pixel;
}

void setFontSize(lv_obj_t* obj, int pixels) {
  auto* f=static_cast<Face*>(lv_obj_get_user_data(obj));
  if(f) f->fontSize=LV_CLAMP(0,pixels,512);
}

// Scale coverage once when the value changes. Animation reuses the RGB565 faces.
static bool drawDigit(Face* f, const char* text, lv_color_t color) {
  if(f->pixel && text[0] && !text[1] && ((text[0]>='0' && text[0]<='9') || text[0]=='-')) {
    const auto* rows=pixel_digits[text[0]=='-'?10:text[0]-'0'];
    const int scale=LV_MAX(1,LV_MIN(f->w*82/500,f->h*78/700));
    const int x0=(f->w-scale*5)/2,y0=(f->h-scale*7)/2;
    for(int y=0;y<7;++y) for(int x=0;x<5;++x) if(rows[y]&(1<<(4-x)))
      for(int dy=0;dy<scale;++dy) for(int dx=0;dx<scale;++dx) {
        const int px=x0+x*scale+dx,py=y0+y*scale+dy;
        if(px>=0 && py>=0 && px<f->w && py<f->h) f->nextPixels[py*f->w+px]=color;
      }
    return true;
  }
  if(!f->fontSize || !text[0] || text[1] || text[0]<'0' || text[0]>'9') return false;
  const auto& glyph=DigitAtlas::glyphs[text[0]-'0'];
  int w=glyph.width*f->fontSize/256, h=glyph.height*f->fontSize/256;
  if(w<1||h<1) return true;
  const int x0=(f->w-w)/2,y0=(f->h-h)/2;
  for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
    if(x+x0<0||x+x0>=f->w||y+y0<0||y+y0>=f->h) continue;
    const size_t index=size_t(y*glyph.height/h)*glyph.width+x*glyph.width/w;
    const uint8_t packed=DigitAtlas::pixels[glyph.offset+index/2];
    const uint8_t alpha=((index%2)?(packed&15):(packed>>4))*17;
    auto& target=f->nextPixels[(y+y0)*f->w+x+x0];
    if(alpha) target=lv_color_mix(color,target,alpha);
  }
  return true;
}

static void compose(void* object, int32_t phase) {
  auto* f=static_cast<Face*>(lv_obj_get_user_data(static_cast<lv_obj_t*>(object)));
  if (!f || !f->frame) return;
  const int half=f->h/2;
  memcpy(f->frame,f->nextPixels,size_t(f->w)*f->h*sizeof(lv_color_t));
  if(phase<1000) {
    // New upper face behind old lower face; fold around their shared hinge.
    memcpy(f->frame+half*f->w,f->oldPixels+half*f->w,size_t(f->w)*(f->h-half)*sizeof(lv_color_t));
    const bool upper=phase<500;
    const int sourceHeight=upper?half:f->h-half;
    const int height=sourceHeight*(upper?500-phase:phase-500)/500;
    const int top=upper?half-height:half;
    const auto* source=upper?f->oldPixels:f->nextPixels+half*f->w;
    for(int y=0;y<height;++y)
      memcpy(f->frame+(top+y)*f->w,source+(y*sourceHeight/height)*f->w,size_t(f->w)*sizeof(lv_color_t));
  }
  // Fine hinge line remains in the same place for every frame.
  for(int x=0;x<f->w;++x) f->frame[half*f->w+x]=lv_color_hex(0x111113);
  lv_img_cache_invalidate_src(&f->descriptor);
  lv_obj_invalidate(f->image);
}

static void deleted(lv_event_t* event) {
  auto* obj=lv_event_get_target(event);
  auto* f=static_cast<Face*>(lv_obj_get_user_data(obj));
  if(!f) return;
  lv_anim_del(obj,compose);
  lv_img_cache_invalidate_src(&f->descriptor);
  heap_caps_free(f->oldPixels);heap_caps_free(f->nextPixels);heap_caps_free(f->frame);
  delete f;
}

lv_obj_t* create(lv_obj_t* parent) {
  auto* obj=lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_clear_flag(obj,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
  auto* f=new Face{};
  f->obj=obj;f->canvas=lv_canvas_create(obj);f->image=lv_img_create(obj);
  lv_obj_add_flag(f->canvas,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_user_data(obj,f);
  lv_obj_add_event_cb(obj,deleted,LV_EVENT_DELETE,nullptr);
  return obj;
}

bool set(lv_obj_t* obj, const char* text, bool animate) {
  auto* f=static_cast<Face*>(lv_obj_get_user_data(obj));
  if(!f) return false;
  lv_obj_update_layout(obj);
  const int w=lv_obj_get_content_width(obj),h=lv_obj_get_content_height(obj);
  // Full-width four-digit clocks need larger faces than six-digit clocks.
  if(w<2||h<2||w>512||h>512) return false;
  lv_anim_del(obj,compose);
  if(f->w!=w||f->h!=h) {
    const size_t bytes=size_t(w)*h*sizeof(lv_color_t);
    // Keep Wi-Fi's internal DMA heap available. Animated faces live in PSRAM.
    auto allocate = [bytes]() { return static_cast<lv_color_t*>(
      heap_caps_calloc(1,bytes,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); };
    auto* oldPixels=allocate();auto* nextPixels=allocate();auto* frame=allocate();
    if(!oldPixels||!nextPixels||!frame) {
      heap_caps_free(oldPixels);heap_caps_free(nextPixels);heap_caps_free(frame);
      return false; // Retain a previously valid face on allocation failure.
    }
    lv_img_cache_invalidate_src(&f->descriptor);
    heap_caps_free(f->oldPixels);heap_caps_free(f->nextPixels);heap_caps_free(f->frame);
    f->w=w;f->h=h;
    f->oldPixels=oldPixels;f->nextPixels=nextPixels;f->frame=frame;
    animate=false;
    f->descriptor.header.cf=LV_IMG_CF_TRUE_COLOR;
    f->descriptor.header.w=w;f->descriptor.header.h=h;
    f->descriptor.data_size=bytes;f->descriptor.data=reinterpret_cast<uint8_t*>(f->frame);
    lv_img_set_src(f->image,&f->descriptor);
  }
  if(!f->oldPixels||!f->nextPixels||!f->frame) return false;
  memcpy(f->oldPixels,f->nextPixels,size_t(w)*h*sizeof(lv_color_t));
  lv_canvas_set_buffer(f->canvas,f->nextPixels,w,h,LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(f->canvas,lv_obj_get_style_bg_color(obj,0),LV_OPA_COVER);
  lv_draw_rect_dsc_t bg;lv_draw_rect_dsc_init(&bg);
  bg.bg_color=lv_obj_get_style_bg_color(obj,0);
  bg.bg_grad.dir=LV_GRAD_DIR_VER;bg.bg_grad.stops_count=2;
  bg.bg_grad.stops[0].color=bg.bg_color;bg.bg_grad.stops[0].frac=0;
  bg.bg_grad.stops[1].color=lv_obj_get_style_bg_grad_color(obj,0);bg.bg_grad.stops[1].frac=255;
  lv_canvas_draw_rect(f->canvas,0,0,w,h,&bg);
  lv_draw_label_dsc_t label;lv_draw_label_dsc_init(&label);
  label.font=lv_obj_get_style_text_font(obj,0);label.color=lv_obj_get_style_text_color(obj,0);
  label.align=LV_TEXT_ALIGN_CENTER;
  if(!drawDigit(f,text,label.color))
    lv_canvas_draw_text(f->canvas,0,(h-lv_font_get_line_height(label.font))/2,w,&label,text);
  compose(obj,animate?0:1000);
  if(animate) {
    lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,obj);lv_anim_set_values(&a,0,1000);
    lv_anim_set_time(&a,300);lv_anim_set_exec_cb(&a,compose);lv_anim_start(&a);
  }
  return true;
}
}
