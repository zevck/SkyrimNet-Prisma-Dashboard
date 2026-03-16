#pragma once
#include <string>

namespace Injections {

// Clipboard integration: C++ bridge functions for GetAsyncKeyState poller.
// Provides __snpdCopy, __snpdCut, __snpdPaste for direct C++ invocation.
inline std::string GetClipboardIntegration()
{
    return R"CLIPBOARD(
// ── Clipboard bridge functions (invoked by C++ GetAsyncKeyState poller) ─
// __snpdCopy() — returns selection text from the focused element.
// __snpdCut()  — same but also deletes the selection; returns the text.
// __snpdPaste(txt) — inserts text at cursor with React-safe prototype setter.
// No JS keyboard events or sync XHR involved; all Ctrl detection is in C++.
window.__snpdCopy=function(){
var t=document.activeElement,s='';
if(t&&(t.tagName==='INPUT'||t.tagName==='TEXTAREA')){
s=t.value.slice(t.selectionStart,t.selectionEnd);
}else if(window.getSelection){s=window.getSelection().toString();}
return s;};
window.__snpdCut=function(){
var t=document.activeElement,s='';
if(t&&(t.tagName==='INPUT'||t.tagName==='TEXTAREA')){
var ss=t.selectionStart,se=t.selectionEnd;
s=t.value.slice(ss,se);
if(s){
var P=t.tagName==='INPUT'?HTMLInputElement.prototype:HTMLTextAreaElement.prototype;
var d=Object.getOwnPropertyDescriptor(P,'value');
var nv=t.value.slice(0,ss)+t.value.slice(se);
if(d&&d.set)d.set.call(t,nv);else t.value=nv;
t.selectionStart=t.selectionEnd=ss;
try{t.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'deleteByCut'}));}catch(_x){t.dispatchEvent(new Event('input',{bubbles:true}));}
t.dispatchEvent(new Event('change',{bubbles:true}));}
}else if(window.getSelection){
s=window.getSelection().toString();
try{document.execCommand('delete');}catch(_x){}}
return s;};
window.__snpdPaste=function(txt){
if(!txt)return;
var el=document.activeElement;
if(el&&(el.tagName==='INPUT'||el.tagName==='TEXTAREA')){
var P=el.tagName==='INPUT'?HTMLInputElement.prototype:HTMLTextAreaElement.prototype;
var d=Object.getOwnPropertyDescriptor(P,'value');
var ss=el.selectionStart,se=el.selectionEnd,sv=el.value;
var nv=sv.slice(0,ss)+txt+sv.slice(se);
if(d&&d.set)d.set.call(el,nv);else el.value=nv;
el.selectionStart=el.selectionEnd=ss+txt.length;
try{el.dispatchEvent(new InputEvent('input',{bubbles:true,data:txt,inputType:'insertText'}));}catch(_x){el.dispatchEvent(new Event('input',{bubbles:true}));}
el.dispatchEvent(new Event('change',{bubbles:true}));
}else{try{document.execCommand('insertText',false,txt);}catch(_x){}}};
// Swallow Ctrl+C/X/V keydowns so Ultralight doesn't play its native ding.
// The C++ GetAsyncKeyState poller handles the actual clipboard work independently.
(function(){
document.addEventListener('keydown',function(e){
var kc=e.keyCode||e.which||0,k=e.key?e.key.toLowerCase():'';
if(!e.ctrlKey&&!e.metaKey)return;
if(kc===67||k==='c'||kc===86||k==='v'||kc===88||k==='x'){
e.preventDefault();e.stopPropagation();}},true);
})();
// DOM copy/paste events for context-menu clipboard (async fetch, no sync XHR)
(function(){
document.addEventListener('copy',function(e){e.preventDefault();
var s=window.__snpdCopy?window.__snpdCopy():'';
if(s)fetch('/clipboard-set',{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body:s}).catch(function(){});},true);
document.addEventListener('paste',function(e){e.preventDefault();
var cd=e.clipboardData&&e.clipboardData.getData?e.clipboardData.getData('text/plain'):'';
if(cd){if(window.__snpdPaste)window.__snpdPaste(cd);}
else{fetch('/clipboard-get').then(function(r){return r.text();}).then(function(t){if(t&&window.__snpdPaste)window.__snpdPaste(t);}).catch(function(){});}
},true);
// navigator.clipboard async polyfill for apps that call it directly
try{Object.defineProperty(navigator,'clipboard',{configurable:true,value:{
writeText:function(t){return fetch('/clipboard-set',{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body:t||''}).then(function(){});}  ,
readText:function(){return fetch('/clipboard-get').then(function(r){return r.text();});}
}});}catch(_x){}
})();
// ── Dialog / nav compat ───────────────────────────────────────────────
window.confirm=function(){return true;};
window.alert=function(){};
window.prompt=function(m,d){return d!==undefined?d:'';};
window.open=function(url){if(url)window.location.href=url;return window;};
// ── Custom prompt modal ───────────────────────────────────────────────
// Ultralight has no native window.prompt. Provide _snpdShowPrompt(msg, def)
// which renders a modal overlay with a text input. On OK/Enter calls
// self._snpdPromptCb(value); on Cancel calls self._snpdPromptCb(null).
(function(){
var _ov=null;
self._snpdShowPrompt=function(msg,def){
if(_ov)_ov.remove();
_ov=document.createElement('div');
_ov.style.cssText='position:fixed;inset:0;z-index:2147483647;display:flex;align-items:center;justify-content:center;background:rgba(0,0,0,.6)';
var box=document.createElement('div');
box.style.cssText='background:#1f2937;border:1px solid #374151;border-radius:8px;padding:24px;min-width:340px;max-width:480px;box-shadow:0 20px 60px rgba(0,0,0,.8);display:flex;flex-direction:column;gap:12px';
var lbl=document.createElement('div');
lbl.style.cssText='color:#f9fafb;font-size:14px;font-family:Consolas,monospace';
lbl.textContent=msg||'';
var inp=document.createElement('input');
inp.type='text';inp.value=def||'';
inp.style.cssText='background:#111827;border:1px solid #4b5563;border-radius:4px;color:#f9fafb;font-size:14px;font-family:Consolas,monospace;padding:8px 10px;outline:none;width:100%;box-sizing:border-box';
var row=document.createElement('div');
row.style.cssText='display:flex;gap:8px;justify-content:flex-end';
var ok=document.createElement('button');
ok.textContent='OK';
ok.style.cssText='padding:6px 18px;background:#3b82f6;border:none;border-radius:4px;color:#fff;font-size:13px;cursor:pointer';
var cancel=document.createElement('button');
cancel.textContent='Cancel';
cancel.style.cssText='padding:6px 18px;background:#374151;border:none;border-radius:4px;color:#fff;font-size:13px;cursor:pointer';
function _ok(){var v=inp.value.trim();_ov.remove();_ov=null;if(self._snpdPromptCb)self._snpdPromptCb(v||null);}
function _cancel(){_ov.remove();_ov=null;if(self._snpdPromptCb)self._snpdPromptCb(null);}
ok.addEventListener('click',_ok);
cancel.addEventListener('click',_cancel);
inp.addEventListener('keydown',function(e){
if(e.key==='Enter'){e.preventDefault();_ok();}
else if(e.key==='Escape'){e.preventDefault();_cancel();}
e.stopPropagation();});
row.appendChild(cancel);row.appendChild(ok);
box.appendChild(lbl);box.appendChild(inp);box.appendChild(row);
_ov.appendChild(box);document.body.appendChild(_ov);
setTimeout(function(){inp.focus();inp.select();},50);
};
})();
)CLIPBOARD";
}

} // namespace Injections
