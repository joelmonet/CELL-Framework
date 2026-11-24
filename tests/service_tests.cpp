#include <cell/assets.hpp>
#include <cell/history.hpp>
#include <cell/operation_ack.hpp>
#include <cell/state_transfer.hpp>
#include <cell/thumbnails.hpp>
#include <cell/parameters.hpp>
#include "allocation_watch.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

using namespace cell;
namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class Predicate> void until(Predicate predicate){
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
 while(!predicate()){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("asynchronous operation timed out");std::this_thread::yield();}
}
struct Gate {
 std::mutex mutex;std::condition_variable wake;bool open{};
 void release(){std::lock_guard lock(mutex);open=true;wake.notify_all();}
 void wait(){std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::seconds(5),[&]{return open;});}
 ~Gate(){release();}
};
void jobsTest(){
 jobs::Pool pool(1,2);jobs::Owner owner;auto gate=std::make_shared<Gate>();std::promise<void> started;std::atomic<unsigned> invoked{};
 auto first=pool.submit(owner.token(),[&,gate](jobs::Context& context){started.set_value();gate->wait();context.progress(.7);context.progress(.2);check(context.cancelled(),"old generation not cancelled");return jobs::Result::success(42);});
 started.get_future().wait();auto second=pool.submit(owner.token(),[&](jobs::Context&){++invoked;return jobs::Result::success(7);});
 check(bool(first)&&bool(second),"job admission rejected available capacity");check(!pool.submit(owner.token(),[](jobs::Context&){return jobs::Result::success(1);}),"job capacity overrun");
 owner.cancel();gate->release();first.wait();second.wait();
 check(first.poll().status==jobs::Status::cancelled&&!first.poll().payload&&!invoked,"stale job published data or ran queued work");
 auto failed=pool.submit(owner.token(),[](jobs::Context&)->jobs::Result{throw std::runtime_error("fixture failure");});failed.wait();check(failed.poll().status==jobs::Status::failed&&failed.poll().message=="fixture failure","worker exception escaped or vanished");
 auto progress=pool.submit(owner.token(),[](jobs::Context& context){context.progress(.7);context.progress(.1);context.progress(NAN);return jobs::Result::failure("done");});progress.wait();check(progress.poll().progress==.7,"progress regressed or accepted NaN");
 auto transient=std::make_unique<jobs::Owner>();auto complete=pool.submit(transient->token(),[](jobs::Context&){return jobs::Result::success(std::string("owned"));});complete.wait();transient.reset();check(complete.poll().status==jobs::Status::cancelled&&!complete.poll().payload,"destroyed owner retained a deliverable payload");
 // Releasing the final service owner from its own job must not self-join or
 // free the queue while a worker still uses it.
 auto nested=std::make_shared<jobs::Pool>(1,4);auto hold=nested;std::weak_ptr<jobs::Pool> weak=nested;
 auto nestedJob=nested->submit(owner.token(),[hold=std::move(hold)](jobs::Context&)mutable{hold.reset();return jobs::Result::success(1);});nested.reset();nestedJob.wait();until([&]{return weak.expired();});
}
void archiveTest(){
 history::Store store;std::vector<std::byte> asset(2u*1024*1024,std::byte{17});const auto shared=store.capture(asset);
 history::Timeline history({40,4u*1024*1024});
 for(unsigned i=0;i<40;++i){ByteWriter header;header.u32(i);const std::array<history::Archive,2> pieces{store.capture(header.bytes),shared};check(history.record(store.join(pieces),"Edit"),"shared asset history exceeded budget");}
 check(history.count()==40&&history.residentBytes()<3u*1024*1024,"history duplicated the embedded asset per edit");
 const auto before=history.current()->state;
 cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;auto copy=before;cell_test::audio=false;
 check(!cell_test::allocations&&!cell_test::deletions&&copy==before,"archive copy allocated a page table");
 const auto full=before.bytes();const auto split=std::size_t(70003);const std::array<history::Archive,2> parts{before.slice(0,split),before.slice(split,before.size()-split)};
 check(store.join(parts)==before&&store.join(parts).bytes()==full,"archive equality depended on page boundaries");
 std::array<std::byte,19> tail{};check(before.read(before.size()-tail.size(),tail)&&std::equal(tail.begin(),tail.end(),full.end()-tail.size()),"cross-page read changed bytes");
 check(!before.read(before.size()-1,tail),"out-of-range archive read accepted");
 check(!history.undo([](const auto&){return false;})&&history.current()->state==before&&history.canUndo(),"failed restore moved undo cursor");
 check(history.undo([](const auto&){return true;})&&history.canRedo(),"successful undo lost redo");
 const auto ticket=history.prepare(true);check(ticket&&history.pending(ticket),"async undo ticket missing");
 check(history.complete(ticket,false)==false&&history.canRedo(),"rejected asynchronous restore changed history");
 check(history.setComparison(before),"shared comparison admission");
 const auto branch=store.capture(std::array<std::byte,1>{std::byte{99}});check(history.record(branch,"Branch")&&!history.canRedo(),"new branch retained stale redo");
 const auto count=history.count();std::vector<std::byte> distinctPages(5u*1024*1024);for(std::size_t n=0;n<distinctPages.size();++n)distinctPages[n]=std::byte(n/history::Store::pageBytes);auto tooLarge=store.capture(distinctPages);check(!history.record(tooLarge,"Too large")&&history.count()==count&&history.current()->state==branch,"rejected budget admission discarded the current branch");
}
void transferTest(){
 auto pool=std::make_shared<jobs::Pool>(1,8);std::vector<std::byte> source(500003);for(std::size_t i=0;i<source.size();++i)source[i]=std::byte(i*31);
 std::atomic<unsigned> captures{};state::SnapshotServer server([&]()->std::optional<std::vector<std::byte>>{++captures;return source;},pool);
 state::SnapshotClient client;std::size_t largest{};
 auto query=[&](std::span<const std::byte> request,bool){auto reply=server.query(request);largest=std::max(largest,reply.size());return reply;};
 client.start();until([&]{return client.poll(query)!=state::SnapshotClient::Status::waiting;});
 check(client.status()==state::SnapshotClient::Status::ready&&client.take()==source&&captures==1&&largest<=maxProductReplyBytes,"snapshot transfer repeated capture, truncated data or exceeded message bounds");
 // Corrupt an actual chunk. No partially downloaded state is delivered.
 client.start();auto corrupt=[&](std::span<const std::byte> request,bool fresh){auto reply=query(request,fresh);ByteReader reader(request);unsigned magic{},version{},op{};reader.u32(magic);reader.u32(version);reader.u32(op);if(op==2&&!reply.empty())reply.back()^=std::byte{1};return reply;};
 until([&]{return client.poll(corrupt)!=state::SnapshotClient::Status::waiting;});check(client.status()==state::SnapshotClient::Status::failed&&client.take().empty(),"corrupt snapshot reached the caller");
 client.cancel(query);client.start();until([&]{return client.poll(query)!=state::SnapshotClient::Status::waiting;});check(client.status()==state::SnapshotClient::Status::ready,"failed transfer leaked its processor lease");
 client.take();client.start();check(client.poll([](auto,bool){return std::vector<std::byte>{std::byte{1}};})==state::SnapshotClient::Status::failed,"malformed reply accepted");
}
void acknowledgementsTest(){
 OperationAck acknowledgements;const std::array<std::byte,3> data{std::byte{1},std::byte{2},std::byte{3}};
 const auto request=acknowledgements.begin(data,4);check(request.token&&acknowledgements.status(request.token)==RestoreStatus::pending,"operation not pending after transport admission");
 check(!acknowledgements.begin(data,4).token&&!acknowledgements.acknowledge(request.token+1,true),"overlapping or wrong-owner receipt admitted");
 check(acknowledgements.acknowledge(request.token,false),"rejection receipt lost");const auto receipt=acknowledgements.consume(request.token);
 check(receipt&&!receipt->accepted&&*receipt->request.bytes==std::vector<std::byte>(data.begin(),data.end())&&!acknowledgements.consume(request.token),"receipt consumed twice or changed payload");
 check(acknowledgements.finish(request.token,false)&&acknowledgements.status(request.token)==RestoreStatus::failed,"failed processor acknowledgement reported success");
 const auto newer=acknowledgements.begin(data,4);check(newer.token!=request.token&&!acknowledgements.acknowledge(request.token,true),"late receipt applied to a new operation");
}
struct Temporary {
 std::filesystem::path path=std::filesystem::temp_directory_path()/("cell-service-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 Temporary(){std::filesystem::create_directories(path);}
 ~Temporary(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void browserTest(){
 Temporary temporary;std::filesystem::create_directory(temporary.path/"Folder");std::ofstream(temporary.path/"z.WAV").put('z');std::ofstream(temporary.path/"A.wav").put('a');std::ofstream(temporary.path/"ignore.txt").put('x');
 auto pool=std::make_shared<jobs::Pool>(1,8);assets::Browser browser(pool);assets::Filter filter;filter.extensions={"wav"};
 check(browser.scan(temporary.path,filter),"folder scan admission");until([&]{return browser.poll();});const auto listing=browser.listing();
 check(listing.entries.size()==3&&listing.entries[0].directory&&listing.entries[1].name=="A.wav"&&listing.entries[2].name=="z.WAV","folder metadata, filtering or deterministic ordering failed");
 assets::Annotations annotations;const auto& entry=listing.entries[1];check(annotations.set(entry.identity,{true,{"Vocal","Bright"}}),"asset annotation rejected");
 check(annotations.matches(entry,"bright vocal",true)&&!annotations.matches(entry,"missing",true),"search ignored tags or favorites");const auto saved=annotations.save();
 assets::Annotations restored;check(restored.load(saved)&&restored.matches(entry,"VOCAL",true),"annotation roundtrip failed");
 for(std::size_t n=0;n<saved.size();++n){check(!restored.load(std::span(saved).first(n))&&restored.save()==saved,"truncated metadata changed the old library");}
 filter.maximumEntries=1;check(browser.scan(temporary.path,filter),"bounded scan admission");until([&]{return browser.poll();});check(browser.listing().truncated&&browser.listing().entries.size()==1,"folder bound silently exceeded");
 check(browser.scan(temporary.path/"missing",filter),"missing folder request rejected before reporting");until([&]{return browser.poll();});check(!browser.listing().error.empty(),"missing folder reported as an empty successful library");
}
void waveFile(const std::filesystem::path& path){
 ByteWriter writer;writer.u32(0x46464952);writer.u32(36+717*4);writer.u32(0x45564157);writer.u32(0x20746d66);writer.u32(16);writer.u32(1u|(2u<<16));writer.u32(48000);writer.u32(48000*4);writer.u32(4u|(16u<<16));writer.u32(0x61746164);writer.u32(717*4);
 for(unsigned i=0;i<717;++i){const std::uint16_t left=16384,right=std::bit_cast<std::uint16_t>(std::int16_t(-8192));writer.u32(left|(std::uint32_t(right)<<16));}
 std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(writer.bytes.data()),std::streamsize(writer.bytes.size()));
}
void thumbnailTest(){
 Temporary temporary;const auto path=temporary.path/"signal.wav";waveFile(path);
 jobs::Owner owner;auto pool=std::make_shared<jobs::Pool>(1,8);waveform::Cache cache({128u*1024,65536,2},waveform::openWave,pool);
 auto job=cache.request(path,owner.token());job.wait();auto result=job.poll();const auto* pointer=result.get<std::shared_ptr<const waveform::Thumbnail>>();
 check(pointer&&*pointer,"valid WAV preview rejected");const auto thumbnail=*pointer;check(thumbnail->audio.channels==2&&thumbnail->audio.frames==717&&thumbnail->levels.size()>1,"preview changed channel/frame dimensions");
 const auto& peak=thumbnail->levels.back().peaks;check(peak.size()==2&&peak[0].minimum==.5f&&peak[0].maximum==.5f&&peak[1].maximum==-.25f&&peak[0].samples==717&&peak[1].samples==717,"peak pyramid padded partial bins or invented zero crossings");
 check(peak[0].rms()==.5&&peak[1].rms()==.25&&cache.residentBytes()<=128u*1024,"preview RMS or memory accounting incorrect");
 auto cached=cache.request(path,owner.token());cached.wait();const auto cachedResult=cached.poll();const auto* again=cachedResult.get<std::shared_ptr<const waveform::Thumbnail>>();check(again&&*again==thumbnail,"unchanged audio decoded into a new thumbnail");
 const auto bad=temporary.path/"corrupt.wav";std::ofstream(bad,std::ios::binary).write("RIFF",4);auto corrupt=cache.request(bad,owner.token());corrupt.wait();check(corrupt.poll().status==jobs::Status::failed,"truncated audio produced a plausible waveform");
 owner.cancel();check(!job.poll().payload,"stale preview retained a deliverable result");
}
void parameterSnapshotTest(){
 const std::array<Parameter,2> fields{{{1,"a","A","",-1,1,0,0,1},{2,"b","B","",0,10,1,0,1}}};ParameterBank bank(fields);bank.set(1,.25);bank.restorePlain(2,7.125);
 std::array<double,2> values{};cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;const bool copied=bank.copyValues(values,true);cell_test::audio=false;
 check(copied&&values[0]==.25&&values[1]==7.125&&!cell_test::allocations&&!cell_test::deletions,"descriptor snapshot allocated, reordered or snapped plain values");
 const auto reader=std::as_const(bank).bind(2);const auto writer=bank.bind(2);check(reader&&writer&&reader.descriptor()->id==2,"binding did not resolve its descriptor");
 cell_test::allocations=cell_test::deletions=0;cell_test::audio=true;const bool restored=writer.restorePlain(1.234567);const auto read=reader.acquire();const bool invalidBinding=bank.bind(999).set(1);cell_test::audio=false;
 check(restored&&read==1.234567&&!invalidBinding&&!cell_test::allocations&&!cell_test::deletions,"cached binding changed plain values, admitted an unknown ID or allocated");
 check(!writer.restorePlain(11)&&!writer.restorePlain(NAN)&&reader.load()==1.234567,"invalid cached write changed the parameter");
 std::array<double,1> invalid{123};check(!bank.copyValues(invalid)&&invalid[0]==123,"invalid destination changed before rejection");
}
}
int main(){try{jobsTest();archiveTest();transferTest();acknowledgementsTest();browserTest();thumbnailTest();parameterSnapshotTest();std::cout<<"PASS: Cell background jobs, full-state archives, transfer integrity, acknowledgements, asset browser and waveform contracts\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
