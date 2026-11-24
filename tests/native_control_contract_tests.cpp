// Original Cell Framework implementation; see LICENSE.
#include <cell/dispatch.hpp>
#include <cell/process_buffer.hpp>
#include <cell/colour.hpp>
#include <climits>
#include <limits>
#include <cell/hash.hpp>
#include <cell/widgets/services.hpp>
#include <atomic>
#include <iostream>
#include <thread>
#include <stdexcept>
namespace {
void need(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
void digest(const std::string&input,const char*expected){cell::Sha256 hash;for(std::size_t at=0;at<input.size();){const auto n=std::min<std::size_t>(71,input.size()-at);hash.update({reinterpret_cast<const std::byte*>(input.data()+at),n});at+=n;}need(hash.hex()==expected,"SHA-256 known vector mismatch");}
}
int main(){try{
 cell::MainThreadDispatcher::bindCurrentThread();
 std::atomic<bool>guiRan=false,modelRan=false,rejected=false;std::thread::id modelThread;
 cell::MainThreadDispatcher::post([&]{need(cell::MainThreadDispatcher::isOwnerThread(),"GUI task ran away from its owner");guiRan=true;});
 cell::MainThreadDispatcher::postControl([&]{modelThread=std::this_thread::get_id();modelRan=true;});
 std::thread host([&]{cell::MainThreadDispatcher::dispatchControl();try{cell::MainThreadDispatcher::dispatch();}catch(const std::logic_error&){rejected=true;}});const auto expected=host.get_id();host.join();
 need(modelRan&&modelThread==expected&&!guiRan&&rejected,"Headless model service consumed GUI work or missed data work");
 cell::MainThreadDispatcher::dispatch();need(guiRan,"GUI queue stopped after a rejected foreign dispatch");
 {
  cell::ControlScope transaction;std::atomic<bool>admitted=true;
  std::thread paint([&]{cell::ControlScope frame(std::try_to_lock);admitted=frame.ownsLock();});paint.join();need(!admitted,"A view entered a model transaction owned by another control thread");
 }
 std::atomic<bool>admitted=false;std::thread paint([&]{cell::ControlScope frame(std::try_to_lock);admitted=frame.ownsLock();});paint.join();need(admitted,"Control transaction did not release its gate");
 struct Listener:cell::ui::widgets::ChangeListener{int changes{};void changeListenerCallback(cell::ui::widgets::ChangeBroadcaster*)override{++changes;}}listener;
 {
  auto source=std::make_unique<cell::ui::widgets::ChangeBroadcaster>();source->addChangeListener(&listener);for(int i=0;i<100000;++i)source->sendChangeMessage();need(cell::MainThreadDispatcher::pending()==1,"Change notifications did not coalesce");source.reset();cell::MainThreadDispatcher::dispatch();need(listener.changes==0,"A destroyed notification owner was called");
 }
 {
  cell::ui::widgets::ChangeBroadcaster source;source.addChangeListener(&listener);source.sendChangeMessage();source.sendChangeMessage();cell::MainThreadDispatcher::dispatch();need(listener.changes==1,"A live notification was lost or duplicated");
 }
 int following=0;cell::MainThreadDispatcher::postControl([]{throw std::runtime_error("expected callback error");});cell::MainThreadDispatcher::postControl([&]{++following;});bool caught=false;try{cell::MainThreadDispatcher::dispatchControl();}catch(const std::runtime_error&){caught=true;}need(caught&&following==1,"An exception poisoned subsequent model completions");
 std::array<float,128>a{},b{};std::array<float*,2>p{a.data(),b.data()};cell::ProcessBuffer<float>block(p.data(),2,a.size());a[5]=.7f;b[5]=-.7f;need(block.getMagnitude(1,0,128)==.7f,"Host buffer magnitude changed");block.applyGainRamp(0,128,1.f,.5f);need(a[5]==-b[5],"A planar host gain ramp changed stereo polarity/phase");block.clear();need(a[5]==0&&b[5]==0,"Native host clear left stale audio");
 {
  const auto rejects = [](auto&& operation) { bool rejected=false;try{operation();}catch(const std::invalid_argument&){rejected=true;}catch(const std::length_error&){rejected=true;}need(rejected,"Processing dimensions narrowed or admitted an oversized host view"); };
  const auto wide = std::size_t(INT_MAX)+1;
  rejects([&]{cell::ProcessBuffer<float> view(p.data(),1,wide);});
  rejects([&]{cell::ProcessBuffer<float> view(0,wide);});
  rejects([&]{block.setDataToReferTo(p.data(),1,wide);});
  rejects([&]{block.resize(0,wide);});
  rejects([&]{block.setSize(0,wide);});
  rejects([&]{block.setSize(-1,1);});
  cell::OwnedAudio<float> source(p.data(),1,wide);
  rejects([&]{block.makeCopyOf(source);});
  rejects([&]{block=source;});
  rejects([&]{block.swap(source);});
  need(block.getNumChannels()==2&&block.getNumSamples()==128,"Rejected wide operation changed the valid processing block");
  const auto nan=std::numeric_limits<float>::quiet_NaN();
  const cell::Colour colour{0x80112233u};
  need(colour.withAlpha(nan).getAlpha()==0&&colour.interpolatedWith(cell::Colour{0xffffffffu},nan)==colour&&
       cell::Colour::fromFloatRGBA(nan,1,0,nan).getARGB()==0x0000ff00u,
       "Nonfinite colour controls reached an invalid integer conversion");
 }
 digest("","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 digest("abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 digest(std::string(1000000,'a'),"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
 std::cout<<"PASS native control: headless/GUI separation, transaction exclusion, cancellation/coalescing, exception recovery, planar buffers, SHA-256 known vectors\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
