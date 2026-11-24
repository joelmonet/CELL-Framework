// SPDX-License-Identifier: Apache-2.0
#include <cell/file_dialog.hpp>
#include <condition_variable>
#include <mutex>
#import <Cocoa/Cocoa.h>

namespace cell::ui {
FileDialog::Response chooseMacFile(const FileDialog::Request& request,jobs::Context& context){
    struct Selection{std::mutex mutex;std::condition_variable condition;bool done{};FileDialog::Response result;NSSavePanel* panel;};
    const auto selection=std::make_shared<Selection>();const auto owned=std::make_shared<FileDialog::Request>(request);
    dispatch_async(dispatch_get_main_queue(),^{
        {std::lock_guard guard(selection->mutex);if(selection->done)return;}
        NSSavePanel* panel=owned->save?[NSSavePanel savePanel]:[NSOpenPanel openPanel];selection->panel=panel;
        panel.title=[NSString stringWithUTF8String:owned->title.c_str()];panel.canCreateDirectories=YES;panel.allowsOtherFileTypes=YES;
        NSMutableArray<NSString*>* types=[NSMutableArray array];for(const auto& extension:owned->extensions){const auto value=extension.starts_with('.')?extension.substr(1):extension;[types addObject:[NSString stringWithUTF8String:value.c_str()]];}if(types.count)panel.allowedFileTypes=types;
        std::error_code error;const auto directory=std::filesystem::is_directory(owned->initial,error)?owned->initial:owned->initial.parent_path();if(!directory.empty())panel.directoryURL=[NSURL fileURLWithFileSystemRepresentation:directory.c_str() isDirectory:YES relativeToURL:nil];
        if(owned->save)panel.nameFieldStringValue=[NSString stringWithUTF8String:owned->initial.filename().c_str()];else{NSOpenPanel* open=(NSOpenPanel*)panel;open.canChooseDirectories=owned->directory;open.canChooseFiles=!owned->directory;open.allowsMultipleSelection=NO;}
        void (^completion)(NSModalResponse)=^(NSModalResponse response){std::lock_guard guard(selection->mutex);if(selection->done)return;if(response==NSModalResponseOK&&panel.URL)selection->result.path=std::filesystem::path(panel.URL.fileSystemRepresentation);else selection->result.cancelled=true;selection->done=true;selection->panel=nil;selection->condition.notify_one();};
        NSView* parent=owned->parent?(__bridge NSView*)reinterpret_cast<void*>(owned->parent):nil;if(parent.window)[panel beginSheetModalForWindow:parent.window completionHandler:completion];else [panel beginWithCompletionHandler:completion];
    });
    std::unique_lock lock(selection->mutex);bool cancelled=false;
    while(!selection->done){selection->condition.wait_for(lock,std::chrono::milliseconds(40));if(context.cancelled()&&!cancelled){cancelled=true;selection->done=true;selection->result.cancelled=true;dispatch_async(dispatch_get_main_queue(),^{[selection->panel cancel:nil];selection->panel=nil;});}}
    return std::move(selection->result);
}
}
