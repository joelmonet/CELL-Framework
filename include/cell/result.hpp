// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text.hpp>
namespace cell {
class Result {
    bool success_=true;
    Text message_;
    Result(bool success,Text message):success_(success),message_(std::move(message)){}
public:
    Result()=default;
    static Result ok(){return {};}
    static Result fail(Text message){return {false,std::move(message)};}
    bool wasOk() const noexcept{return success_;}
    bool failed() const noexcept{return !success_;}
    explicit operator bool() const noexcept{return success_;}
    const Text& getErrorMessage() const noexcept{return message_;}
    // Transitional I/O boundaries can return any status carrying these two
    // observations. No foreign status object or implementation is retained.
    template<class T>requires requires(const T& value){value.wasOk();value.getErrorMessage();}
    Result(const T& value):success_(value.wasOk()),message_(success_?Text{}:Text(value.getErrorMessage())){}
};
} // namespace cell
