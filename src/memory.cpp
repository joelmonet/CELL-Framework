#include <cell/memory.hpp>
#include <algorithm>
#include <fstream>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif
namespace cell {
std::optional<std::uint64_t> availableMemoryBytes()noexcept{try{
#ifdef _WIN32
 MEMORYSTATUSEX value{};value.dwLength=sizeof(value);if(GlobalMemoryStatusEx(&value))return value.ullAvailPhys;
#elif defined(__APPLE__)
 vm_statistics64_data_t stats{};mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;vm_size_t page{};const auto host=mach_host_self();const bool ok=host_page_size(host,&page)==KERN_SUCCESS&&host_statistics64(host,HOST_VM_INFO64,reinterpret_cast<host_info64_t>(&stats),&count)==KERN_SUCCESS;mach_port_deallocate(mach_task_self(),host);if(ok)return std::uint64_t(stats.free_count+stats.inactive_count)*page;
#else
 std::ifstream input("/proc/meminfo");std::string key,line;std::uint64_t value{};std::optional<std::uint64_t> result;
 while(input>>key>>value){std::getline(input,line);if(key=="MemAvailable:"){result=value*1024;break;}}
 // Respect a container's remaining allowance as well as system availability.
 std::ifstream limitFile("/sys/fs/cgroup/memory.max"),usedFile("/sys/fs/cgroup/memory.current");std::string limit;std::uint64_t used{};if(limitFile>>limit&&usedFile>>used&&limit!="max"){const auto maximum=std::stoull(limit);const auto remaining=used<maximum?maximum-used:0;result=result?std::min(*result,std::uint64_t(remaining)):remaining;}if(result)return result;
 const auto pages=sysconf(_SC_AVPHYS_PAGES),page=sysconf(_SC_PAGESIZE);if(pages>=0&&page>0)return std::uint64_t(pages)*std::uint64_t(page);
#endif
 return {};
}catch(...){return {};}}
}
