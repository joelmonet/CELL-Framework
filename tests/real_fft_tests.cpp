// Original Cell Framework implementation; see LICENSE.
#include <cell/dsp.hpp>
#include <iostream>
namespace {void need(bool v,const char*m){if(!v)throw std::runtime_error(m);}}
int main(){try{
 for(std::size_t size:{std::size_t(2),std::size_t(128),std::size_t(8192),std::size_t(16384)}){cell::dsp::RealFFT fft(size);std::vector<float>data(size);data[0]=1;fft.magnitude(data);for(std::size_t k=0;k<=size/2;++k)need(std::abs(data[k]-1)<2.e-5f,"Impulse spectrum convention changed");std::fill(data.begin(),data.end(),.25f);fft.magnitude(data);need(std::abs(data[0]-.25f*size)<.0002f,"DC scaling changed");for(std::size_t k=1;k<=size/2;++k)need(data[k]<.0002f,"Constant signal leaked into non-DC bins");if(size>2){for(std::size_t i=0;i<size;++i)data[i]=float(.3*std::cos(2*std::numbers::pi*7*i/size));fft.magnitude(data);need(std::abs(data[7]-.15f*size)<.001f*size,"Sinusoid magnitude or frequency changed");}
  std::vector<float>real(size*2);for(std::size_t i=0;i<size;++i)real[i]=float(.3*std::sin(.17*i)+.2*std::cos(.31*i));const auto original=real;fft.forward(real);need(std::abs(real[1])<1.e-6f&&std::abs(real[size+1])<1.e-5f,"Real DC/Nyquist acquired an imaginary component");fft.inverse(real);for(std::size_t i=0;i<size;++i)need(std::abs(real[i]-original[i])<2.e-5f,"Real spectrum round trip changed sample origin/phase");
  cell::dsp::FFT complex(size);std::vector<std::complex<float>>samples(size);for(std::size_t i=0;i<size;++i)samples[i]={float(std::sin(.31*i)),float(std::cos(.17*i))};auto before=samples;complex.transform(samples);complex.transform(samples,true);for(std::size_t i=0;i<size;++i)need(std::abs(samples[i]-before[i])<3.e-5f,"FFT round trip lost precision/phase");
 }
 bool rejected=false;try{cell::dsp::FFT fft(127);}catch(const std::invalid_argument&){rejected=true;}need(rejected,"Invalid FFT shape accepted");
 std::cout<<"PASS native real FFT: impulse/DC/tone oracles, unscaled forward, normalized inverse, phase/precision round trips and invalid-shape rejection\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
