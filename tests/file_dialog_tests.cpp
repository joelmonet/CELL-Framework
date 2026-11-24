// SPDX-License-Identifier: Apache-2.0
#include <cell/file_dialog.hpp>
#include <cell/files.hpp>
#include <cell/browser_locations.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace cell;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
ui::FileDialog::Response wait(ui::FileDialog& dialog){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(std::chrono::steady_clock::now()<deadline){if(auto result=dialog.poll())return *result;std::this_thread::sleep_for(std::chrono::milliseconds(5));}throw std::runtime_error("System dialog did not settle");}
}
int main(){try{
#if !defined(_WIN32)&&!defined(__APPLE__)
    const auto directory=std::filesystem::temp_directory_path()/("cell-dialog-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(directory);
    const auto* current=std::getenv("PATH");const std::string previous=current?current:"";
    const auto helper=directory/"zenity";const auto script=[&](std::string_view text){files::atomicWrite(helper,std::as_bytes(std::span(text.data(),text.size())),true);chmod(helper.c_str(),0700);};
    setenv("PATH",(directory.string()+":"+previous).c_str(),1);setenv("CELL_TEST_DIALOG_RESULT",(directory/"Preset with spaces.fxp").c_str(),1);
    try{
        script("#!/bin/sh\nprintf '%s\\n' \"$CELL_TEST_DIALOG_RESULT\"\n");check(ui::FileDialog::available(),"System dialog helper was not found");ui::FileDialog dialog;ui::FileDialog::Request request;request.title="Choose a preset";request.initial=directory;request.extensions={".fxp",".hsvstpreset"};
        check(dialog.open(request),"Dialog launch failed");auto result=wait(dialog);check(!result.cancelled&&result.error.empty()&&result.path==directory/"Preset with spaces.fxp","Asynchronous selection changed the returned filename");
        const auto library=directory/"Drums with spaces";std::filesystem::create_directory(library);const auto other=directory/"Other";std::filesystem::create_directory(other);
        ui::BrowserLocations locations(directory/"preferences"/"folders.txt");locations.remember("drums",other);locations.setDefault("drums",library);ui::BrowserLocations second(directory/"preferences"/"folders.txt");second.setDefault("cab-ir",other);check(locations.initial("drums",directory)==library&&locations.initial("cab-ir",directory)==other,"Separate instances lost the saved default or mixed module libraries");locations.remember("drums",other);check(second.initial("drums",directory)==library,"Loading another sample changed the user's chosen default folder");std::filesystem::remove(library);check(second.initial("drums",directory)==other,"Missing default did not fall back to the last available folder");
        request.directory=true;request.extensions.clear();setenv("CELL_TEST_DIALOG_RESULT",other.c_str(),1);script("#!/bin/sh\nfound=0\nfor arg in \"$@\"; do [ \"$arg\" = --directory ] && found=1; done\n[ $found = 1 ] || exit 2\nprintf '%s\\n' \"$CELL_TEST_DIALOG_RESULT\"\n");check(dialog.open(request),"Folder dialog launch failed");result=wait(dialog);check(result.path==other&&result.error.empty(),"Native folder chooser lost directory mode");request.directory=false;
        script("#!/bin/sh\nexit 1\n");check(dialog.open(request)&&wait(dialog).cancelled,"Cancel was treated as a failed selection");
        script("#!/bin/sh\nexec /bin/sleep 60\n");check(dialog.open(request),"Cancellation fixture did not launch");std::this_thread::sleep_for(std::chrono::milliseconds(80));dialog.cancel();script("#!/bin/sh\nprintf '%s\\n' \"$CELL_TEST_DIALOG_RESULT\"\n");check(dialog.open(request),"Reopening after cancellation failed");result=wait(dialog);check(!result.cancelled&&result.error.empty()&&!result.path.empty(),"A stale dialog result replaced the new selection");
        script("#!/bin/sh\nexec /bin/sleep 60\n");const auto start=std::chrono::steady_clock::now();{ui::FileDialog closing;check(closing.open(request),"Teardown fixture failed");std::this_thread::sleep_for(std::chrono::milliseconds(80));}check(std::chrono::steady_clock::now()-start<std::chrono::seconds(2),"Editor teardown waited for the system dialog");
        setenv("PATH",previous.c_str(),1);unsetenv("CELL_TEST_DIALOG_RESULT");std::filesystem::remove_all(directory);
    }catch(...){setenv("PATH",previous.c_str(),1);unsetenv("CELL_TEST_DIALOG_RESULT");std::filesystem::remove_all(directory);throw;}
    std::cout<<"PASS asynchronous file selection, cancellation, reopening, filename preservation and bounded editor teardown\n";
#else
    std::cout<<"Native dialog interaction requires the platform host fixture\n";
#endif
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
