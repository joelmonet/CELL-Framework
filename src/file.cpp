// Original Cell Framework implementation; see LICENSE.
#include <cell/file.hpp>
#include <cell/files.hpp>
#include <cell/uuid.hpp>
#include <chrono>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <dlfcn.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
namespace cell {
namespace {
Text pathText(const std::filesystem::path&path){
#ifdef _WIN32
    auto display=path.native();
    if(display.rfind(L"\\\\?\\UNC\\",0)==0)display=L"\\\\"+display.substr(8);
    else if(display.rfind(L"\\\\?\\",0)==0&&display.size()>5&&display[5]==L':')display.erase(0,4);
    const auto bytes=std::filesystem::path(display).u8string();
#else
    const auto bytes=path.u8string();
#endif
    return Text(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
}
std::filesystem::path environmentPath(const char*name){
#ifdef _WIN32
    std::wstring key;for(const char*c=name;*c;++c)key+=wchar_t(*c);const auto count=GetEnvironmentVariableW(key.c_str(),nullptr,0);if(!count)return {};std::wstring value(count,L'\0');const auto wrote=GetEnvironmentVariableW(key.c_str(),value.data(),count);if(!wrote||wrote>=count)return {};value.resize(wrote);return value;
#else
    const char*value=std::getenv(name);return value?std::filesystem::path(value):std::filesystem::path{};
#endif
}
void moduleMarker(){}
std::filesystem::path executable(bool module){
#ifdef _WIN32
    HMODULE owner{};if(module&&!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&moduleMarker),&owner))return {};
    std::wstring value(256,L'\0');for(;;){const auto size=GetModuleFileNameW(owner,value.data(),DWORD(value.size()));if(!size)return {};if(size<value.size()){value.resize(size);return value;}if(value.size()>std::numeric_limits<DWORD>::max()/2)return {};value.resize(value.size()*2);}
#else
    if(module){Dl_info info{};if(dladdr(reinterpret_cast<void*>(&moduleMarker),&info)&&info.dli_fname)return std::filesystem::absolute(info.dli_fname);}
#ifdef __APPLE__
    std::uint32_t size{};_NSGetExecutablePath(nullptr,&size);std::string value(size,'\0');if(_NSGetExecutablePath(value.data(),&size)==0)return std::filesystem::path(value.c_str());return {};
#else
    std::error_code error;return std::filesystem::read_symlink("/proc/self/exe",error);
#endif
#endif
}
}
File::File(Text text){
    if(text.isEmpty())return;if(text.containsChar(0))throw std::invalid_argument("A file path cannot contain NUL");
    if(text=="~"){*this=getSpecialLocation(userHomeDirectory);return;}
    if(text.startsWith("~/")){*this=File(getSpecialLocation(userHomeDirectory).nativePath()/std::filesystem::u8path(text.substring(2).utf8()));return;}
    *this=File(std::filesystem::u8path(text.utf8()));
}
File::File(std::filesystem::path path):path_(std::move(path)){
    if(path_.empty())return;if(path_.native().find(typename std::filesystem::path::value_type{})!=std::filesystem::path::string_type::npos)throw std::invalid_argument("A file path cannot contain NUL");
#ifdef _WIN32
    path_=files::windowsPath(path_);
#else
    if(path_.is_relative())path_=std::filesystem::absolute(path_);
#endif
    while(path_!=path_.root_path()&&(path_.filename().empty()||path_.filename()=="."))path_=path_.parent_path();
    // Never erase '..' across symlinks before the filesystem follows the link.
}
Text File::getFullPathName()const{return pathText(path_);}Text File::getFileName()const{return pathText(path_.filename());}Text File::getFileNameWithoutExtension()const{return pathText(path_.stem());}Text File::getFileExtension()const{return pathText(path_.extension());}
File File::getParentDirectory()const{return path_.empty()?File{}:File(path_.parent_path());}
File File::getChildFile(const Text&name)const{if(name.isEmpty())return *this;if(name=="~"||name.startsWith("~/"))return File(name);const auto child=std::filesystem::u8path(name.utf8());if(path_.empty()&&!child.is_absolute())return {};return File(child.is_absolute()?child:path_/child);}
Text File::foreignPath()const{if(path_.empty())return {};std::error_code error;const auto canonical=std::filesystem::weakly_canonical(path_,error);return error?getFullPathName():pathText(canonical);}
File File::getSiblingFile(const Text&name)const{return getParentDirectory().getChildFile(name);}
File File::withFileExtension(const Text&extension)const{if(path_.empty())return {};auto path=path_;path.replace_extension(std::filesystem::u8path(extension.utf8()));return File(std::move(path));}
Text File::getRelativePathFrom(const File&base)const{const auto relative=path_.lexically_relative(base.path_);return relative.empty()?getFullPathName():pathText(relative);}
bool File::isAChildOf(const File&parent)const{
    if(path_.empty()||parent.path_.empty()||*this==parent)return false;
    std::error_code error;auto own=std::filesystem::weakly_canonical(path_,error);if(error)own=path_;error.clear();auto base=std::filesystem::weakly_canonical(parent.path_,error);if(error)base=parent.path_;
    const File target(base);for(File at=File(own).getParentDirectory();!at.path_.empty();){if(at==target)return true;auto next=at.getParentDirectory();if(next==at)break;at=std::move(next);}return false;
}
Text File::fitChildFileName(const Text&source)const{
    const auto name=createLegalFileName(source);std::size_t maximum{};
#ifdef _WIN32
    std::wstring volume(std::max<std::size_t>(path_.native().size()+2,256),L'\0');
    if(GetVolumePathNameW(path_.c_str(),volume.data(),DWORD(volume.size()))){DWORD size{};if(GetVolumeInformationW(volume.c_str(),nullptr,0,nullptr,&size,nullptr,nullptr,0))maximum=size;}
    const auto units=[](const Text&text){std::size_t result{};for(auto code:text)result+=code>0xffff?2:1;return result;};
#else
    const auto size=::pathconf(path_.c_str(),_PC_NAME_MAX);if(size>0)maximum=std::size_t(size);
    const auto units=[](const Text&text){return text.getNumBytesAsUTF8();};
#endif
    if(!maximum||units(name)<=maximum)return name;
    const auto dot=name.lastIndexOfChar('.');const auto extension=dot>0?name.substring(dot):Text{};
    const auto suffix=units(extension);if(suffix+2>=maximum)throw std::length_error("File extension exceeds the filesystem name capacity");
    const auto hash=Text::toHexString(name.hashCode64()).paddedLeft('0',16);
    const auto marker="~"+hash.substring(0,int(std::min<std::size_t>(16,maximum-suffix-1)));
    const auto allowance=maximum-suffix-units(marker);Text prefix;std::size_t used{};
    const auto stem=dot>0?name.substring(0,dot):name;for(auto code:stem){const auto character=Text::charToString(code);const auto width=units(character);if(width>allowance-used)break;prefix+=character;used+=width;}
    return prefix+marker+extension;
}
bool File::exists()const noexcept{std::error_code error;return !path_.empty()&&std::filesystem::exists(path_,error);}bool File::existsAsFile()const noexcept{std::error_code error;return !path_.empty()&&std::filesystem::is_regular_file(path_,error);}bool File::isDirectory()const noexcept{std::error_code error;return !path_.empty()&&std::filesystem::is_directory(path_,error);}
std::int64_t File::getSize()const noexcept{std::error_code error;const auto size=std::filesystem::file_size(path_,error);return error||size>std::uintmax_t(INT64_MAX)?0:std::int64_t(size);}
FileTime File::getLastModificationTime()const noexcept{
#ifdef _WIN32
    const auto handle=CreateFileW(path_.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return {};FILETIME stamp{};const bool okay=GetFileTime(handle,nullptr,nullptr,&stamp)!=FALSE;CloseHandle(handle);if(!okay)return {};
    const auto ticks=(std::uint64_t(stamp.dwHighDateTime)<<32)|stamp.dwLowDateTime;
    return FileTime{std::int64_t(ticks/10000)-INT64_C(11644473600000)};
#else
    std::error_code error;const auto time=std::filesystem::last_write_time(path_,error);if(error)return {};
    // Convert epochs in a wide duration before narrowing to milliseconds. Adding
    // the epoch offset to the native nanosecond counter could otherwise overflow.
    using Floating=std::chrono::duration<long double,std::milli>;
    const std::chrono::time_point<std::chrono::file_clock,Floating>wide{Floating(time.time_since_epoch())};
    const auto value=std::chrono::file_clock::to_sys(wide).time_since_epoch().count();
    if(value<INT64_MIN||value>INT64_MAX)return {};return FileTime{static_cast<std::int64_t>(value)};
#endif
}
bool File::setLastModificationTime(FileTime value)const{
#ifdef _WIN32
    constexpr std::int64_t epoch=INT64_C(11644473600000);
    constexpr auto latest=std::int64_t(UINT64_MAX/10000)-epoch;
    if(value.milliseconds< -epoch||value.milliseconds>latest)return false;
    const auto ticks=std::uint64_t(value.milliseconds+epoch)*10000;
    const FILETIME stamp{DWORD(ticks),DWORD(ticks>>32)};
    const auto handle=CreateFileW(path_.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;const bool okay=SetFileTime(handle,nullptr,nullptr,&stamp)!=FALSE;CloseHandle(handle);return okay;
#else
#ifdef __APPLE__
    // APFS stores signed Unix nanoseconds even when libc++ uses wider file-clock ticks.
    using namespace std::chrono;
    constexpr auto first = duration_cast<milliseconds>(nanoseconds::min()).count();
    constexpr auto last = duration_cast<milliseconds>(nanoseconds::max()).count();
    if (value.milliseconds < first || value.milliseconds > last) return false;
#endif
    using Duration=std::filesystem::file_time_type::duration;using Floating=std::chrono::duration<long double,std::milli>;
    const auto target=std::chrono::file_clock::from_sys(std::chrono::sys_time<Floating>{Floating(value.milliseconds)});
    const auto ticks=std::chrono::duration<long double,Duration::period>(target.time_since_epoch()).count();
    if(ticks<std::numeric_limits<Duration::rep>::lowest()||ticks>std::numeric_limits<Duration::rep>::max())return false;
    std::error_code error;std::filesystem::last_write_time(path_,std::filesystem::file_time_type(Duration(static_cast<Duration::rep>(ticks))),error);return !error;
#endif
}
bool File::hasFileExtension(const Text&extensions)const{const auto suffix=getFileExtension();for(auto extension:TextList::fromTokens(extensions,";,", "")){extension=extension.trim();if(!extension.startsWithChar('.'))extension="."+extension;if(suffix.equalsIgnoreCase(extension))return true;}return false;}
Result File::createDirectory()const{if(path_.empty())return Result::fail("Choose a directory");std::error_code error;if(std::filesystem::create_directories(path_,error)||isDirectory())return Result::ok();return Result::fail(Text("Could not create directory: ")+Text(error.message()));}
Result File::create()const{if(existsAsFile())return Result::ok();FileOutputStream output(*this,FileWriteMode::createNew,0);return output.getStatus();}
bool File::deleteFile()const noexcept{if(path_.empty())return false;std::error_code error;const bool removed=std::filesystem::remove(path_,error);return !error&&(removed||!exists());}
bool File::deleteRecursively()const noexcept{
    if(path_.empty()||path_==path_.root_path())return false;
    std::error_code error;
    const auto resolved=std::filesystem::weakly_canonical(path_,error);
    if(error||resolved==resolved.root_path())return false;
    std::filesystem::remove_all(path_,error);return !error;
}
bool File::copyFileTo(const File&target)const{std::error_code error;return std::filesystem::copy_file(path_,target.path_,std::filesystem::copy_options::none,error);}
bool File::moveFileTo(const File&target)const{return files::publish(path_,target.path_,false).wasOk();}
bool File::replaceWithData(const void*data,std::size_t bytes)const{try{if(!data&&bytes)return false;files::atomicWrite(path_,std::span(static_cast<const std::byte*>(data),bytes),true);return true;}catch(const std::exception&){return false;}}
bool File::replaceWithText(const Text&text)const{return replaceWithData(text.toRawUTF8(),text.getNumBytesAsUTF8());}
bool File::appendText(const Text&text)const{FileOutputStream output(*this,FileWriteMode::append);if(!output.write(text.toRawUTF8(),text.getNumBytesAsUTF8()))return false;output.flush();return output.getStatus().wasOk();}
bool File::loadFileAsData(ByteBlock&destination)const{try{FileInputStream input(*this);if(!input.openedOk()||std::uint64_t(input.getTotalLength())>std::numeric_limits<std::size_t>::max())return false;ByteBlock bytes;bytes.setSize(std::size_t(input.getTotalLength()),false);if(!input.readFully(std::span(static_cast<std::byte*>(bytes.getData()),bytes.getSize())))return false;destination=std::move(bytes);return true;}catch(const std::exception&){return false;}}
Text File::loadFileAsString()const{
    ByteBlock data;if(!loadFileAsData(data))return {};const auto bytes=data.bytes();const auto*raw=reinterpret_cast<const unsigned char*>(bytes.data());const auto size=bytes.size();
    if(size>=4&&((raw[0]==0xff&&raw[1]==0xfe&&raw[2]==0&&raw[3]==0)||(raw[0]==0&&raw[1]==0&&raw[2]==0xfe&&raw[3]==0xff))){if(size%4)return {};const bool little=raw[0]==0xff;Text result;for(std::size_t at=4;at<size;at+=4){std::uint32_t value{};for(unsigned n=0;n<4;++n)value|=std::uint32_t(raw[at+n])<<(8*(little?n:3-n));result+=char32_t(value);}return result;}
    if(size>=2&&((raw[0]==0xff&&raw[1]==0xfe)||(raw[0]==0xfe&&raw[1]==0xff))){if(size%2)return {};const bool little=raw[0]==0xff;const auto unit=[&](std::size_t at){return std::uint32_t(raw[at+(little?0:1)])|(std::uint32_t(raw[at+(little?1:0)])<<8);};Text result;for(std::size_t at=2;at<size;at+=2){auto code=unit(at);if(code>=0xd800&&code<=0xdbff&&at+3<size){const auto low=unit(at+2);if(low>=0xdc00&&low<=0xdfff){code=0x10000+((code-0xd800)<<10)+(low-0xdc00);at+=2;}}result+=char32_t(code);}return result;}
    const auto start=size>=3&&raw[0]==0xef&&raw[1]==0xbb&&raw[2]==0xbf?3u:0u;return size?Text(std::string_view(reinterpret_cast<const char*>(raw+start),size-start)):Text{};
}
File File::getNonexistentChildFile(const Text&prefix,const Text&suffix,bool brackets)const{auto value=getChildFile(fitChildFileName(prefix+suffix));for(std::uint64_t number=2;value.exists();++number){if(number==UINT64_MAX)throw std::length_error("Filename sequence exhausted");value=getChildFile(fitChildFileName(prefix+(brackets?Text(" ("):Text(" "))+Text(number)+(brackets?Text(")"):Text{})+suffix));}return value;}
File File::getNonexistentSibling(bool brackets)const{return getParentDirectory().getNonexistentChildFile(getFileNameWithoutExtension(),getFileExtension(),brackets);}
bool File::isAbsolutePath(const Text&text){return text=="~"||text.startsWith("~/")||std::filesystem::u8path(text.utf8()).is_absolute();}
File File::getCurrentWorkingDirectory(){return File(std::filesystem::current_path());}
File File::getSpecialLocation(SpecialLocationType type){
    if(type==currentModuleFile)return File(executable(true));if(type==currentExecutableFile||type==currentApplicationFile)return File(executable(false));
#ifdef _WIN32
    if(type==tempDirectory){std::wstring path(256,L'\0');for(;;){const auto count=GetTempPathW(DWORD(path.size()),path.data());if(!count)return {};if(count<path.size()){path.resize(count);return File(std::filesystem::path(path));}path.resize(std::size_t(count)+1);}}
    const auto&known=type==userHomeDirectory?FOLDERID_Profile:type==userDocumentsDirectory?FOLDERID_Documents:FOLDERID_RoamingAppData;PWSTR path{};if(SHGetKnownFolderPath(known,0,nullptr,&path)!=S_OK)return {};File result{std::filesystem::path(path)};CoTaskMemFree(path);return result;
#else
    auto home=environmentPath("HOME");if(home.empty())home=std::filesystem::current_path();
    if(type==userHomeDirectory)return File(home);if(type==userDocumentsDirectory)return File(home/"Documents");if(type==tempDirectory)return File(std::filesystem::temp_directory_path());
    const auto configured=environmentPath("XDG_CONFIG_HOME");
#ifdef __APPLE__
    return File(home/"Library/Application Support");
#else
    return File(configured.empty()?home/".config":configured);
#endif
#endif
}
File File::createTempFile(const Text&extension){const auto root=getSpecialLocation(tempDirectory);for(;;){const auto file=root.getChildFile("cell-"+Uuid().toString()+extension);FileOutputStream stream(file,FileWriteMode::createNew,0);if(stream.openedOk())return file;if(!file.exists())throw std::runtime_error(stream.getStatus().getErrorMessage().toStdString());}}
Text File::createLegalFileName(const Text&name){Text result;for(auto code:name)if(code>=32&&code!=127&&!Text("<>:\"/\\|?*").containsChar(code))result+=code;result=result.trim().trimCharactersAtEnd(". ");if(result.isEmpty())result="Untitled";const auto stem=result.upToFirstOccurrenceOf(".",false,false).toUpperCase();if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||(stem.length()==4&&(stem.startsWith("COM")||stem.startsWith("LPT"))&&stem[3]>='1'&&stem[3]<='9'))result="_"+result;return result;}
std::vector<File>File::children(int types,bool recursive,const Text&patterns)const{std::vector<File>result;const auto filters=TextList::fromTokens(patterns,";","");const auto visit=[&](const std::filesystem::directory_entry&entry){std::error_code error;const bool directory=entry.is_directory(error);if(error||!(types&(directory?findDirectories:findFiles)))return;const auto name=pathText(entry.path().filename());for(const auto&pattern:filters)if(name.matchesWildcard(pattern,true)){result.emplace_back(entry.path());break;}};std::error_code error;if(recursive){for(std::filesystem::recursive_directory_iterator at(path_,std::filesystem::directory_options::skip_permission_denied,error),end;at!=end&&!error;at.increment(error))visit(*at);}else{for(std::filesystem::directory_iterator at(path_,std::filesystem::directory_options::skip_permission_denied,error),end;at!=end&&!error;at.increment(error))visit(*at);}return result;}
bool File::operator==(const File&other)const{
#ifdef _WIN32
    if(path_==other.path_)return true;if(path_.native().size()>INT_MAX||other.path_.native().size()>INT_MAX)return false;return CompareStringOrdinal(path_.c_str(),int(path_.native().size()),other.path_.c_str(),int(other.path_.native().size()),TRUE)==CSTR_EQUAL;
#else
    return path_==other.path_;
#endif
}
bool File::operator<(const File&other)const{
#ifdef _WIN32
    if(path_.native().size()<=INT_MAX&&other.path_.native().size()<=INT_MAX){const auto result=CompareStringOrdinal(path_.c_str(),int(path_.native().size()),other.path_.c_str(),int(other.path_.native().size()),TRUE);if(result)return result==CSTR_LESS_THAN;}
#endif
    return path_<other.path_;
}
TemporaryFile::TemporaryFile(const File&target):target_(target){if(target.nativePath().empty()||target.nativePath().filename().empty())throw std::invalid_argument("Choose a target file");for(;;){temporary_=target.getSiblingFile(".cell-temp-"+Uuid().toString());FileOutputStream output(temporary_,FileWriteMode::createNew,0);if(output.openedOk())break;if(!temporary_.exists())throw std::runtime_error(output.getStatus().getErrorMessage().toStdString());}}
TemporaryFile::~TemporaryFile(){if(!committed_)temporary_.deleteFile();}
bool TemporaryFile::overwriteTargetFileWithTemporary(){const auto result=files::publish(temporary_.nativePath(),target_.nativePath(),true);if(result.wasOk())committed_=true;return result.wasOk();}
}
