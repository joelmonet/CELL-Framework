// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/file.hpp>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif
namespace cellTest {
// Only owned test files. Never extend a Windows file to a huge logical size
// before NTFS has acknowledged sparse allocation.
inline cell::FileWriteMode prepareSparseFixture(const cell::File&file){
#ifdef _WIN32
 const auto handle=CreateFileW(file.nativePath().c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create owned sparse fixture");DWORD returned{};const bool okay=DeviceIoControl(handle,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr)!=FALSE;CloseHandle(handle);
 if(!okay)throw std::runtime_error("Fixture filesystem has no sparse-file support; no large allocation attempted");return cell::FileWriteMode::truncate;
#else
 (void)file;return cell::FileWriteMode::createNew;
#endif
}
}
