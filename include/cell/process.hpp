// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text_list.hpp>
#include <memory>
namespace cell {
// Direct argv invocation, with no shell expansion. Owns and reaps only the
// process it created; cancellation cannot address another process group.
class ChildProcess {
 struct Impl;std::unique_ptr<Impl>impl_;
 public:ChildProcess();~ChildProcess();ChildProcess(const ChildProcess&)=delete;ChildProcess&operator=(const ChildProcess&)=delete;
 bool start(const TextList&,int=0);bool isRunning();void kill()noexcept;int getExitCode();
};
}
