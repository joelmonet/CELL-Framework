#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <WebView2.h>
#include "internal.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <stdexcept>
namespace cell::web::detail {
namespace {
std::atomic<unsigned> callbackObjects{},viewObjects{};
constexpr char editorUri[] = "https://cell-editor.invalid/editor.html";
constexpr wchar_t editorUriWide[] = L"https://cell-editor.invalid/editor.html";

bool isEditorUri(std::string_view uri) {
    constexpr std::string_view page = editorUri;
    return uri.starts_with(page) && (uri.size() == page.size() || uri[page.size()] == '#');
}
std::wstring wide(std::string_view value){
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    if(n<1&&!value.empty())throw std::invalid_argument("WebView UTF-8");std::wstring result(std::size_t(n),L'\0');
    if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),result.data(),n);return result;
}
std::string utf8(const wchar_t* value){
    if(!value)return {};std::size_t length=0;while(length<=maxMessageBytes&&value[length])++length;
    if(length>maxMessageBytes)throw std::length_error("WebView message bound");
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,int(length),nullptr,0,nullptr,nullptr);if(n<1&&length)throw std::invalid_argument("WebView UTF-16");
    std::string text(std::size_t(n),'\0');if(n)WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,int(length),text.data(),n,nullptr,nullptr);return text;
}
struct Release {template<class T>void operator()(T* p)const noexcept{if(p)p->Release();}};
template<class T>using Com=std::unique_ptr<T,Release>;
template<class T>Com<T> retain(T* p){if(p)p->AddRef();return Com<T>(p);}
// Public COM interface identities from the official WebView2 ABI. Explicit
// traits also work on MinGW, where the SDK does not specialize __mingw_uuidof.
template<class T>const GUID& callbackId();
template<>const GUID& callbackId<ICoreWebView2WebMessageReceivedEventHandler>(){static constexpr GUID id{0x57213f19,0x00e6,0x49fa,{0x8e,0x07,0x89,0x8e,0xa0,0x1e,0xcb,0xd2}};return id;}
template<>const GUID& callbackId<ICoreWebView2NavigationStartingEventHandler>(){static constexpr GUID id{0x9adbe429,0xf36d,0x432b,{0x9d,0xdc,0xf8,0x88,0x1f,0xbd,0x76,0xe3}};return id;}
template<>const GUID& callbackId<ICoreWebView2NavigationCompletedEventHandler>(){static constexpr GUID id{0xd33a35bf,0x1c49,0x4f98,{0x93,0xab,0x00,0x6e,0x05,0x33,0xfe,0x1c}};return id;}
template<>const GUID& callbackId<ICoreWebView2NewWindowRequestedEventHandler>(){static constexpr GUID id{0xd4c185fe,0xc81c,0x4989,{0x97,0xaf,0x2d,0x3f,0xa7,0xab,0x56,0x51}};return id;}
template<>const GUID& callbackId<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(){static constexpr GUID id{0x6c4819f3,0xc9b7,0x4260,{0x81,0x27,0xc9,0xf5,0xbd,0xe7,0xf6,0x8c}};return id;}
template<>const GUID& callbackId<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(){static constexpr GUID id{0x4e8a3389,0xc9d8,0x4bd2,{0xb6,0xb5,0x12,0x4f,0xee,0x6c,0xc1,0x4d}};return id;}
template<>const GUID& callbackId<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(){static constexpr GUID id{0xb99369f3,0x9b11,0x47b5,{0xbc,0x6f,0x8e,0x78,0x95,0xfc,0xea,0x17}};return id;}
template<>const GUID& callbackId<ICoreWebView2WebResourceRequestedEventHandler>(){static constexpr GUID id{0xab00b74c,0x15f1,0x4646,{0x80,0xe8,0xe7,0x63,0x41,0xd2,0x5d,0x71}};return id;}
struct TaskFree {void operator()(wchar_t* p)const noexcept{CoTaskMemFree(p);}};
using TaskString=std::unique_ptr<wchar_t,TaskFree>;
template<class Interface,class... Args>class Callback final:public Interface {
    std::atomic<ULONG> references_{1};std::function<HRESULT(Args...)> function_;
public:
    explicit Callback(std::function<HRESULT(Args...)> f):function_(std::move(f)){++callbackObjects;}
    ~Callback(){--callbackObjects;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output)override{
        if(!output)return E_POINTER;*output=nullptr;
        if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,callbackId<Interface>())){*output=static_cast<Interface*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto n=--references_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Invoke(Args... args)override{try{return function_(args...);}catch(...){return E_FAIL;}}
};
struct Loader {
    HMODULE module{};
    decltype(&CreateCoreWebView2EnvironmentWithOptions) create{};
    decltype(&GetAvailableCoreWebView2BrowserVersionString) version{};
    explicit Loader(const Options& options){
        auto path=options.loaderPath;
        if(path.empty()){
            HMODULE own{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&platformAvailability),&own);
            std::array<wchar_t,32768> name{};const auto n=GetModuleFileNameW(own,name.data(),DWORD(name.size()));if(!n||n>=name.size())throw std::runtime_error("Locate WebView2Loader.dll beside the application/plugin or supply loaderPath");path=std::filesystem::path(std::wstring(name.data(),n)).parent_path()/L"WebView2Loader.dll";
        }
        if(!path.is_absolute())throw std::invalid_argument("WebView2 loaderPath must be absolute");
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!module)throw std::runtime_error("Supply Microsoft WebView2Loader.dll and the Evergreen WebView2 Runtime");
        create=reinterpret_cast<decltype(create)>(GetProcAddress(module,"CreateCoreWebView2EnvironmentWithOptions"));
        version=reinterpret_cast<decltype(version)>(GetProcAddress(module,"GetAvailableCoreWebView2BrowserVersionString"));
        if(!create||!version){FreeLibrary(module);module=nullptr;throw std::runtime_error("WebView2 loader exports are missing");}
    }
    ~Loader(){if(module)FreeLibrary(module);}
};
struct Apartment {
    bool initialized{};
    Apartment(){const auto hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(hr))throw std::runtime_error("WebView2 requires an STA GUI thread");initialized=true;}
    ~Apartment(){if(initialized)CoUninitialize();}
};
class WindowsState:public std::enable_shared_from_this<WindowsState> {
public:
    std::shared_ptr<State> state;std::shared_ptr<Loader> loader;std::shared_ptr<Apartment> apartment;
    Com<ICoreWebView2Environment> environment;
    Com<ICoreWebView2Controller> controller;Com<ICoreWebView2> web;
    EventRegistrationToken messageToken{},navigationToken{},loadedToken{},newWindowToken{},resourceToken{};
    bool messageRegistered{},navigationRegistered{},loadedRegistered{},newWindowRegistered{},closed{};
    bool resourceRegistered{},resourceFilter{},visible{true};
    std::string html;
    std::optional<UINT64> navigationId;
    bool navigationRequested{};
    explicit WindowsState(std::shared_ptr<State> s):state(std::move(s)){++viewObjects;}
    ~WindowsState(){close();--viewObjects;}
    void failure(std::string text,HRESULT code){state->ready=false;state->push(Event::Kind::error,std::move(text)+" (HRESULT "+std::to_string(std::uint32_t(code))+")");}
    void begin(){
        if(!IsWindow(reinterpret_cast<HWND>(state->options.parent)))throw std::invalid_argument("WebView2 parent HWND");
        if(state->options.userDataDirectory.empty()||!state->options.userDataDirectory.is_absolute())throw std::invalid_argument("Supply an absolute application-owned WebView2 userDataDirectory");
        std::filesystem::create_directories(state->options.userDataDirectory);
        loader=std::make_shared<Loader>(state->options);apartment=std::make_shared<Apartment>();
        auto weak=weak_from_this();auto life=apartment;auto keepLoader=loader;
        using Handler=Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,HRESULT,ICoreWebView2Environment*>;
        Com<Handler> completed(new Handler([weak,life,keepLoader](HRESULT result,ICoreWebView2Environment* environment){
            if(auto self=weak.lock();self&&!self->closed){if(FAILED(result)||!environment)self->failure("Cannot create WebView2 environment",result);else self->createController(environment);}return S_OK;
        }));
        const auto hr=loader->create(nullptr,state->options.userDataDirectory.c_str(),nullptr,completed.get());if(FAILED(hr))throw std::runtime_error("WebView2 environment request failed");
    }
    void createController(ICoreWebView2Environment* createdEnvironment){
        environment = retain(createdEnvironment);
        auto weak=weak_from_this();auto life=apartment;auto keepLoader=loader;
        using Handler=Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,HRESULT,ICoreWebView2Controller*>;
        Com<Handler> completed(new Handler([weak,life,keepLoader](HRESULT result,ICoreWebView2Controller* created){
            if(auto self=weak.lock();self&&!self->closed){
                if(FAILED(result)||!created)self->failure("Cannot create WebView2 controller",result);
                else if(!IsWindow(reinterpret_cast<HWND>(self->state->options.parent))){created->Close();self->failure("WebView2 parent window closed",E_ABORT);}
                else{self->controller=retain(created);self->attach();}
            }else if(created)created->Close();return S_OK;
        }));
        const auto hr=environment->CreateCoreWebView2Controller(reinterpret_cast<HWND>(state->options.parent),completed.get());if(FAILED(hr))failure("Cannot request WebView2 controller",hr);
    }
    void attach(){
        ICoreWebView2* raw{};const auto hr=controller->get_CoreWebView2(&raw);web.reset(raw);if(FAILED(hr)||!web){failure("Cannot obtain WebView2",hr);return;}
        ICoreWebView2Settings* rawSettings{};if(SUCCEEDED(web->get_Settings(&rawSettings))&&rawSettings){Com<ICoreWebView2Settings> settings(rawSettings);settings->put_IsWebMessageEnabled(TRUE);settings->put_AreDevToolsEnabled(state->options.developerTools);settings->put_AreDefaultContextMenusEnabled(state->options.developerTools);}
        auto weak=weak_from_this();auto life=apartment;auto keepLoader=loader;
        using Message=Callback<ICoreWebView2WebMessageReceivedEventHandler,ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs*>;
        Com<Message> message(new Message([weak,life,keepLoader](auto*,auto* args){if(auto self=weak.lock();self&&!self->closed){LPWSTR text{};if(SUCCEEDED(args->TryGetWebMessageAsString(&text))&&text){try{auto value=utf8(text);CoTaskMemFree(text);text=nullptr;self->state->push(Event::Kind::message,std::move(value));}catch(...){CoTaskMemFree(text);self->state->push(Event::Kind::error,"WebView2 bridge message bounds/encoding");}}}return S_OK;}));
        messageRegistered=SUCCEEDED(web->add_WebMessageReceived(message.get(),&messageToken));
        using Navigation=Callback<ICoreWebView2NavigationStartingEventHandler,ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*>;
        Com<Navigation> navigation(new Navigation([weak,life,keepLoader](auto*,auto* args) {
            if (auto self=weak.lock(); self && !self->closed) self->startNavigation(args);
            return S_OK;
        }));
        navigationRegistered=SUCCEEDED(web->add_NavigationStarting(navigation.get(),&navigationToken));
        using Loaded=Callback<ICoreWebView2NavigationCompletedEventHandler,ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*>;
        Com<Loaded> loaded(new Loaded([weak,life,keepLoader](auto*,auto* args) {
            if (auto self=weak.lock(); self && !self->closed) self->finishNavigation(args);
            return S_OK;
        }));
        loadedRegistered=SUCCEEDED(web->add_NavigationCompleted(loaded.get(),&loadedToken));
        using NewWindow=Callback<ICoreWebView2NewWindowRequestedEventHandler,ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs*>;
        Com<NewWindow> newWindow(new NewWindow([weak,life,keepLoader](auto*,auto* args){args->put_Handled(TRUE);return S_OK;}));
        newWindowRegistered=SUCCEEDED(web->add_NewWindowRequested(newWindow.get(),&newWindowToken));
        // Serve the document from memory. NavigateToString caps HTML at 2 MiB,
        // which is too small for designer exports with embedded artwork.
        resourceFilter = SUCCEEDED(web->AddWebResourceRequestedFilter(editorUriWide, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT));
        using Resource = Callback<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>;
        Com<Resource> resource(new Resource([weak,life,keepLoader](auto*,auto* args) {
            if (auto self=weak.lock(); self && !self->closed) self->serveEditor(args);
            return S_OK;
        }));
        resourceRegistered = resourceFilter && SUCCEEDED(web->add_WebResourceRequested(resource.get(), &resourceToken));
        if(!messageRegistered||!navigationRegistered||!loadedRegistered||!newWindowRegistered||!resourceRegistered){failure("WebView2 callback registration failed",E_FAIL);close();return;}

        setBounds(state->options.bounds);
        controller->put_IsVisible(visible);
        using Script = Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, HRESULT, LPCWSTR>;
        Com<Script> installed(new Script([weak,life,keepLoader](HRESULT result, LPCWSTR) {
            if (auto self=weak.lock(); self && !self->closed) {
                if (FAILED(result)) self->failure("Cannot install WebView2 bridge", result);
                else {
                    self->state->ready = true;
                    self->state->push(Event::Kind::ready, {});
                }
            }
            return S_OK;
        }));
        const auto script = wide(bridgeScript());
        const auto added = web->AddScriptToExecuteOnDocumentCreated(script.c_str(), installed.get());
        if (FAILED(added)) failure("Cannot request WebView2 bridge installation", added);
    }
    void startNavigation(ICoreWebView2NavigationStartingEventArgs* args) {
        LPWSTR rawUri{};
        const auto result = args->get_Uri(&rawUri);
        const TaskString uriMemory(rawUri);
        if (FAILED(result) || !rawUri) { args->put_Cancel(TRUE); return; }
        std::string uri;
        try { uri = utf8(rawUri); }
        catch (...) { args->put_Cancel(TRUE); throw; }
        if ((html.empty() || !isEditorUri(uri)) && !navigationAllowed(uri, state->options)) {
            args->put_Cancel(TRUE);
            state->push(Event::Kind::navigationBlocked, std::move(uri));
            return;
        }
        UINT64 id{};
        if (navigationRequested && SUCCEEDED(args->get_NavigationId(&id))) navigationId = id;
    }
    void finishNavigation(ICoreWebView2NavigationCompletedEventArgs* args) {
        UINT64 id{};
        if (!navigationId || FAILED(args->get_NavigationId(&id)) || id != *navigationId) return;
        navigationId.reset();
        BOOL success{};
        args->get_IsSuccess(&success);
        state->push(success ? Event::Kind::loaded : Event::Kind::error,
                    success ? std::string{} : "WebView2 navigation failed");
    }
    void serveEditor(ICoreWebView2WebResourceRequestedEventArgs* args) {
        IStream* rawStream{};
        auto result = html.empty() ? E_FAIL : CreateStreamOnHGlobal(nullptr, TRUE, &rawStream);
        const Com<IStream> stream(rawStream);
        ULONG written{};
        if (SUCCEEDED(result)) result = stream->Write(html.data(), static_cast<ULONG>(html.size()), &written);
        if (SUCCEEDED(result) && written != html.size()) result = STG_E_WRITEFAULT;
        if (SUCCEEDED(result)) result = stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr);
        ICoreWebView2WebResourceResponse* rawResponse{};
        constexpr auto headers = L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store";
        if (SUCCEEDED(result)) result = environment->CreateWebResourceResponse(stream.get(), 200, L"OK", headers, &rawResponse);
        const Com<ICoreWebView2WebResourceResponse> response(rawResponse);
        if (SUCCEEDED(result)) result = args->put_Response(response.get());
        if (FAILED(result)) {
            web->Stop();
            failure("Cannot serve the embedded WebView2 editor", result);
        }
    }
    bool navigate(const wchar_t* uri) {
        if (!web) return false;
        navigationRequested = true;
        const auto result = web->Navigate(uri);
        if (FAILED(result)) failure("Cannot navigate WebView2", result);
        return SUCCEEDED(result);
    }
    bool loadHtml(std::string_view page) {
        html.assign(page);
        return navigate(editorUriWide);
    }
    void setVisible(bool show) {
        visible = show;
        if (controller) controller->put_IsVisible(show);
    }
    void setBounds(Bounds b){if(controller){const RECT rect{b.x,b.y,b.x+b.width,b.y+b.height};controller->put_Bounds(rect);}}
    void close(){if(closed)return;closed=true;state->ready=false;
        if(web){if(messageRegistered)web->remove_WebMessageReceived(messageToken);if(navigationRegistered)web->remove_NavigationStarting(navigationToken);if(loadedRegistered)web->remove_NavigationCompleted(loadedToken);if(newWindowRegistered)web->remove_NewWindowRequested(newWindowToken);}
        if (resourceRegistered) web->remove_WebResourceRequested(resourceToken);
        if (resourceFilter) web->RemoveWebResourceRequestedFilter(editorUriWide, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
        if(controller)controller->Close();web.reset();controller.reset();environment.reset();
    }
};
class WindowsView final:public NativeView {
    std::shared_ptr<WindowsState> native_;
public:
    explicit WindowsView(const std::shared_ptr<State>& state):native_(std::make_shared<WindowsState>(state)){native_->begin();}
    ~WindowsView()override{close();}
    bool loadHtml(std::string_view html)override{return native_->loadHtml(html);}
    bool navigate(std::string_view uri)override{return native_->navigate(wide(uri).c_str());}
    bool evaluate(std::string_view script)override{return native_->web&&SUCCEEDED(native_->web->ExecuteScript(wide(script).c_str(),nullptr));}
    void bounds(Bounds b)override{native_->setBounds(b);}
    void visible(bool visible)override{native_->setVisible(visible);}
    void focus()override{if(native_->controller)native_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);}
    void poll()override{} // The embedding window's GUI loop dispatches COM messages.
    void close()override{if(native_){native_->close();native_.reset();}}
};
}
Availability platformAvailability(const Options& options){try{Loader loader(options);LPWSTR version{};const auto hr=loader.version(nullptr,&version);TaskString memory(version);const bool available=SUCCEEDED(hr)&&memory;std::string detail=available?"Microsoft WebView2 Runtime "+utf8(memory.get()):"Install the Microsoft Evergreen WebView2 Runtime";return {Backend::webView2,available,std::move(detail)};}catch(const std::exception& e){return {Backend::webView2,false,e.what()};}}
std::unique_ptr<NativeView> createNative(const std::shared_ptr<State>& state){return std::make_unique<WindowsView>(state);}
bool platformReadyToUnload()noexcept{return callbackObjects.load()==0&&viewObjects.load()==0;}
}
