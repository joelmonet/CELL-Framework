#include <cell/webview.hpp>
#include <iostream>
#include <thread>
#include <stdexcept>
namespace {void check(bool value,const char* what){if(!value)throw std::runtime_error(what);}}
int main(){try{
    using namespace cell::web;
    View view;check(!view.ready(),"unopened browser ready");
    Options empty;check(!view.open(empty),"null native parent accepted");
    bool reported=false;for(const auto& event:view.poll())reported|=event.kind==Event::Kind::error;
    check(reported&&!view.error().empty(),"missing parent failed without an error");
    check(!view.send("{bad}")&&!view.send("{}"),"unopened browser accepted a message");
    check(!view.loadHtml("<p>unopened</p>")&&!view.evaluate("1"),"unopened browser accepted content");
    check(!view.navigate("https://example.com"),"unopened browser accepted navigation");
    for(Bounds bounds: {Bounds{0,0,0,200},Bounds{0,0,8200,200},Bounds{2000000,0,400,200}}){
        bool rejected=false;try{view.setBounds(bounds);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"invalid browser geometry accepted");
    }
    bool rejectedThread=false;std::thread worker([&]{try{view.ready();}catch(const std::logic_error&){rejectedThread=true;}});worker.join();check(rejectedThread,"cross-thread browser access accepted");
    view.close();view.close();check(!view.ready()&&View::readyToUnload(),"unopened/closed view retained callback owners");
#if defined(_WIN32)
    Options missing;missing.loaderPath=L"Z:\\does-not-exist-cell-webview\\WebView2Loader.dll";
    const auto availability=View::availability(missing);check(!availability.available&&!availability.detail.empty(),"missing loader did not report unavailable");
#endif
    std::cout<<"PASS: WebView link, closed states, bounds, GUI ownership and unload contract\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
