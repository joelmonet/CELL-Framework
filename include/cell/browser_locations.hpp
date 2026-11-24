// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cell/files.hpp>
#include <filesystem>
#include <iomanip>
#include <map>
#include <sstream>
namespace cell::ui {
// User preferences, never part of a plug-in preset. Read afresh for each edit so
// separate plug-in instances do not overwrite each other's library locations.
class BrowserLocations {
public:
 explicit BrowserLocations(std::filesystem::path file):file_(std::move(file)){}
 std::filesystem::path initial(std::string_view key,std::filesystem::path fallback={})const {
  const auto rows=read();const auto at=rows.find(std::string(key));
  if(at!=rows.end())for(const auto& candidate:{at->second.first,at->second.second}){
   const auto path=std::filesystem::u8path(candidate);std::error_code error;
   if(path.is_absolute()&&std::filesystem::is_directory(path,error))return path;
  }return fallback;
 }
 std::filesystem::path defaultFolder(std::string_view key)const{const auto rows=read();const auto found=rows.find(std::string(key));if(found==rows.end())return {};const auto path=std::filesystem::u8path(found->second.first);std::error_code error;return path.is_absolute()&&std::filesystem::is_directory(path,error)?path:std::filesystem::path{};}
 void remember(std::string_view key,const std::filesystem::path& path)const {write(key,path,false);}
 void setDefault(std::string_view key,const std::filesystem::path& path)const {write(key,path,true);}
private:
 using Rows=std::map<std::string,std::pair<std::string,std::string>>;
 Rows read()const {Rows rows;try{const auto bytes=files::readBounded(file_,1024*1024);std::istringstream in(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));std::string key,preferred,last;while(rows.size()<256&&in>>std::quoted(key)>>std::quoted(preferred)>>std::quoted(last))rows[key]={preferred,last};}catch(const std::exception&){}return rows;}
 void write(std::string_view key,const std::filesystem::path& path,bool preferred)const {
  std::error_code error;if(key.empty()||key.size()>128||!path.is_absolute()||!std::filesystem::is_directory(path,error))throw std::invalid_argument("Choose an existing library folder");
  auto rows=read();const auto utf8=path.u8string();auto& row=rows[std::string(key)];(preferred?row.first:row.second)=std::string(reinterpret_cast<const char*>(utf8.data()),utf8.size());
  std::ostringstream out;for(const auto& [name,paths]:rows)out<<std::quoted(name)<<' '<<std::quoted(paths.first)<<' '<<std::quoted(paths.second)<<'\n';
  std::filesystem::create_directories(file_.parent_path());const auto text=out.str();files::atomicWrite(file_,std::as_bytes(std::span(text.data(),text.size())),true);
 }
 std::filesystem::path file_;
};
}
