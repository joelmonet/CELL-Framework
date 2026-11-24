#pragma once
#include <string_view>
inline constexpr std::string_view cellWebExampleHtml=R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
body{margin:40px;background:#14272d;color:#e4eeee;font:18px system-ui}
main{max-width:600px}h1{font-size:28px}input{width:100%;accent-color:#f4c45b}
output{font:28px monospace}small{color:#b0cbcf}button{padding:10px;margin-top:15px}
</style></head><body><main><h1>Cell native engine / web editor</h1>
<p>GAIN <output id="value">0.00 dB</output></p>
<input id="gain" type="range" min="-60" max="12" step="0.01" value="0">
<p><small>Native C++ processing and parameter gestures. No remote assets.</small></p>
<button id="reset">Reset</button><p id="status">Connecting…</p>
<script>
const gain=document.getElementById('gain'), value=document.getElementById('value');
let active=false;
function send(type,amount){const m={type:type,key:'gain'};if(amount!==undefined)m.value=amount;cell.postMessage(m);}
function begin(){if(!active){send('begin');active=true;}}
function end(){if(active){send('end');active=false;}}
gain.addEventListener('pointerdown',begin);gain.addEventListener('input',()=>{begin();send('parameter',Number(gain.value));value.textContent=Number(gain.value).toFixed(2)+' dB';});
window.addEventListener('pointerup',end);window.addEventListener('pointercancel',end);window.addEventListener('blur',end);
gain.addEventListener('keyup',end);gain.addEventListener('change',end);
document.getElementById('reset').onclick=()=>{end();send('parameter',0);};
cell.onMessage(message=>{if(message.type!=='parameters')return;const p=message.parameters.find(p=>p.key==='gain');if(!p)return;if(!active)gain.value=p.value;value.textContent=Number(p.value).toFixed(2)+' '+p.unit;document.getElementById('status').textContent='Native engine connected';});
</script></main></body></html>)HTML";
