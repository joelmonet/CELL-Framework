#include <cell/files.hpp>
#include <fstream>
#include <algorithm>
#include <system_error>
#include <atomic>
#include <cstring>
#include <limits>
#include <cell/uuid.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif
namespace cell::files {
#ifdef _WIN32
std::filesystem::path windowsPath(const std::filesystem::path& path) {
    if(path.empty())return {};
    auto absolute=path;absolute.make_preferred();
    auto native=absolute.native();
    const bool extended=native.rfind(L"\\\\?\\",0)==0||native.rfind(L"\\\\.\\",0)==0;
    // Keep component traversal intact. Lexically erasing link/.. before the
    // filesystem follows the link can select a different recording or preset.
    if(!absolute.is_absolute()) {
        if(!absolute.has_root_name()) {
            const auto cwd=std::filesystem::current_path();
            absolute=absolute.has_root_directory()?cwd.root_name()/absolute:cwd/absolute;
        } else {
            // A drive-relative path uses that drive's current directory. Resolve
            // only the drive anchor; retain every caller-supplied component.
            const auto drive=absolute.root_name().native()+L".";
            const auto count=GetFullPathNameW(drive.c_str(),0,nullptr,nullptr);
            if(!count)throw std::system_error(int(GetLastError()),std::system_category(),"Could not resolve native drive");
            std::wstring base(count,L'\0');const auto written=GetFullPathNameW(drive.c_str(),count,base.data(),nullptr);
            if(!written||written>=count)throw std::runtime_error("Native drive changed while resolving");
            base.resize(written);absolute=std::filesystem::path(base)/absolute.relative_path();
        }
    }
    absolute.make_preferred();native=absolute.native();
    if(!extended)absolute=native.rfind(L"\\\\",0)==0?std::filesystem::path(L"\\\\?\\UNC\\"+native.substr(2)):std::filesystem::path(L"\\\\?\\"+native);
    bool parents=false;for(const auto&component:absolute.relative_path())parents|=component==L"..";
    if(!parents)return absolute;
    auto resolved=absolute.root_path();
    for(const auto&component:absolute.relative_path()) {
        if(component==L".")continue;
        if(component!=L"..") { resolved/=component;continue; }
        // Win32 otherwise normalizes '..' before following a directory link.
        // Resolve the actual directory at this boundary, then choose its parent.
        struct DirectoryHandle {HANDLE value;~DirectoryHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} directory{CreateFileW(resolved.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
        if(directory.value==INVALID_HANDLE_VALUE)throw std::system_error(int(GetLastError()),std::system_category(),"Cannot traverse parent of unavailable directory");
        BY_HANDLE_FILE_INFORMATION information{};
        if(!GetFileInformationByHandle(directory.value,&information)||!(information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Parent traversal requires an existing directory");
        const DWORD count=GetFinalPathNameByHandleW(directory.value,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(!count)throw std::system_error(int(GetLastError()),std::system_category(),"Cannot resolve directory link");
        std::wstring physical(std::size_t(count)+1,L'\0');
        const auto written=GetFinalPathNameByHandleW(directory.value,physical.data(),DWORD(physical.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if(!written||written>=physical.size())throw std::runtime_error("Directory link changed while resolving");
        physical.resize(written);resolved=std::filesystem::path(physical).parent_path();
    }
    return resolved;
}
namespace {
DWORD renameOpenFile(HANDLE source, const std::filesystem::path& to, bool replace) {
    const auto& name = to.native();
    // POSIX replacement keeps the old file object accessible to existing readers.
    constexpr DWORD replaceAndKeepReaders = 0x1 | 0x2;
    constexpr auto renameInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    constexpr auto nameOffset = offsetof(FILE_RENAME_INFO, FileName);
    if (name.size() > (std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO)) / sizeof(wchar_t))
        return ERROR_FILENAME_EXCED_RANGE;
    const auto nameBytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    const auto size = std::max<std::size_t>(sizeof(FILE_RENAME_INFO), nameOffset + nameBytes);
    std::vector<FILE_RENAME_INFO> storage((size + sizeof(FILE_RENAME_INFO) - 1) / sizeof(FILE_RENAME_INFO));
    auto& info = storage.front();
    // Older SDK headers call this first DWORD ReplaceIfExists.
    const DWORD flags = replace ? replaceAndKeepReaders : 0;
    std::memcpy(&info, &flags, sizeof(flags));
    info.RootDirectory = nullptr;
    info.FileNameLength = nameBytes;
    std::memcpy(reinterpret_cast<std::byte*>(storage.data()) + nameOffset, name.data(), nameBytes);
    return SetFileInformationByHandle(source, renameInfoEx, storage.data(), static_cast<DWORD>(size)) ?
        ERROR_SUCCESS : GetLastError();
}

bool needsLegacyRename(DWORD error) {
    return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_INVALID_FUNCTION || error == ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD moveWindows(const std::filesystem::path& from, const std::filesystem::path& to, bool replace) {
    return MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) ?
        ERROR_SUCCESS : GetLastError();
}

DWORD publishWindows(const std::filesystem::path& from,
                     const std::filesystem::path& to, bool replace) {
    if (replace) {
        const auto source = CreateFileW(from.c_str(), DELETE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (source == INVALID_HANDLE_VALUE)
            return GetLastError();
        const auto error = renameOpenFile(source, to, true);
        CloseHandle(source);
        if (!needsLegacyRename(error))
            return error;
    }
    return moveWindows(from, to, replace);
}
}

#endif
std::vector<std::byte> readBounded(const std::filesystem::path& path,std::size_t limit){
    std::ifstream file(
#ifdef _WIN32
        windowsPath(path),
#else
        path,
#endif
        std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Could not open file");auto size=file.tellg();if(size<0||std::uint64_t(size)>limit)throw std::runtime_error("File exceeds the supported size");std::vector<std::byte> bytes(static_cast<std::size_t>(size));file.seekg(0);if(size&&!file.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("Could not read file");return bytes;
}
struct AtomicWriter::Impl {
    std::filesystem::path path,temporary;bool replace{},committed{};
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    void close()noexcept{if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}}
#else
    int handle=-1;
    void close()noexcept{if(handle>=0){::close(handle);handle=-1;}}
#endif
    ~Impl(){close();if(!committed&&!temporary.empty()){
#ifdef _WIN32
        DeleteFileW(temporary.c_str());
#else
        ::unlink(temporary.c_str());
#endif
    }}
};
AtomicWriter::AtomicWriter(std::filesystem::path path,bool replace):impl_(std::make_unique<Impl>()){
    if(path.filename().empty())throw std::invalid_argument("Choose a file name");
#ifdef _WIN32
    path=windowsPath(path);
#endif
    impl_->path=std::move(path);impl_->replace=replace;
    for(;;){auto candidate=impl_->path.parent_path()/std::filesystem::u8path(".cell-write-"+Uuid().toString().toStdString());
#ifdef _WIN32
        auto handle=CreateFileW(candidate.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle!=INVALID_HANDLE_VALUE){impl_->temporary=std::move(candidate);impl_->handle=handle;return;}if(GetLastError()!=ERROR_FILE_EXISTS&&GetLastError()!=ERROR_ALREADY_EXISTS)break;
#else
        auto handle=::open(candidate.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(handle>=0){impl_->temporary=std::move(candidate);impl_->handle=handle;return;}if(errno!=EEXIST)break;
#endif
    }
    throw std::runtime_error("Could not create temporary file");
}
AtomicWriter::~AtomicWriter()=default;
void AtomicWriter::write(std::span<const std::byte> bytes){
    auto& p=*impl_;if(p.committed)throw std::logic_error("Output already committed");std::size_t offset=0;
#ifdef _WIN32
    if(p.handle==INVALID_HANDLE_VALUE)throw std::logic_error("Output is closed");while(offset<bytes.size()){DWORD wrote=0;if(!WriteFile(p.handle,bytes.data()+offset,DWORD(std::min<std::size_t>(bytes.size()-offset,1u<<20)),&wrote,nullptr)||!wrote)throw std::runtime_error("Could not write file");offset+=wrote;}
#else
    if(p.handle<0)throw std::logic_error("Output is closed");while(offset<bytes.size()){const auto count=::write(p.handle,bytes.data()+offset,std::min<std::size_t>(bytes.size()-offset,1u<<20));if(count<0&&errno==EINTR)continue;if(count<=0)throw std::system_error(errno,std::generic_category(),"Could not write file");offset+=std::size_t(count);}
#endif
}
void AtomicWriter::commit(){
    auto& p=*impl_;if(p.committed)throw std::logic_error("Output already committed");
#ifdef _WIN32
    if (p.handle == INVALID_HANDLE_VALUE)
        throw std::logic_error("Output is closed");
    if (!FlushFileBuffers(p.handle))
        throw std::system_error(int(GetLastError()), std::system_category(), "Could not flush file");
    // Keep the exclusive temporary handle through the rename. A scanner
    // could acquire a conflicting handle in a close-and-reopen window.
    auto error = renameOpenFile(p.handle, p.path, p.replace);
    p.close();
    if (needsLegacyRename(error))
        error = moveWindows(p.temporary, p.path, p.replace);
    if (error != ERROR_SUCCESS)
        throw std::system_error(int(error), std::system_category(), "Could not install file");
#else
    if(p.handle<0)throw std::logic_error("Output is closed");if(::fsync(p.handle))throw std::system_error(errno,std::generic_category(),"Could not flush file");const int handle=p.handle;p.handle=-1;if(::close(handle))throw std::system_error(errno,std::generic_category(),"Could not close file");
    if(p.replace){if(::rename(p.temporary.c_str(),p.path.c_str()))throw std::system_error(errno,std::generic_category(),"Could not replace file");}
    else{if(::link(p.temporary.c_str(),p.path.c_str()))throw std::system_error(errno,std::generic_category(),"File already exists or could not be installed");::unlink(p.temporary.c_str());}
#endif
    p.committed=true;
}
void atomicWrite(const std::filesystem::path& path,std::span<const std::byte> bytes,bool replace){AtomicWriter writer(path,replace);writer.write(bytes);writer.commit();}
Result publish(const std::filesystem::path&from,const std::filesystem::path&to,bool replace){
    std::error_code error;if(from.empty()||to.empty()||from==to||!std::filesystem::is_regular_file(from,error)||error)return Result::fail("The completed output file is unavailable");
#ifdef _WIN32
    const auto nativeError = publishWindows(windowsPath(from), windowsPath(to), replace);
    if (nativeError == ERROR_SUCCESS)
        return Result::ok();
    return Result::fail(Text("Cannot publish output: ") + Text(std::system_category().message(int(nativeError))));
#else
    if(replace){if(::rename(from.c_str(),to.c_str())==0)return Result::ok();}
    else{
#if defined(__linux__) && defined(SYS_renameat2)
        if(::syscall(SYS_renameat2,AT_FDCWD,from.c_str(),AT_FDCWD,to.c_str(),1)==0)return Result::ok();
        if(errno!=ENOSYS&&errno!=EINVAL&&errno!=EOPNOTSUPP)return Result::fail(Text("Cannot publish output: ")+Text(std::generic_category().message(errno)));
#endif
        if(::link(from.c_str(),to.c_str())==0){::unlink(from.c_str());return Result::ok();}
    }
    return Result::fail(Text("Cannot publish output: ")+Text(std::generic_category().message(errno)));
#endif
}
}
