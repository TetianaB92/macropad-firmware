#include "remote_image.h"
#include "frame_buffer.h"
#include <src/extra/libs/sjpg/tjpgd.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include <driver/jpeg_decode.h>
#endif
#include <algorithm>
#include <cstring>
#if LV_USE_SJPG
namespace {
struct Source {
  const uint8_t* jpeg;size_t length,pos=0;
  uint16_t* pixels=nullptr;
  int width,height,sourceWidth=0,sourceHeight=0,drawWidth=0,drawHeight=0,left=0,top=0;
};
size_t input(JDEC* decoder,uint8_t* buffer,size_t count) {
  auto* s=static_cast<Source*>(decoder->device);count=std::min(count,s->length-s->pos);
  if(buffer) memcpy(buffer,s->jpeg+s->pos,count);s->pos+=count;return count;
}
int output(JDEC* decoder,void* bitmap,JRECT* rect) {
  auto* s=static_cast<Source*>(decoder->device);
  const int blockWidth=rect->right-rect->left+1;
  const auto ceilDiv=[](int a,int b){return (a+b-1)/b;};
  const int x1=std::max(0,s->left+ceilDiv(rect->left*s->drawWidth,s->sourceWidth));
  const int x2=std::min(s->width,s->left+ceilDiv((rect->right+1)*s->drawWidth,s->sourceWidth));
  const int y1=std::max(0,s->top+ceilDiv(rect->top*s->drawHeight,s->sourceHeight));
  const int y2=std::min(s->height,s->top+ceilDiv((rect->bottom+1)*s->drawHeight,s->sourceHeight));
  for(int y=y1;y<y2;y++) for(int x=x1;x<x2;x++) {
    const int sx=(x-s->left)*s->sourceWidth/s->drawWidth,sy=(y-s->top)*s->sourceHeight/s->drawHeight;
    const size_t i=(sy-rect->top)*blockWidth+(sx-rect->left);
#if JD_FORMAT == 1
    s->pixels[y*s->width+x]=static_cast<uint16_t*>(bitmap)[i];
#else
    auto* p=static_cast<uint8_t*>(bitmap)+i*3;
    s->pixels[y*s->width+x]=((p[0]&0xf8)<<8)|((p[1]&0xfc)<<3)|(p[2]>>3);
#endif
  }
  return 1;
}
}
uint8_t* RemoteImage::decode(const uint8_t* jpeg,size_t length,unsigned width,unsigned height,bool cover) {
  if(!jpeg || length<4 || length>512*1024 || !width || !height || width>1024 || height>600) return nullptr;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  // Exact-size frames use the P4 JPEG peripheral. The software decoder remains
  // the fallback for crop/fit operations because the peripheral does not scale.
  jpeg_decode_picture_info_t info{};
  jpeg_decoder_handle_t engine=nullptr;
  jpeg_decode_engine_cfg_t engineCfg{.intr_priority=0,.timeout_ms=100};
  jpeg_decode_cfg_t decodeCfg{.output_format=JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order=JPEG_DEC_RGB_ELEMENT_ORDER_RGB,.conv_std=JPEG_YUV_RGB_CONV_STD_BT601};
  if(jpeg_decoder_get_info(jpeg,length,&info)==ESP_OK && info.width==width && info.height==height &&
     (width % 16u)==0 && (height % 16u)==0 &&
     jpeg_new_decoder_engine(&engineCfg,&engine)==ESP_OK) {
    auto* output=allocFrameBuffer(size_t(width)*height*2);uint32_t outputSize=0;
    if(output && jpeg_decoder_process(engine,&decodeCfg,jpeg,length,output,width*height*2,&outputSize)==ESP_OK &&
       outputSize==width*height*2) { psramWriteback(output,outputSize);jpeg_del_decoder_engine(engine);return output; }
    releaseFrameBuffer(output);jpeg_del_decoder_engine(engine);
  }
#endif
  uint8_t work[4096];JDEC decoder{};Source s{jpeg,length,0,nullptr,int(width),int(height)};
  if(jd_prepare(&decoder,input,work,sizeof(work),&s)!=JDR_OK || !decoder.width || !decoder.height ||
     decoder.width>1280 || decoder.height>1024) return nullptr;
  s.sourceWidth=decoder.width;s.sourceHeight=decoder.height;
  const bool widthFit=uint64_t(width)*decoder.height<=uint64_t(height)*decoder.width;
  if(widthFit != cover) { s.drawWidth=width;s.drawHeight=std::max(1,int(uint64_t(width)*decoder.height/decoder.width)); }
  else { s.drawHeight=height;s.drawWidth=std::max(1,int(uint64_t(height)*decoder.width/decoder.height)); }
  s.left=(int(width)-s.drawWidth)/2;s.top=(int(height)-s.drawHeight)/2;
  const size_t size=size_t(width)*height*2;
  auto* bytes=allocFrameBuffer(size);if(!bytes) return nullptr;
  memset(bytes,0,size);s.pixels=reinterpret_cast<uint16_t*>(bytes);
  if(jd_decomp(&decoder,output,0)!=JDR_OK) { releaseFrameBuffer(bytes);return nullptr; }
  psramWriteback(bytes,size);return bytes;
}

#else
uint8_t* RemoteImage::decode(const uint8_t*,size_t,unsigned,unsigned,bool) { return nullptr; }
#endif
