#pragma once
#include <cstddef>
#include <span>
#include <array>
#include <vector>
#include <mutex>
#include <optional>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <string>
namespace cell {
class Engine;

namespace web {
enum class ActionMode { invoke, momentary };
enum class ActionPhase { invoke, press, release };
struct ActionBinding { std::string_view key; ActionMode mode; };
struct ActionResult { bool ok{}; std::string message; };

// Optional, control-thread interface. A successful press owns one release.
// Release must remain available until the press has been cleared.
class ActionTarget {
public:
    virtual ~ActionTarget() = default;
    virtual bool supportsWebAction(std::string_view, ActionMode) const noexcept = 0;
    virtual bool webAction(Engine&, std::string_view, ActionPhase) = 0;
    virtual ActionResult webActionWithArgument(Engine& engine, std::string_view key, ActionPhase phase,
                                               std::string_view argument) {
        if (!argument.empty()) return {false, "This action takes no argument"};
        return {webAction(engine, key, phase), {}};
    }
};
}

enum class RestoreStatus { unknown, pending, succeeded, failed };
inline constexpr char productRestoreMessage[]="Cell.State.Restore.v2";
inline constexpr char productRestoreReplyMessage[]="Cell.State.Restore.Reply.v2";
inline constexpr char productAcknowledgedCommandMessage[]="Cell.Product.Command.v2";
inline constexpr char productCommandReplyMessage[]="Cell.Product.Command.Reply.v2";
// Optional non-audio product operations, routed from the editor to the actual
// processing component. The product validates its bounded command envelope.
class CommandTarget {
public:virtual ~CommandTarget()=default;
 virtual bool executeCommand(std::span<const std::byte>)=0;
 // Non-audio read-only snapshots. No disk/network I/O in this operation.
 virtual std::vector<std::byte> queryProduct(std::span<const std::byte>){return {};}
};
inline constexpr char productCommandMessage[]="Cell.Product.Command.v1";
inline constexpr char productInputMessage[]="Cell.Input.v1";
inline constexpr char productQueryMessage[]="Cell.Product.Query.v1";
inline constexpr char productQueryReplyMessage[]="Cell.Product.Query.Reply.v1";
inline constexpr std::size_t maxProductQueryBytes=16384,maxProductReplyBytes=262144;

// Non-realtime bridge for hosts that deliver processor/controller messages
// asynchronously. Replies belong to a specific request, including after slot
// changes, eviction and timeout. The GUI never waits for a host reply.
class ProductQueries {
public:
 using Clock=std::chrono::steady_clock;
 struct Request {std::uint64_t token{};std::vector<std::byte> bytes;};
 std::optional<Request> begin(std::span<const std::byte> key,Clock::time_point now=Clock::now()){
  if(key.empty()||key.size()>maxProductQueryBytes)return {};std::lock_guard guard(mutex_);Entry* entry=nullptr;
  for(auto& item:entries_)if(equal(item.key,key)){entry=&item;break;}
  if(entry&&entry->waiting&&now-entry->sent<std::chrono::seconds(1))return {};
  if(!entry){for(auto& item:entries_)if(item.key.empty()){entry=&item;break;}
   if(!entry)for(auto& item:entries_)if((!item.waiting||now-item.sent>=std::chrono::seconds(1))&&(!entry||item.sent<entry->sent))entry=&item;
   if(!entry)return {};*entry={};entry->key.assign(key.begin(),key.end());
  }
  Request request{++nextToken_,{key.begin(),key.end()}};entry->token=request.token;entry->sent=now;entry->waiting=true;return request;
 }
 bool complete(std::uint64_t token,std::span<const std::byte> bytes){
  if(bytes.size()>maxProductReplyBytes)return false;std::lock_guard guard(mutex_);
  for(auto& item:entries_)if(item.waiting&&item.token==token){item.reply.assign(bytes.begin(),bytes.end());item.waiting=false;return true;}return false;
 }
 std::vector<std::byte> take(std::span<const std::byte> key){std::lock_guard guard(mutex_);for(auto& item:entries_)if(equal(item.key,key)){std::vector<std::byte> result;result.swap(item.reply);return result;}return {};}
 void invalidate(std::span<const std::byte> key){std::lock_guard guard(mutex_);for(auto& item:entries_)if(equal(item.key,key)){item={};return;}}
 void clear(){std::lock_guard guard(mutex_);for(auto& item:entries_)item={};}
private:
 struct Entry {std::vector<std::byte> key,reply;std::uint64_t token{};Clock::time_point sent{};bool waiting{};};
 static bool equal(const std::vector<std::byte>& a,std::span<const std::byte> b){return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin());}
 std::mutex mutex_;std::array<Entry,16> entries_;std::uint64_t nextToken_{};
};
}
