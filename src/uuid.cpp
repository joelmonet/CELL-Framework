// Original Cell Framework implementation; see LICENSE.
#include <cell/uuid.hpp>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <cstdlib>
#else
#include <sys/random.h>
#include <cerrno>
#endif
namespace cell {
void randomBytes(std::span<std::byte>bytes){
    std::size_t at{};while(at<bytes.size()){
#ifdef _WIN32
        const auto count=ULONG(std::min<std::size_t>(bytes.size()-at,std::numeric_limits<ULONG>::max()));
        if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(bytes.data()+at),count,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("System random provider failed");at+=count;
#elif defined(__APPLE__)
        arc4random_buf(bytes.data()+at,bytes.size()-at);at=bytes.size();
#else
        const auto count=::getrandom(bytes.data()+at,std::min<std::size_t>(bytes.size()-at,std::numeric_limits<ssize_t>::max()),0);
        if(count<0&&errno==EINTR)continue;if(count<=0)throw std::system_error(errno,std::generic_category(),"System random provider failed");at+=std::size_t(count);
#endif
    }
}
Text Uuid::toString()const{constexpr char hex[]="0123456789abcdef";std::string value;value.reserve(32);for(auto byte:bytes_){const auto b=std::to_integer<unsigned>(byte);value+=hex[b>>4];value+=hex[b&15];}return Text(std::move(value));}
Text Uuid::toDashedString()const{const auto raw=toString();std::string value;value.reserve(36);for(std::size_t at=0;at<32;++at){if(at==8||at==12||at==16||at==20)value+='-';value+=raw.utf8()[at];}return Text(std::move(value));}
}
