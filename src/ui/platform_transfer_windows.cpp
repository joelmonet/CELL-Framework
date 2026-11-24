// Original Cell Framework implementation; see LICENSE.
#include <cell/widgets/services.hpp>
#include "platform_transfer.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <climits>
#include <cstring>
namespace cell::ui::native {
namespace {
std::wstring wide(std::string_view text){if(text.size()>INT_MAX)throw std::length_error("Windows text addressing");const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);if(!n&&!text.empty())throw std::invalid_argument("Invalid UTF-8 text");std::wstring out(std::size_t(n),L'\0');if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),n);return out;}
std::string utf8(std::wstring_view text){if(text.size()>INT_MAX)throw std::length_error("Windows text addressing");const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);if(!n&&!text.empty())throw std::invalid_argument("Invalid UTF-16 text");std::string out(std::size_t(n),'\0');if(n)WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),n,nullptr,nullptr);return out;}
struct Apartment {
 HRESULT status=E_FAIL;bool initialized{};
 bool open(){if(!initialized){status=OleInitialize(nullptr);initialized=true;}return SUCCEEDED(status);}
 void close()noexcept{const bool release=initialized&&SUCCEEDED(status);initialized=false;status=E_FAIL;if(release)OleUninitialize();}
 ~Apartment(){close();}
};
struct ClipboardOwner {
 HWND window{};
 HWND open(){if(!window)window=CreateWindowExW(0,L"STATIC",L"Cell clipboard",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);return window;}
 void close()noexcept{const auto handle=window;window=nullptr;if(handle)DestroyWindow(handle);}
 ~ClipboardOwner(){close();}
};
thread_local Apartment nativeApartment;
thread_local ClipboardOwner nativeClipboard;
bool apartment(){return nativeApartment.open();}
struct ClipboardLock{bool open{};ClipboardLock(){const auto owner=nativeClipboard.open();for(int i=0;i<5&&!open;++i){open=owner&&OpenClipboard(owner)!=FALSE;if(!open)Sleep(1);}}~ClipboardLock(){if(open)CloseClipboard();}};
class Formats final:public IEnumFORMATETC {
 std::atomic<ULONG>refs_{1};bool used_{};
 public:HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_IEnumFORMATETC){*out=static_cast<IEnumFORMATETC*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Next(ULONG count,FORMATETC*out,ULONG*read)override{if(!out||(count!=1&&!read))return E_POINTER;if(read)*read=0;if(!count)return S_OK;if(used_)return S_FALSE;*out={CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};used_=true;if(read)*read=1;return count==1?S_OK:S_FALSE;}
 HRESULT STDMETHODCALLTYPE Skip(ULONG count)override{if(!count)return S_OK;if(used_)return S_FALSE;used_=true;return count==1?S_OK:S_FALSE;}
 HRESULT STDMETHODCALLTYPE Reset()override{used_=false;return S_OK;}
 HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC**out)override{if(!out)return E_POINTER;auto*copy=new Formats;copy->used_=used_;*out=copy;return S_OK;}
};
class Files final:public IDataObject {
 std::atomic<ULONG>refs_{1};std::vector<std::byte>bytes_;
 public:explicit Files(std::span<const std::string>files){std::vector<std::wstring>names;std::size_t chars=1;for(const auto&name:files){names.push_back(wide(name));if(names.back().size()>SIZE_MAX-chars-1)throw std::length_error("File drag size");chars+=names.back().size()+1;}if(chars>(SIZE_MAX-sizeof(DROPFILES))/sizeof(wchar_t))throw std::length_error("File drag size");bytes_.resize(sizeof(DROPFILES)+chars*sizeof(wchar_t));auto*header=reinterpret_cast<DROPFILES*>(bytes_.data());header->pFiles=sizeof(DROPFILES);header->fWide=TRUE;auto*out=reinterpret_cast<wchar_t*>(bytes_.data()+sizeof(DROPFILES));for(const auto&name:names){std::copy(name.begin(),name.end(),out);out+=name.size();*out++=L'\0';}*out=L'\0';}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_IDataObject){*out=static_cast<IDataObject*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC*format)override{return format&&format->cfFormat==CF_HDROP&&(format->tymed&TYMED_HGLOBAL)&&format->dwAspect==DVASPECT_CONTENT?S_OK:DV_E_FORMATETC;}
 HRESULT STDMETHODCALLTYPE GetData(FORMATETC*format,STGMEDIUM*out)override{if(!out)return E_POINTER;if(QueryGetData(format)!=S_OK)return DV_E_FORMATETC;auto handle=GlobalAlloc(GMEM_MOVEABLE,bytes_.size());if(!handle)return E_OUTOFMEMORY;auto*data=GlobalLock(handle);if(!data){GlobalFree(handle);return E_OUTOFMEMORY;}std::memcpy(data,bytes_.data(),bytes_.size());GlobalUnlock(handle);*out={};out->tymed=TYMED_HGLOBAL;out->hGlobal=handle;return S_OK;}
 HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,STGMEDIUM*)override{return DATA_E_FORMATETC;}
 HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,FORMATETC*out)override{if(out)out->ptd=nullptr;return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetData(FORMATETC*,STGMEDIUM*,BOOL)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,IEnumFORMATETC**out)override{if(!out)return E_POINTER;*out=nullptr;if(direction!=DATADIR_GET)return E_NOTIMPL;*out=new Formats;return S_OK;}
 HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*)override{return OLE_E_ADVISENOTSUPPORTED;}
 HRESULT STDMETHODCALLTYPE DUnadvise(DWORD)override{return OLE_E_ADVISENOTSUPPORTED;}
 HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**)override{return OLE_E_ADVISENOTSUPPORTED;}
};
class Source final:public IDropSource {
 std::atomic<ULONG>refs_{1};
 public:HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_IDropSource){*out=static_cast<IDropSource*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,DWORD keys)override{if(escape)return DRAGDROP_S_CANCEL;return (keys&(MK_LBUTTON|MK_RBUTTON))?S_OK:DRAGDROP_S_DROP;}
 HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD)override{return DRAGDROP_S_USEDEFAULTCURSORS;}
};
class Target final:public IDropTarget {
 std::atomic<ULONG>refs_{1};bool acceptable_{};HWND window_{};
 public:Editor*editor{};explicit Target(HWND window,Editor&e):window_(window),editor(&e){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_IDropTarget){*out=static_cast<IDropTarget*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{const auto n=--refs_;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*data,DWORD,POINTL,DWORD*effect)override{if(!effect)return E_POINTER;FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};acceptable_=editor&&editor->onFilesDropped&&data&&data->QueryGetData(&format)==S_OK;*effect=acceptable_&&(*effect&DROPEFFECT_COPY)?DROPEFFECT_COPY:DROPEFFECT_NONE;return S_OK;}
 HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL,DWORD*effect)override{if(!effect)return E_POINTER;*effect=acceptable_&&editor&&(*effect&DROPEFFECT_COPY)?DROPEFFECT_COPY:DROPEFFECT_NONE;return S_OK;}
 HRESULT STDMETHODCALLTYPE DragLeave()override{acceptable_=false;return S_OK;}
 HRESULT STDMETHODCALLTYPE Drop(IDataObject*data,DWORD,POINTL where,DWORD*effect)override{if(!effect)return E_POINTER;const auto allowed=*effect;*effect=DROPEFFECT_NONE;if(!acceptable_||!editor||!(allowed&DROPEFFECT_COPY))return S_OK;FORMATETC format{CF_HDROP,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM medium{};if(!data||data->GetData(&format,&medium)!=S_OK)return S_OK;struct Release{STGMEDIUM&m;~Release(){ReleaseStgMedium(&m);}}release{medium};try{const auto drop=static_cast<HDROP>(medium.hGlobal);const auto count=DragQueryFileW(drop,0xffffffff,nullptr,0);std::vector<std::string>files;files.reserve(count);for(UINT i=0;i<count;++i){const auto length=DragQueryFileW(drop,i,nullptr,0);std::wstring path(std::size_t(length)+1,L'\0');if(DragQueryFileW(drop,i,path.data(),length+1)!=length)continue;path.resize(length);files.push_back(utf8(path));}POINT point{where.x,where.y};ScreenToClient(window_,&point);ControlScope control;if(editor&&editor->onFilesDropped&&editor->onFilesDropped(files,{point.x/editor->nativeWindowScale,point.y/editor->nativeWindowScale}))*effect=DROPEFFECT_COPY;}catch(...){*effect=DROPEFFECT_NONE;}acceptable_=false;return S_OK;}
 HWND window()const{return window_;}
};
}
void installClipboard(){widgets::Clipboard::setBackend([]()->widgets::Text{ControlScope control;ClipboardLock clipboard;if(!clipboard.open||!IsClipboardFormatAvailable(CF_UNICODETEXT))return {};const auto handle=GetClipboardData(CF_UNICODETEXT);if(!handle)return {};const auto*data=static_cast<const wchar_t*>(GlobalLock(handle));if(!data)return {};const auto maximum=GlobalSize(handle)/sizeof(wchar_t);std::size_t length{};while(length<maximum&&data[length])++length;std::string text;try{if(length<maximum)text=utf8({data,length});}catch(...){GlobalUnlock(handle);return {};}GlobalUnlock(handle);return text;},[](const widgets::Text&text){ControlScope control;const auto value=wide(text.utf8());if(value.size()>(SIZE_MAX/sizeof(wchar_t))-1)return;auto memory=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));if(!memory)return;auto*data=GlobalLock(memory);if(!data){GlobalFree(memory);return;}std::memcpy(data,value.c_str(),(value.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);ClipboardLock clipboard;if(!clipboard.open||!EmptyClipboard()||!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);});}
void pollTransfers(){}
bool beginFileDrag(std::uintptr_t,std::span<const std::string>files,bool move){if(files.empty()||!apartment())return false;try{std::unique_ptr<Files,void(*)(Files*)>data(new Files(files),[](Files*p){p->Release();});std::unique_ptr<Source,void(*)(Source*)>source(new Source,[](Source*p){p->Release();});DWORD effect{};const auto result=DoDragDrop(data.get(),source.get(),move?DROPEFFECT_MOVE|DROPEFFECT_COPY:DROPEFFECT_COPY,&effect);return result==DRAGDROP_S_DROP||result==DRAGDROP_S_CANCEL;}catch(...){return false;}}
void*registerDropTarget(std::uintptr_t handle,Editor&editor){installClipboard();if(!editor.onFilesDropped||!apartment())return nullptr;auto*target=new Target(reinterpret_cast<HWND>(handle),editor);if(RegisterDragDrop(reinterpret_cast<HWND>(handle),target)!=S_OK){target->Release();return nullptr;}return target;}
void unregisterDropTarget(void*token){if(auto*target=static_cast<Target*>(token)){target->editor=nullptr;RevokeDragDrop(target->window());target->Release();}}
}

namespace cell::ui {
void shutdownNativePlatformServices() {
    // Invoke on the GUI/lifecycle thread before FreeLibrary, after every view
    // and drag target has closed. COM shutdown is forbidden inside DllMain.
    native::nativeClipboard.close();
    native::nativeApartment.close();
}
}
