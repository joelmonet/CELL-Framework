#include "../src/detail/integer_decimal.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
int main(){try{
 using cell::detail::integerDecimal;
 auto equal=[](const auto& a,const auto& b){if(a!=b)throw std::runtime_error("Integer text lost precision or sign");};
 equal(integerDecimal(0),"0");equal(integerDecimal(-1),"-1");
 equal(integerDecimal(std::numeric_limits<std::int64_t>::min()),"-9223372036854775808");
 equal(integerDecimal(std::numeric_limits<std::int64_t>::max()),"9223372036854775807");
 equal(integerDecimal(std::numeric_limits<std::uint64_t>::max()),"18446744073709551615");
#ifdef __SIZEOF_INT128__
 const auto maximum=static_cast<__int128>((~static_cast<unsigned __int128>(0))>>1);
 const auto minimum=-maximum-1;const __int128 wide=static_cast<__int128>(1)<<100;
 equal(integerDecimal(maximum),"170141183460469231731687303715884105727");
 equal(integerDecimal(minimum),"-170141183460469231731687303715884105728");
 equal(integerDecimal(wide),"1267650600228229401496703205376");
 equal(integerDecimal(-wide),"-1267650600228229401496703205376");
 if(integerDecimal(wide)==integerDecimal(wide+1))throw std::runtime_error("Wide timestamp collision");
#endif
 std::cout<<"PASS full-width integer timestamp text and signed boundaries\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
