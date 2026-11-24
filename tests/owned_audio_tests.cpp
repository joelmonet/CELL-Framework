// Original Cell Framework implementation; see LICENSE.
#include <cell/audio.hpp>
#include <iostream>
namespace {void need(bool v,const char*m){if(!v)throw std::runtime_error(m);}}
int main(){try{
 cell::AudioBuffer<float>wide(300,128);need(wide.channelCount()==300&&wide.frameCount()==128,"Fixed channel ceiling remains");wide.setSample(299,127,.375f);need(wide.getSample(299,127)==.375f,"High channel address incorrect");
 cell::AudioBuffer<float>longBuffer(1,(std::size_t(1)<<24)+1);longBuffer.setSample(0,longBuffer.frameCount()-1,.25f);need(longBuffer.getSample(0,longBuffer.frameCount()-1)==.25f,"Fixed frame ceiling remains");
 cell::AudioBuffer<float>a(2,4);for(std::size_t c=0;c<2;++c)for(std::size_t n=0;n<4;++n)a.setSample(c,n,float(c*10+n));auto copy=a;copy.setSample(0,0,99);need(a.getSample(0,0)==0&&copy.getSample(0,0)==99,"Owned copy aliases the source");a.setSize(3,7,true,true);need(a.getSample(1,3)==13&&a.getSample(1,6)==0&&a.getSample(2,0)==0,"Preserving resize changed existing channels or leaked extra samples");
 float samples[]={1,2,3,4,5,6};float*channels[]={samples};cell::AudioBuffer<float>view(channels,1,6);view.applyGain(.5f);need(samples[5]==3&&view.getSample(0,0)==.5f,"Borrowed view copied instead of addressing host samples");view.copyFrom(0,1,view,0,0,5);need(samples[1]==.5f&&samples[5]==2.5f,"Overlapping audio copy corrupted its source");view.clear(0,2,2);need(samples[2]==0&&samples[3]==0&&samples[4]==2,"Audio clear escaped its selected range");
 auto moved=std::move(copy);need(copy.channelCount()==0&&copy.frameCount()==0&&moved.getSample(0,0)==99,"Move left stale source pointers/dimensions");cell::AudioBuffer<float>empty(32,0);need(empty.getReadPointer(31)==nullptr&&empty.block().frames==0,"Zero-length channels expose invalid pointers");
 bool overflow=false;try{empty.resize(std::numeric_limits<std::size_t>::max(),4);}catch(const std::length_error&){overflow=true;}need(overflow&&empty.channelCount()==32,"Overflowing resize changed the existing buffer");
 const std::size_t huge=std::size_t(std::numeric_limits<std::uint32_t>::max())+1;cell::AudioBuffer<float>metadata(channels,1,huge);need(metadata.view().frames==huge,"Wide source span was truncated");bool blockRejected=false;try{metadata.block();}catch(const std::length_error&){blockRejected=true;}need(blockRejected,"A host-sized block silently truncated a wide source span");
 std::cout<<"PASS native audio storage: 300 channels, frames beyond 2^24, wide source extents, host views, overlap, preserving resize, zero frames, moves and overflow admission\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
