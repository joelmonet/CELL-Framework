// SPDX-License-Identifier: Apache-2.0
#include <cell/wavetable.hpp>
#include <cell/dsp.hpp>
#include <cell/core.hpp>
#include <cell/json.hpp>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>
#include <charconv>
#include <memory>
namespace cell::dsp {
namespace {
std::uint32_t u32(std::span<const std::byte> b,std::size_t at,unsigned size=4){if(at>b.size()||size>b.size()-at)throw std::invalid_argument("Truncated wavetable");std::uint32_t value=0;for(unsigned j=0;j<size;++j)value|=std::uint32_t(std::to_integer<unsigned char>(b[at+j]))<<(j*8);return value;}
bool tag(std::span<const std::byte>b,std::size_t at,const char* name){return at<=b.size()&&b.size()-at>=4&&std::memcmp(b.data()+at,name,4)==0;}
float number(std::span<const std::byte>b,std::size_t at,unsigned bits,bool fp){const auto raw=u32(b,at,bits/8);if(fp){const auto x=std::bit_cast<float>(raw);if(!std::isfinite(x))throw std::invalid_argument("Nonfinite wavetable sample");return x;}if(bits==8)return (int(raw)-128)/128.f;const auto value=std::int32_t(raw<<(32-bits))>>(32-bits);return float(double(value)/std::ldexp(1.,bits-1));}
std::vector<std::byte> base64(std::string_view text){std::vector<std::byte> out;unsigned bits=0,value=0;bool padding=false;for(unsigned char c:text){if(c=='='){padding=true;continue;}if(c==' '||c=='\n'||c=='\r'||c=='\t')continue;if(padding)throw std::invalid_argument("Invalid wavetable base64 padding");int x=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;if(x<0)throw std::invalid_argument("Invalid wavetable base64");value=(value<<6)|unsigned(x);bits+=6;if(bits>=8){bits-=8;out.push_back(std::byte((value>>bits)&255));}}return out;}
std::vector<float> vitalFrames(const json::Value& root){
 const json::Value* source=&root;
 if(const auto* groups=root["groups"].array()){
  if(groups->size()!=1)throw std::invalid_argument("Multiple procedural .vitaltable groups require a rendered WAV export");
  const auto* components=(*groups)[0]["components"].array();if(!components||components->size()!=1)throw std::invalid_argument("Procedural .vitaltable modifiers require a rendered WAV export");source=&(*components)[0];
 }
 const auto* rows=(*source)["keyframes"].array();if(!rows||rows->empty()||rows->size()>Wavetable::maximumFrames)throw std::invalid_argument("No supported baked .vitaltable frames; export the wavetable as WAV");
 const auto integer=[&](const char* key,int fallback){const auto* value=(*source)[key].number();if(!value)return fallback;if(*value!=std::floor(*value)||*value<0||*value>1)throw std::invalid_argument("Invalid .vitaltable interpolation");return int(*value);};
 const int interpolation=integer("interpolation",0),style=integer("interpolation_style",1);
 if(interpolation<0||interpolation>1||style<0||style>1)throw std::invalid_argument("Cubic/procedural .vitaltable interpolation requires a rendered WAV export");
 struct Frame{double position{};std::vector<float> samples;std::vector<std::complex<float>> spectrum;};std::vector<Frame> frames;FFT fft(Wavetable::cycleSize);
 for(const auto& row:*rows){const auto* position=row["position"].number();if(!position||*position<0||*position>256)throw std::invalid_argument("Invalid .vitaltable frame position");const auto bytes=base64(row["wave_data"].text());if(bytes.size()!=Wavetable::cycleSize*4)throw std::invalid_argument("Only baked 2048-sample .vitaltable frames can be imported");Frame frame;frame.position=*position;frame.samples.resize(Wavetable::cycleSize);for(unsigned n=0;n<Wavetable::cycleSize;++n)frame.samples[n]=number(bytes,n*4,32,true);if(interpolation){frame.spectrum.resize(Wavetable::cycleSize);for(unsigned n=0;n<Wavetable::cycleSize;++n)frame.spectrum[n]={frame.samples[n],0};fft.transform(frame.spectrum);}frames.push_back(std::move(frame));}
 std::stable_sort(frames.begin(),frames.end(),[](const Frame& a,const Frame& b){return a.position<b.position;});for(unsigned n=1;n<frames.size();++n)if(frames[n-1].position==frames[n].position)throw std::invalid_argument("Duplicate .vitaltable frame positions");
 if(frames.size()==1)return std::move(frames[0].samples);
 // Bake the admitted source into a uniform cycle bank. Frequency morphing
 // interpolates magnitude and shortest phase; time morphing interpolates samples.
 std::vector<float> result(Wavetable::maximumFrames*Wavetable::cycleSize);std::vector<std::complex<float>> bins(Wavetable::cycleSize);
 for(unsigned f=0;f<Wavetable::maximumFrames;++f){const double position=f;auto right=std::upper_bound(frames.begin(),frames.end(),position,[](double p,const Frame& frame){return p<frame.position;});const auto& b=right==frames.end()?frames.back():*right;const auto& a=right==frames.begin()?frames.front():*(right-1);const float t=style&&b.position>a.position?float(std::clamp((position-a.position)/(b.position-a.position),0.,1.)):0;
  if(interpolation){for(unsigned n=0;n<Wavetable::cycleSize;++n){const auto x=a.spectrum[n],y=b.spectrum[n];const float phase=std::arg(x)+t*std::remainder(std::arg(y)-std::arg(x),float(2*std::numbers::pi));bins[n]=std::polar(std::abs(x)+t*(std::abs(y)-std::abs(x)),phase);}fft.transform(bins,true);for(unsigned n=0;n<Wavetable::cycleSize;++n)result[f*Wavetable::cycleSize+n]=bins[n].real();}
  else for(unsigned n=0;n<Wavetable::cycleSize;++n)result[f*Wavetable::cycleSize+n]=a.samples[n]+t*(b.samples[n]-a.samples[n]);
 }return result;
}
}
Wavetable Wavetable::import(std::span<const std::byte> b,unsigned requested){
 if(b.empty()||b.size()>64*1024*1024)throw std::invalid_argument("Wavetable must contain at most 64 MiB");
 std::vector<float> samples;unsigned frame=requested;
 if(tag(b,0,"vawt")){
  frame=u32(b,4);const auto count=u32(b,8,2),flags=u32(b,10,2);if(flags&~15u)throw std::invalid_argument("Unsupported .wt encoding flags; export as WAV");if(flags&3)throw std::invalid_argument("This .wt is a sampled instrument, not a cyclic wavetable");
  if(!frame||frame>16384||!count||count>maximumFrames)throw std::invalid_argument("Invalid .wt dimensions");const unsigned bits=flags&4?16:32;const bool fp=!(flags&4);const auto total=std::size_t(frame)*count;if(total>(b.size()-12)/(bits/8))throw std::invalid_argument("Truncated .wt audio");samples.resize(total);for(std::size_t i=0;i<total;++i)samples[i]=number(b,12+i*(bits/8),bits,fp)*((flags&4)&&!(flags&8)?2.f:1.f);
 }else if(tag(b,0,"RIFF")&&tag(b,8,"WAVE")){
  unsigned format=0,channels=0,bits=0,align=0;std::span<const std::byte> data;
  const auto end=std::size_t(u32(b,4))+8;if(end>b.size()||end<12)throw std::invalid_argument("Truncated WAV file");
  for(std::size_t at=12;at+8<=end;){const auto length=u32(b,at+4);const auto start=at+8;if(length>end-start)throw std::invalid_argument("Truncated WAV chunk");
   if(tag(b,at,"fmt ")){if(length<16)throw std::invalid_argument("Invalid WAV format");format=u32(b,start,2);channels=u32(b,start+2,2);align=u32(b,start+12,2);bits=u32(b,start+14,2);if(format==0xfffe&&length>=40)format=u32(b,start+24,2);}
   if(tag(b,at,"data"))data=b.subspan(start,length);
   if(tag(b,at,"clm ")&&!requested){const std::string text(reinterpret_cast<const char*>(b.data()+start),length);if(const auto marker=text.find("<!>");marker!=std::string::npos){unsigned value{};const auto first=text.data()+marker+3;const auto parsed=std::from_chars(first,text.data()+text.size(),value);if(parsed.ec!=std::errc{}||parsed.ptr==first||value<16||value>16384)throw std::invalid_argument("Invalid .wav cycle metadata");frame=value;}}
   at=start+length+(length&1);
  }
  if((format!=1&&format!=3)||!channels||channels>2||!(bits==8||bits==16||bits==24||bits==32)||(format==3&&bits!=32)||align!=channels*(bits/8)||data.empty()||data.size()%align)throw std::invalid_argument("Wavetable WAV must be PCM8/16/24/32 or float32, mono/stereo");
  samples.resize(data.size()/align);for(std::size_t i=0;i<samples.size();++i){double sum=0;for(unsigned c=0;c<channels;++c)sum+=number(data,i*align+c*(bits/8),bits,format==3);samples[i]=float(sum/channels);}
  if(!frame)frame=samples.size()<cycleSize?unsigned(samples.size()):cycleSize;
 }else{
  const auto root=json::read({reinterpret_cast<const char*>(b.data()),b.size()},json::Limits{64u*1024*1024,262144,64u*1024*1024,48});samples=vitalFrames(root);frame=cycleSize;
 }
 return fromCycles(samples,frame);
}
Wavetable Wavetable::fromCycles(std::span<const float> source,unsigned frame){
 if(frame<16||frame>16384||source.empty()||source.size()%frame||source.size()/frame>maximumFrames)throw std::invalid_argument("Wavetable cycles must be 16..16384 samples and divide the audio exactly");
 std::unique_ptr<FFT> resample;if(frame!=cycleSize&&!(frame&(frame-1)))resample=std::make_unique<FFT>(frame);std::vector<std::complex<float>> original(resample?frame:0),resized(resample?cycleSize:0);FFT cycleFFT(cycleSize);
 Wavetable result;result.frames_=unsigned(source.size()/frame);result.cycles_.resize(std::size_t(result.frames_)*cycleSize);float peak=0,sourcePeak=0;
 for(unsigned f=0;f<result.frames_;++f){double mean=0;for(unsigned n=0;n<frame;++n){const auto x=source[std::size_t(f)*frame+n];if(!std::isfinite(x))throw std::invalid_argument("Nonfinite wavetable sample");mean+=x;}mean/=frame;for(unsigned n=0;n<frame;++n)sourcePeak=std::max(sourcePeak,float(std::abs(source[std::size_t(f)*frame+n]-mean)));if(resample){for(unsigned n=0;n<frame;++n)original[n]={float(source[std::size_t(f)*frame+n]-mean),0};resample->transform(original);std::fill(resized.begin(),resized.end(),std::complex<float>{});const unsigned limit=std::min(frame/2-1,cycleSize/2-1);const float scale=float(cycleSize)/frame;for(unsigned n=1;n<=limit;++n){resized[n]=original[n]*scale;resized[cycleSize-n]=original[frame-n]*scale;}cycleFFT.transform(resized,true);}for(unsigned n=0;n<cycleSize;++n){const double at=double(n)*frame/cycleSize;const unsigned a=unsigned(at),next=(a+1)%frame;const auto offset=std::size_t(f)*frame;const float x=resample?resized[n].real():float(source[offset+a]+(source[offset+next]-source[offset+a])*(at-a)-mean);result.cycles_[std::size_t(f)*cycleSize+n]=x;peak=std::max(peak,std::abs(x));}}
 if(peak<std::max(1.e-9f,resample?sourcePeak*1.e-5f:0.f))throw std::invalid_argument("Wavetable contains no alternating signal");for(auto&x:result.cycles_)x/=peak;
 FFT fft(cycleSize);std::vector<std::complex<float>> spectrum(cycleSize),work(cycleSize);result.data_.resize(std::size_t(result.frames_)*mipCount*(cycleSize+1));
 for(unsigned f=0;f<result.frames_;++f){for(unsigned n=0;n<cycleSize;++n)spectrum[n]={result.cycles_[std::size_t(f)*cycleSize+n],0};fft.transform(spectrum);for(unsigned m=0;m<mipCount;++m){work=spectrum;const unsigned limit=std::max(1u,(cycleSize/2-1)>>m);work[0]={};for(unsigned n=limit+1;n<cycleSize-limit;++n)work[n]={};fft.transform(work,true);auto* dest=result.data_.data()+(std::size_t(f)*mipCount+m)*(cycleSize+1);for(unsigned n=0;n<cycleSize;++n)dest[n]=work[n].real();dest[cycleSize]=dest[0];}}
 return result;
}
std::vector<std::byte> Wavetable::waveFile()const{
 if(!frames_)throw std::invalid_argument("No wavetable to export");
 const std::string metadata="<!>2048 00000000";ByteWriter writer;writer.u32(0x46464952);writer.u32(36+8+unsigned(metadata.size())+unsigned(cycles_.size()*4));writer.u32(0x45564157);writer.u32(0x20746d66);writer.u32(16);const auto shortWord=[&](unsigned n){writer.bytes.push_back(std::byte(n&255));writer.bytes.push_back(std::byte((n>>8)&255));};shortWord(3);shortWord(1);writer.u32(48000);writer.u32(48000*4);shortWord(4);shortWord(32);writer.u32(0x206d6c63);writer.u32(unsigned(metadata.size()));for(unsigned char c:metadata)writer.bytes.push_back(std::byte(c));writer.u32(0x61746164);writer.u32(unsigned(cycles_.size()*4));for(float value:cycles_)writer.u32(std::bit_cast<std::uint32_t>(value));return std::move(writer.bytes);
}
float Wavetable::read(float position,double phase,double hz,double rate)const noexcept{
 if(!frames_||!std::isfinite(position)||!std::isfinite(phase)||!std::isfinite(hz)||!std::isfinite(rate)||rate<=0)return 0;
 const float at=std::clamp(position,0.f,1.f)*(frames_-1);const unsigned a=unsigned(at),b=std::min(a+1,frames_-1);phase-=std::floor(phase);const double sample=phase*cycleSize;const auto i=unsigned(sample);
 const double level=std::clamp(std::log2(std::max(1.,std::abs(hz)*(cycleSize/2-1)/(rate*.43)))+1,1.,double(mipCount-1));const auto lo=unsigned(level),hi=std::min(lo+1,mipCount-1);
 const auto one=[&](unsigned f,unsigned m){const auto* p=data_.data()+(std::size_t(f)*mipCount+m)*(cycleSize+1);return p[i]+float(sample-i)*(p[i+1]-p[i]);};const auto cycle=[&](unsigned f){const auto x=one(f,lo);return x+float(level-lo)*(one(f,hi)-x);};const auto x=cycle(a);return x+(at-a)*(cycle(b)-x);
}
}
