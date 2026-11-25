// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <string_view>

namespace cell::web::detail {
inline constexpr std::size_t maxMessageBytes = 64 * 1024;
inline constexpr std::size_t maxHtmlBytes = 8 * 1024 * 1024;
inline constexpr std::string_view offlineContentPolicy = "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; font-src data:; connect-src 'none'; frame-src 'none'; form-action 'none'\">";
inline constexpr std::string_view assetOrigin = "cell-asset://editor/";
inline constexpr std::string_view assetContentPolicy = "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob: cell-asset: https://cell-editor.invalid/assets/; font-src data: cell-asset: https://cell-editor.invalid/assets/; connect-src 'none'; frame-src 'none'; form-action 'none'\">";

inline constexpr std::string_view bridgeScript() noexcept {
    return R"JS((function(){'use strict';const listeners=[];window.cell=Object.freeze({
postMessage:function(value){const json=JSON.stringify(value);if(json.length>65536)throw new Error('Cell message exceeds 64 KiB');if(window.chrome&&window.chrome.webview)window.chrome.webview.postMessage(json);else if(window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.cell)window.webkit.messageHandlers.cell.postMessage(json);else throw new Error('Cell native bridge is unavailable');},
assetUrl:function(name){if(typeof name!=='string'||!/^[A-Za-z0-9._/-]{1,160}$/.test(name)||name.split('/').some(part=>!part||part==='.')||name.includes('..'))throw new TypeError('Invalid editor asset name');return (location.origin==='https://cell-editor.invalid'?location.origin+'/assets/':'cell-asset://editor/')+name;},
onMessage:function(fn){if(typeof fn!=='function')throw new TypeError('Expected function');listeners.push(fn);return function(){const n=listeners.indexOf(fn);if(n>=0)listeners.splice(n,1);};}});
Object.defineProperty(window,'__cellReceive',{value:function(value){listeners.slice().forEach(function(fn){fn(value);});},writable:false,configurable:false});})();)JS";
}
} // namespace cell::web::detail
