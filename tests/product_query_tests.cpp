#include <cell/commands.hpp>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void queries(){
 using namespace cell;ProductQueries queries;const auto time=ProductQueries::Clock::time_point{};
 const std::array<std::byte,1> a{std::byte{1}},b{std::byte{2}},reply{std::byte{42}};
 auto first=queries.begin(a,time);check(first.has_value()&&first->bytes==std::vector<std::byte>(a.begin(),a.end()),"query bytes were changed");
 check(!queries.begin(a,time+std::chrono::milliseconds(10)),"an in-flight query was duplicated");
 check(queries.complete(first->token,reply)&&queries.take(b).empty(),"reply crossed request keys");
 check(queries.take(a)==std::vector<std::byte>(reply.begin(),reply.end())&&queries.take(a).empty(),"reply was lost or delivered twice");
 auto old=queries.begin(a,time),retry=queries.begin(a,time+std::chrono::seconds(2));check(old&&retry&&old->token!=retry->token,"expired request was not retried");
 check(!queries.complete(old->token,reply)&&queries.complete(retry->token,reply),"stale request completed the replacement");
 auto pending=queries.begin(a,time+std::chrono::seconds(3));queries.invalidate(a);check(!queries.complete(pending->token,reply)&&queries.take(a).empty(),"fresh-data invalidation retained an old reply");
 auto fresh=queries.begin(a,time+std::chrono::seconds(3));check(fresh&&fresh->token!=pending->token,"fresh request reused a stale token");queries.clear();check(!queries.complete(fresh->token,reply),"cleared cache accepted an old reply");
 std::array<ProductQueries::Request,16> requests;for(unsigned i=0;i<16;++i){const std::array<std::byte,1> key{std::byte(i+1)};auto request=queries.begin(key,time);check(request.has_value(),"bounded cache rejected an available entry");requests[i]=std::move(*request);}
 const std::array<std::byte,1> extra{std::byte{99}};check(!queries.begin(extra,time),"cache evicted an active request before timeout");
 check(queries.complete(requests[0].token,reply),"completed entry rejected");auto replacement=queries.begin(extra,time);check(replacement&&!queries.complete(requests[0].token,reply),"eviction accepted a stale reply");
 const std::vector<std::byte> oversizedRequest(maxProductQueryBytes+1),oversizedReply(maxProductReplyBytes+1);check(!queries.begin(oversizedRequest,time)&&!queries.complete(replacement->token,oversizedReply),"query/reply bounds were not enforced");
 check(queries.complete(replacement->token,reply),"rejected oversized reply destroyed its pending request");
}
}
int main(){try{queries();std::cout<<"PASS non-realtime product queries: request isolation, asynchronous delivery, fresh reads, timeout, eviction and size bounds\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
