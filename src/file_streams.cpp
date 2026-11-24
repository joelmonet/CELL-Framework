// Original Cell Framework implementation; see LICENSE.
#include <cell/file.hpp>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include "windows_file_io.hpp"
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#endif
namespace cell {
namespace {
Result ioError(const char*action){
#ifdef _WIN32
    return Result::fail(Text(action)+": "+Text(std::system_category().message(int(GetLastError()))));
#else
    return Result::fail(Text(action)+": "+Text(std::generic_category().message(errno)));
#endif
}
struct Handle {
#ifdef _WIN32
    HANDLE value=INVALID_HANDLE_VALUE;
    bool valid()const noexcept{return value!=INVALID_HANDLE_VALUE;}
    ~Handle(){if(valid())CloseHandle(value);}
    bool seek(std::int64_t position){LARGE_INTEGER at{},result{};at.QuadPart=position;return SetFilePointerEx(value,at,&result,FILE_BEGIN)&&result.QuadPart==position;}
    std::int64_t length()const{LARGE_INTEGER result{};return GetFileSizeEx(value,&result)?result.QuadPart:-1;}
#else
    int value=-1;
    bool valid()const noexcept{return value>=0;}
    ~Handle(){if(valid())::close(value);}
    bool seek(std::int64_t position){return ::lseek(value,off_t(position),SEEK_SET)==position;}
    std::int64_t length()const{struct stat status{};return ::fstat(value,&status)==0&&S_ISREG(status.st_mode)?std::int64_t(status.st_size):-1;}
#endif
};
}
struct FileInputStream::Impl {Handle handle;std::int64_t position{},length{};Result status=Result::ok();};
FileInputStream::FileInputStream(const File&file):impl_(std::make_unique<Impl>()){
    auto&p=*impl_;
#ifdef _WIN32
    p.handle.value = files::detail::openSharedWindowsFile(file.nativePath().c_str(), GENERIC_READ, FILE_ATTRIBUTE_NORMAL);
#else
    p.handle.value=::open(file.nativePath().c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK);
#endif
    if(!p.handle.valid()){p.status=ioError("Could not open input file");return;}
    p.length=p.handle.length();if(p.length<0)p.status=Result::fail("Input is not a supported regular file");
}
FileInputStream::~FileInputStream()=default;
bool FileInputStream::openedOk()const noexcept{return impl_->handle.valid()&&impl_->status.wasOk();}
const Result&FileInputStream::getStatus()const noexcept{return impl_->status;}
int FileInputStream::read(void*data,int amount){
    auto&p=*impl_;if(amount<=0||!p.status.wasOk())return 0;if(!data){p.status=Result::fail("Missing input destination");return 0;}
#ifdef _WIN32
    DWORD count{};if(!ReadFile(p.handle.value,data,DWORD(amount),&count,nullptr)){p.status=ioError("Could not read input file");return 0;}
#else
    ssize_t count;do{count=::read(p.handle.value,data,std::size_t(amount));}while(count<0&&errno==EINTR);
    if(count<0){p.status=ioError("Could not read input file");return 0;}
#endif
    p.position+=std::int64_t(count);return int(count);
}
std::int64_t FileInputStream::getPosition()const{return impl_->position;}
std::int64_t FileInputStream::getTotalLength()const{return impl_->length;}
bool FileInputStream::isExhausted()const{return impl_->status.failed()||impl_->position>=impl_->length;}
bool FileInputStream::setPosition(std::int64_t at){auto&p=*impl_;if(at<0||p.status.failed())return false;if(!p.handle.seek(at)){p.status=ioError("Could not seek input file");return false;}p.position=at;return true;}

struct FileOutputStream::Impl {
    Handle handle;Result status=Result::ok();std::vector<std::byte>buffer;std::size_t buffered{};std::int64_t position{};FileWriteMode mode;
    bool raw(const void*data,std::size_t amount){
        if(status.failed())return false;const auto*bytes=static_cast<const std::byte*>(data);std::size_t at{};
        while(at<amount){
#ifdef _WIN32
            DWORD count{};if(!WriteFile(handle.value,bytes+at,DWORD(std::min<std::size_t>(amount-at,std::numeric_limits<DWORD>::max())),&count,nullptr)||!count){status=ioError("Could not write output file");return false;}
#else
            const auto count=::write(handle.value,bytes+at,std::min<std::size_t>(amount-at,std::numeric_limits<ssize_t>::max()));
            if(count<0&&errno==EINTR)continue;if(count<=0){status=ioError("Could not write output file");return false;}
#endif
            at+=std::size_t(count);
        }return true;
    }
    bool drain(){if(!buffered)return status.wasOk();const auto count=buffered;buffered=0;return raw(buffer.data(),count);}
};
FileOutputStream::FileOutputStream(const File&file,FileWriteMode mode,std::size_t bufferBytes):impl_(std::make_unique<Impl>()){
    auto&p=*impl_;p.mode=mode;p.buffer.resize(bufferBytes);
#ifdef _WIN32
    const DWORD disposition=mode==FileWriteMode::createNew?CREATE_NEW:OPEN_ALWAYS;
    p.handle.value=CreateFileW(file.nativePath().c_str(),mode==FileWriteMode::append?(FILE_APPEND_DATA|FILE_READ_ATTRIBUTES):GENERIC_WRITE,FILE_SHARE_READ,nullptr,disposition,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(p.handle.valid()&&GetFileType(p.handle.value)!=FILE_TYPE_DISK)p.status=Result::fail("Output is not a regular file");
    // Truncate the existing handle instead of superseding the file: NTFS must
    // retain sparse/compression metadata before any distant recorder write.
    if(p.handle.valid()&&p.status.wasOk()&&mode==FileWriteMode::truncate&&!SetEndOfFile(p.handle.value))p.status=ioError("Could not truncate output file");
#else
    const int flags=O_WRONLY|O_CREAT|O_CLOEXEC|O_NONBLOCK|(mode==FileWriteMode::createNew?O_EXCL:mode==FileWriteMode::truncate?O_TRUNC:O_APPEND);
    p.handle.value=::open(file.nativePath().c_str(),flags,0600);if(p.handle.valid()&&p.handle.length()<0)p.status=Result::fail("Output is not a regular file");
#endif
    if(!p.handle.valid()){p.status=ioError("Could not open output file");return;}
    if(p.status.failed())return;
    if(mode==FileWriteMode::append){p.position=p.handle.length();if(p.position<0)p.status=ioError("Could not inspect output file");}
}
FileOutputStream::~FileOutputStream(){impl_->drain();}
bool FileOutputStream::openedOk()const noexcept{return impl_->handle.valid()&&impl_->status.wasOk();}
const Result&FileOutputStream::getStatus()const noexcept{return impl_->status;}
bool FileOutputStream::write(const void*data,std::size_t amount){
    auto&p=*impl_;if(p.status.failed())return false;if(amount>std::uint64_t(INT64_MAX-p.position)||(!data&&amount)){p.status=Result::fail("Output position or payload is not representable");return false;}
    const auto*bytes=static_cast<const std::byte*>(data);std::size_t at{};
    while(at<amount){
        if(!p.buffered&&(p.buffer.empty()||amount-at>=p.buffer.size())){if(!p.raw(bytes+at,amount-at))return false;p.position+=std::int64_t(amount-at);return true;}
        const auto count=std::min(p.buffer.size()-p.buffered,amount-at);std::memcpy(p.buffer.data()+p.buffered,bytes+at,count);p.buffered+=count;p.position+=std::int64_t(count);at+=count;
        if(p.buffered==p.buffer.size()&&!p.drain())return false;
    }return true;
}
void FileOutputStream::flush(){auto&p=*impl_;if(!p.drain())return;
#ifdef _WIN32
    if(!FlushFileBuffers(p.handle.value))p.status=ioError("Could not flush output file");
#else
    if(::fsync(p.handle.value))p.status=ioError("Could not flush output file");
#endif
}
std::int64_t FileOutputStream::getPosition()const{return impl_->position;}
bool FileOutputStream::setPosition(std::int64_t at){auto&p=*impl_;if(at<0||p.status.failed()||p.mode==FileWriteMode::append||!p.drain())return false;if(!p.handle.seek(at)){p.status=ioError("Could not seek output file");return false;}p.position=at;return true;}
Result FileOutputStream::truncate(){auto&p=*impl_;if(!p.drain())return p.status;
#ifdef _WIN32
    if(!SetEndOfFile(p.handle.value))p.status=ioError("Could not truncate output file");
#else
    if(::ftruncate(p.handle.value,off_t(p.position)))p.status=ioError("Could not truncate output file");
#endif
    return p.status;
}
std::unique_ptr<FileInputStream>File::createInputStream()const{auto stream=std::make_unique<FileInputStream>(*this);return stream->openedOk()?std::move(stream):nullptr;}
std::unique_ptr<FileOutputStream>File::createOutputStream(FileWriteMode mode)const{auto stream=std::make_unique<FileOutputStream>(*this,mode);return stream->openedOk()?std::move(stream):nullptr;}
}
