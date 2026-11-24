// SPDX-License-Identifier: Apache-2.0
#include <cell/file_dialog.hpp>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#else
#ifndef __APPLE__
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

namespace cell::ui {
namespace {
#ifdef _WIN32
std::wstring wide(std::string_view text){if(text.empty())return {};const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);if(!count)throw std::invalid_argument("Invalid dialog text");std::wstring result(std::size_t(count),L' ');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),count);return result;}
template<class T>struct Com {T* value{};~Com(){if(value)value->Release();}T** out(){return &value;}T* operator->(){return value;}};
struct DialogCancellation {IFileDialog* dialog;const jobs::Context* context;HWND parent;};
thread_local DialogCancellation* currentDialog{};
void CALLBACK cancelDialog(HWND,UINT,UINT_PTR,DWORD){if(currentDialog&&(currentDialog->context->cancelled()||(currentDialog->parent&&!IsWindow(currentDialog->parent))))currentDialog->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));}
FileDialog::Response choose(const FileDialog::Request& request,jobs::Context& context){
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return {{},false,"The system file dialog could not be initialized"};
    struct Apartment{~Apartment(){CoUninitialize();}} apartment;
    Com<IFileDialog> dialog;HRESULT status=CoCreateInstance(request.save?CLSID_FileSaveDialog:CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.out()));if(FAILED(status))return {{},false,"The system file dialog could not be opened"};
    DWORD options{};dialog->GetOptions(&options);dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR|FOS_PATHMUSTEXIST|(request.directory?FOS_PICKFOLDERS:request.save?FOS_OVERWRITEPROMPT:FOS_FILEMUSTEXIST));
    const auto title=wide(request.title);dialog->SetTitle(title.c_str());
    std::wstring pattern;for(const auto& extension:request.extensions){if(!pattern.empty())pattern+=L';';pattern+=L'*'+wide(extension);}
    const COMDLG_FILTERSPEC filters[]{{L"Supported files",pattern.c_str()},{L"All files",L"*.*"}};if(!request.directory&&!pattern.empty()){dialog->SetFileTypes(2,filters);dialog->SetFileTypeIndex(1);if(request.save){auto extension=wide(request.extensions.front());if(!extension.empty()&&extension.front()==L'.')extension.erase(extension.begin());dialog->SetDefaultExtension(extension.c_str());}}
    std::error_code error;const auto directory=std::filesystem::is_directory(request.initial,error)?request.initial:request.initial.parent_path();Com<IShellItem> folder;if(!directory.empty()&&SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(),nullptr,IID_PPV_ARGS(folder.out()))))dialog->SetFolder(folder.value);
    if(!request.directory&&!request.initial.empty()&&!std::filesystem::is_directory(request.initial,error)){const auto name=request.initial.filename().native();dialog->SetFileName(name.c_str());}
    const auto parent=reinterpret_cast<HWND>(request.parent);DialogCancellation cancellation{dialog.value,&context,parent};currentDialog=&cancellation;
    const UINT_PTR timer=SetTimer(nullptr,0,40,cancelDialog);if(!timer){currentDialog=nullptr;return {{},false,"The file dialog could not start"};}
    struct Timer{UINT_PTR value;~Timer(){KillTimer(nullptr,value);currentDialog=nullptr;}} timerOwner{timer};
    if(context.cancelled())return {{},true,{}};status=dialog->Show(parent);
    if(status==HRESULT_FROM_WIN32(ERROR_CANCELLED)||context.cancelled())return {{},true,{}};
    if(FAILED(status))return {{},false,"The file selection could not be completed"};Com<IShellItem> selected;if(FAILED(dialog->GetResult(selected.out())))return {{},false,"The selected path is unavailable"};
    PWSTR path{};if(FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH,&path)))return {{},false,"The selected file is not a local file"};struct Name{PWSTR value;~Name(){CoTaskMemFree(value);}} name{path};return {std::filesystem::path(path),false,{}};
}
#elif !defined(__APPLE__)
std::filesystem::path helper(){const auto* path=std::getenv("PATH");if(!path)return {};std::string_view list(path);for(const auto* name:{"zenity","kdialog"}){auto remaining=list;for(;;){const auto separator=remaining.find(':');const auto directory=remaining.substr(0,separator);if(!directory.empty()){auto candidate=std::filesystem::path(std::string(directory))/name;if(candidate.is_absolute()&&access(candidate.c_str(),X_OK)==0)return candidate;}if(separator==remaining.npos)break;remaining.remove_prefix(separator+1);}}return {};}
FileDialog::Response choose(const FileDialog::Request& request,jobs::Context& context){
    const auto executable=helper();if(executable.empty())return {{},false,"No system file chooser is available"};std::vector<std::string> arguments{executable.string()};
    if(executable.filename()=="zenity"){arguments.push_back("--file-selection");arguments.push_back("--title="+request.title);std::error_code initialError;auto initial=request.initial.string();if(std::filesystem::is_directory(request.initial,initialError)&&!initial.ends_with('/'))initial+='/';arguments.push_back("--filename="+initial);if(request.parent)arguments.push_back("--attach="+std::to_string(request.parent));if(request.directory)arguments.push_back("--directory");if(request.save){arguments.push_back("--save");arguments.push_back("--confirm-overwrite");}if(!request.extensions.empty()){std::string filter="--file-filter=Supported files |";for(const auto& extension:request.extensions)filter+=" *"+extension;arguments.push_back(filter);}arguments.push_back("--file-filter=All files | *");}
    else{arguments.push_back(request.directory?"--getexistingdirectory":request.save?"--getsavefilename":"--getopenfilename");arguments.push_back(request.initial.string());std::string filter;for(const auto& extension:request.extensions){if(!filter.empty())filter+=' ';filter+='*'+extension;}if(!request.directory)arguments.push_back(filter+"|Supported files\n*|All files");arguments.push_back("--title");arguments.push_back(request.title);if(request.parent){arguments.push_back("--attach");arguments.push_back(std::to_string(request.parent));}}
    std::vector<char*> argv;for(auto& argument:arguments)argv.push_back(argument.data());argv.push_back(nullptr);int pipe[2];if(pipe2(pipe,O_CLOEXEC|O_NONBLOCK))return {{},false,"The file chooser could not start"};
    const long maximum=std::min(65536l,sysconf(_SC_OPEN_MAX));const auto child=fork();if(child<0){close(pipe[0]);close(pipe[1]);return {{},false,"The file chooser could not start"};}
    if(!child){dup2(pipe[1],STDOUT_FILENO);for(int fd=3;fd<maximum;++fd)close(fd);execv(executable.c_str(),argv.data());_exit(127);}
    close(pipe[1]);std::string output;int status{};bool stopped=false;auto stoppedAt=std::chrono::steady_clock::time_point{};
    for(;;){if(context.cancelled()&&!stopped){kill(child,SIGTERM);stopped=true;stoppedAt=std::chrono::steady_clock::now();}if(stopped&&std::chrono::steady_clock::now()-stoppedAt>std::chrono::milliseconds(250))kill(child,SIGKILL);pollfd descriptor{pipe[0],POLLIN,0};::poll(&descriptor,1,40);char buffer[4096];for(;;){const auto count=read(pipe[0],buffer,sizeof buffer);if(count<=0)break;output.append(buffer,std::size_t(count));if(output.size()>65536){kill(child,SIGTERM);stopped=true;}}
        const auto result=waitpid(child,&status,WNOHANG);if(result==child){for(;;){const auto count=read(pipe[0],buffer,sizeof buffer);if(count<=0)break;output.append(buffer,std::size_t(count));}break;}if(result<0&&errno!=EINTR){status=-1;break;}}
    close(pipe[0]);if(context.cancelled()||status!=-1&&WIFEXITED(status)&&WEXITSTATUS(status)==1)return {{},true,{}};
    if(stopped||status==-1||!WIFEXITED(status)||WEXITSTATUS(status)!=0)return {{},false,"The system file chooser could not complete the selection"};
    if(!output.empty()&&output.back()=='\n')output.pop_back();if(!output.empty()&&output.back()=='\r')output.pop_back();if(output.empty())return {{},true,{}};return {std::filesystem::u8path(output),false,{}};
}
#endif
}
#ifdef __APPLE__
FileDialog::Response chooseMacFile(const FileDialog::Request&,jobs::Context&);
#endif
struct FileDialog::Impl {jobs::Owner owner;jobs::Job job;std::shared_ptr<jobs::Pool> pool;};
FileDialog::FileDialog():impl_(std::make_unique<Impl>()){}
FileDialog::~FileDialog(){cancel();}
bool FileDialog::available(){
#if defined(_WIN32)||defined(__APPLE__)
    return true;
#else
    return !helper().empty();
#endif
}
bool FileDialog::open(Request request){
    cancel();if(!available()||request.extensions.size()>32)return false;if(!impl_->pool)impl_->pool=std::make_shared<jobs::Pool>(1,4);
    impl_->job=impl_->pool->submit(impl_->owner.next(),[request=std::move(request)](jobs::Context& context){
#ifdef __APPLE__
        return jobs::Result::success(chooseMacFile(request,context));
#else
        return jobs::Result::success(choose(request,context));
#endif
    },jobs::Priority::interactive);return bool(impl_->job);
}
void FileDialog::cancel(){impl_->owner.cancel();impl_->job.cancel();impl_->job={};}
bool FileDialog::pending()const{return bool(impl_->job);}
std::optional<FileDialog::Response> FileDialog::poll(){if(!impl_->job||!impl_->job.settled())return {};const auto result=impl_->job.poll();impl_->job={};if(const auto* response=result.get<Response>())return *response;return Response{{},result.status==jobs::Status::cancelled,result.message};}
}
