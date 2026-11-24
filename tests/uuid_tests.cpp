// Original Cell Framework implementation; see LICENSE.
#include <cell/uuid.hpp>
#include <set>
#include <iostream>
int main(){try{std::set<std::string>ids;for(int i=0;i<10000;++i){cell::Uuid id;const auto text=id.toString();if(text.length()!=32||text[12]!='4'||(text[16]!='8'&&text[16]!='9'&&text[16]!='a'&&text[16]!='b')||!ids.insert(text.toStdString()).second||id.toDashedString().length()!=36)throw std::runtime_error("Invalid or repeated UUID");}std::cout<<"PASS native system-random UUIDs:10k unique identifiers, version/variant and canonical formatting\n";return 0;}catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}}
