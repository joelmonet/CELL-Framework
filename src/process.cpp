// Original Cell Framework implementation; see LICENSE.
#include <cell/process.hpp>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif !defined(CELL_NO_CHILD_PROCESS)
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
extern char**environ;
#endif
namespace cell {
struct ChildProcess::Impl {
#if defined(_WIN32)
 HANDLE process{};
#elif !defined(CELL_NO_CHILD_PROCESS)
 pid_t pid=-1;
#endif
 int exit=-1;bool running=false;
};
ChildProcess::ChildProcess():impl_(std::make_unique<Impl>()){}
ChildProcess::~ChildProcess(){kill();}
bool ChildProcess::start(const TextList&arguments,int){if(arguments.isEmpty()||impl_->running)return false;impl_->exit=-1;
#if defined(CELL_NO_CHILD_PROCESS)
 return false;
#elif defined(_WIN32)
 std::wstring command;
 for(const auto&arg:arguments){const auto text=arg.toStdString();const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);if(count<0||(!count&&!text.empty()))return false;std::wstring wide(std::size_t(count),L'\0');if(count)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),wide.data(),count);if(!command.empty())command+=L' ';command+=L'"';unsigned slashes=0;for(wchar_t c:wide){if(c==L'\\'){++slashes;continue;}if(c==L'"'){command.append(slashes*2+1,L'\\');command+=c;}else{command.append(slashes,L'\\');command+=c;}slashes=0;}command.append(slashes*2,L'\\');command+=L'"';}
 STARTUPINFOW start{};start.cb=sizeof start;PROCESS_INFORMATION info{};if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&info))return false;CloseHandle(info.hThread);impl_->process=info.hProcess;
#else
 std::vector<std::string>storage;std::vector<char*>argv;for(const auto&arg:arguments)storage.push_back(arg.toStdString());for(auto&arg:storage)argv.push_back(arg.data());argv.push_back(nullptr);pid_t pid{};const int result=posix_spawnp(&pid,argv[0],nullptr,nullptr,argv.data(),environ);if(result)return false;impl_->pid=pid;
#endif
#if !defined(CELL_NO_CHILD_PROCESS)
 impl_->running=true;return true;
#endif
}
bool ChildProcess::isRunning(){if(!impl_->running)return false;
#if defined(_WIN32)
 DWORD code{};if(!GetExitCodeProcess(impl_->process,&code))return false;if(code==STILL_ACTIVE)return true;impl_->exit=int(code);CloseHandle(impl_->process);impl_->process=nullptr;
#elif !defined(CELL_NO_CHILD_PROCESS)
 int status{};const auto result=waitpid(impl_->pid,&status,WNOHANG);if(result==0||(result<0&&errno==EINTR))return true;if(result==impl_->pid)impl_->exit=WIFEXITED(status)?WEXITSTATUS(status):WIFSIGNALED(status)?128+WTERMSIG(status):-1;impl_->pid=-1;
#endif
 impl_->running=false;return false;}
void ChildProcess::kill()noexcept{if(!impl_||!impl_->running)return;
#if defined(_WIN32)
 TerminateProcess(impl_->process,1);WaitForSingleObject(impl_->process,INFINITE);CloseHandle(impl_->process);impl_->process=nullptr;impl_->exit=1;
#elif !defined(CELL_NO_CHILD_PROCESS)
 if(impl_->pid>0){::kill(impl_->pid,SIGKILL);int status{};while(waitpid(impl_->pid,&status,0)<0&&errno==EINTR){}impl_->pid=-1;impl_->exit=128+SIGKILL;}
#endif
 impl_->running=false;}
int ChildProcess::getExitCode(){isRunning();return impl_->exit;}
}
