// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text.hpp>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace cell {
class DynamicLibrary {
 void*handle_{};
 public:DynamicLibrary()=default;~DynamicLibrary(){close();}DynamicLibrary(const DynamicLibrary&)=delete;DynamicLibrary&operator=(const DynamicLibrary&)=delete;
 bool open(const Text&path){close();
#if defined(_WIN32)
 const auto text=path.toStdString();const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);if(count<=0)return false;std::wstring wide(std::size_t(count),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),wide.data(),count);handle_=LoadLibraryExW(wide.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
 handle_=dlopen(path.toRawUTF8(),RTLD_NOW|RTLD_LOCAL);
#endif
 return handle_!=nullptr;}
 void close()noexcept{if(handle_){
#if defined(_WIN32)
 FreeLibrary(static_cast<HMODULE>(handle_));
#else
 dlclose(handle_);
#endif
 handle_=nullptr;}}
 void*getFunction(const char*name)const noexcept {if(!handle_)return nullptr;
#if defined(_WIN32)
 return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_),name));
#else
 return dlsym(handle_,name);
#endif
 }
};
}
