// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
#include <cstdint>
namespace cell {
template<class Sample>struct AudioView {
 Sample*const*channels{};std::size_t channelCount{},frames{},offset{};
 Sample*channel(std::size_t c)const noexcept{assert(c<channelCount);return channels[c]?channels[c]+offset:nullptr;}
 AudioView slice(std::size_t begin,std::size_t count)const{if(begin>frames||count>frames-begin)throw std::out_of_range("Audio view range");return {channels,channelCount,count,offset+begin};}
};
// Contiguous planar ownership or a non-owning host-buffer view. Owning resize is
// non-realtime. Access, gain, copy, and view operations allocate no memory.
// Dimensions are size_t and checked against the allocator, never preset limits.
template<class Sample=float>class OwnedAudio {
 std::vector<Sample>storage_;std::vector<Sample*>pointers_;
 Sample*const*external_{};std::size_t channels_{},frames_{};
 Sample*const*data()const noexcept{return external_?external_:pointers_.data();}
 static bool needsReverse(const Sample*destination,const Sample*source,std::size_t count)noexcept{const auto d=reinterpret_cast<std::uintptr_t>(destination),s=reinterpret_cast<std::uintptr_t>(source);return d>s&&(d-s)/sizeof(Sample)<count;}
 void range(std::size_t c,std::size_t at,std::size_t count)const{if(c>=channels_||at>frames_||count>frames_-at)throw std::out_of_range("Audio buffer range");}
 public:
 OwnedAudio()=default;OwnedAudio(std::size_t c,std::size_t n){resize(c,n);}
 OwnedAudio(Sample*const*p,std::size_t c,std::size_t n){setDataToReferTo(p,c,n);}
 OwnedAudio(const OwnedAudio&other){resize(other.channels_,other.frames_);for(std::size_t c=0;c<channels_;++c)if(frames_)std::copy_n(other.getReadPointer(c),frames_,pointers_[c]);}
 OwnedAudio&operator=(const OwnedAudio&other){if(this!=&other){OwnedAudio copy(other);swap(copy);}return *this;}
 OwnedAudio(OwnedAudio&&other)noexcept{swap(other);}OwnedAudio&operator=(OwnedAudio&&other)noexcept{if(this!=&other){OwnedAudio empty;swap(empty);swap(other);}return *this;}
 void swap(OwnedAudio&b)noexcept{storage_.swap(b.storage_);pointers_.swap(b.pointers_);std::swap(external_,b.external_);std::swap(channels_,b.channels_);std::swap(frames_,b.frames_);}
 void resize(std::size_t channels,std::size_t frames){setSize(channels,frames,false,true);}
 void setSize(std::size_t channels,std::size_t frames,bool keep=false,bool clearExtra=true,bool avoidReallocating=false){
  if(!external_&&channels==channels_&&frames==frames_){if(!keep&&clearExtra)clear();return;}
  if(channels>pointers_.max_size()||(frames&&channels>storage_.max_size()/frames))throw std::length_error("Audio allocation size overflow");
  const auto count=channels*frames;
  if(!external_&&!keep&&avoidReallocating&&count<=storage_.capacity()&&channels<=pointers_.capacity()){
   storage_.resize(count);pointers_.resize(channels);for(std::size_t c=0;c<channels;++c)pointers_[c]=frames?storage_.data()+c*frames:nullptr;channels_=channels;frames_=frames;if(clearExtra)clear();return;
  }
  std::vector<Sample>storage(count,Sample{});std::vector<Sample*>pointers(channels);
  for(std::size_t c=0;c<channels;++c)pointers[c]=frames?storage.data()+c*frames:nullptr;
  if(keep){const auto n=std::min(frames,frames_),c=std::min(channels,channels_);for(std::size_t channel=0;channel<c;++channel)if(n)std::copy_n(getReadPointer(channel),n,pointers[channel]);}
  storage_.swap(storage);pointers_.swap(pointers);external_=nullptr;channels_=channels;frames_=frames;
 }
 void setDataToReferTo(Sample*const*source,std::size_t channels,std::size_t frames){if(channels&&!source)throw std::invalid_argument("Audio channel pointers missing");for(std::size_t c=0;c<channels;++c)if(frames&&!source[c])throw std::invalid_argument("Audio channel is null");external_=source;channels_=channels;frames_=frames;}
 std::size_t channelCount()const noexcept{return channels_;}std::size_t frameCount()const noexcept{return frames_;}
 std::size_t getNumChannels()const noexcept{return channels_;}std::size_t getNumSamples()const noexcept{return frames_;}
 Sample*getWritePointer(std::size_t c,std::size_t at=0){range(c,at,0);auto*p=data()[c];return p?p+at:nullptr;}
 const Sample*getReadPointer(std::size_t c,std::size_t at=0)const{range(c,at,0);auto*p=data()[c];return p?p+at:nullptr;}
 Sample*const*getArrayOfWritePointers()noexcept{return data();}const Sample*const*getArrayOfReadPointers()const noexcept{return data();}
 Sample getSample(std::size_t c,std::size_t at)const{range(c,at,1);return data()[c][at];}
 void setSample(std::size_t c,std::size_t at,Sample v){range(c,at,1);data()[c][at]=v;}
 void addSample(std::size_t c,std::size_t at,Sample v){range(c,at,1);data()[c][at]+=v;}
 AudioView<Sample>view()noexcept{return {data(),channels_,frames_,0};}
 void clear()noexcept{for(std::size_t c=0;c<channels_;++c)if(frames_)std::fill_n(data()[c],frames_,Sample{});}
 void clear(std::size_t c,std::size_t start,std::size_t count){range(c,start,count);if(count)std::fill_n(getWritePointer(c,start),count,Sample{});}
 void clear(std::size_t start,std::size_t count){for(std::size_t c=0;c<channels_;++c)clear(c,start,count);}
 void copyFrom(std::size_t c,std::size_t start,const Sample*source,std::size_t count,Sample gain=Sample(1)){range(c,start,count);if(count&&!source)throw std::invalid_argument("Audio copy source missing");if(!count)return;auto*dest=getWritePointer(c,start);if(gain==Sample(1)){std::memmove(dest,source,count*sizeof(Sample));return;}if(needsReverse(dest,source,count)){for(auto n=count;n>0;--n)dest[n-1]=source[n-1]*gain;}else for(std::size_t n=0;n<count;++n)dest[n]=source[n]*gain;}
 void copyFrom(std::size_t c,std::size_t start,const OwnedAudio&source,std::size_t sc,std::size_t at,std::size_t count){source.range(sc,at,count);copyFrom(c,start,source.getReadPointer(sc,at),count);}
 template<class Other>void makeCopyOf(const OwnedAudio<Other>&source,bool=false){setSize(source.getNumChannels(),source.getNumSamples());for(std::size_t c=0;c<channels_;++c)for(std::size_t n=0;n<frames_;++n)data()[c][n]=Sample(source.getSample(c,n));}
 void addFrom(std::size_t c,std::size_t start,const Sample*source,std::size_t count,Sample gain=Sample(1)){range(c,start,count);if(count&&!source)throw std::invalid_argument("Audio sum source missing");if(!count)return;auto*dest=getWritePointer(c,start);if(needsReverse(dest,source,count)){for(auto n=count;n>0;--n)dest[n-1]+=source[n-1]*gain;}else for(std::size_t n=0;n<count;++n)dest[n]+=source[n]*gain;}
 void addFrom(std::size_t c,std::size_t start,const OwnedAudio&source,std::size_t sc,std::size_t at,std::size_t count,Sample gain=Sample(1)){source.range(sc,at,count);addFrom(c,start,source.getReadPointer(sc,at),count,gain);}
 void applyGain(std::size_t c,std::size_t start,std::size_t count,Sample gain){range(c,start,count);auto*p=getWritePointer(c,start);for(std::size_t n=0;n<count;++n)p[n]*=gain;}
 void applyGain(Sample gain){for(std::size_t c=0;c<channels_;++c)applyGain(c,0,frames_,gain);}
 void applyGain(std::size_t start,std::size_t count,Sample gain){for(std::size_t c=0;c<channels_;++c)applyGain(c,start,count,gain);}
 void applyGainRamp(std::size_t c,std::size_t start,std::size_t count,Sample first,Sample last){range(c,start,count);if(!count)return;auto*p=getWritePointer(c,start);const Sample delta=(last-first)/Sample(count);for(std::size_t n=0;n<count;++n){p[n]*=first;first+=delta;}}
};
}
