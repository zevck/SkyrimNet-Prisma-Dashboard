#pragma once
#include <string>

namespace Injections {

// Audio polyfill for Ultralight (which has no HTMLMediaElement/Web Audio support).
// Provides fake Audio constructor and AudioContext that route playback to C++ via fetch().
inline std::string GetAudioPolyfill()
{
    return R"AUDIO(
// ── Audio bridge ─────────────────────────────────────────────────────
// Ultralight has no HTMLMediaElement/Web Audio/AudioContext support.
// We provide a complete fake Audio + AudioContext that:
//   - Has a working addEventListener/removeEventListener/dispatchEvent
//   - Fires canplaythrough+loadedmetadata etc. 30ms after src is set so
//     React components that wait for "ready" events unblock immediately
//   - C++ proxy caches audio bytes and returns stub+ID header; play() is tiny
(function(){
// Unconditionally override URL.createObjectURL/revokeObjectURL.
// Ultralight may define these natively (WebKit heritage) but the Blob URL
// scheme is not supported — calling the native version crashes the renderer.
// We must override regardless of whether the native version exists.
URL.createObjectURL=function(){return '';};
URL.revokeObjectURL=function(){};
// Active Audio instances waiting for completion.
var _paActive=[];
// Called from C++ via Invoke() when PlaySound finishes.
window.__onAudioEnded=function(){
var a=_paActive.splice(0,_paActive.length);
for(var i=0;i<a.length;i++){
try{a[i].paused=true;a[i].ended=true;}catch(e){}
try{_fireOn(a[i],'ended');}catch(e){}
}
try{document.dispatchEvent(new Event('snpd:audioended'));}catch(e){}
};
// Called from C++ when test-TTS audio finishes (separate from diary TTS).
window.__onTestTtsEnded=function(){
try{document.dispatchEvent(new Event('snpd:testttsended'));}catch(e){}
};
// _pa: dispatch play/pause/stop to local proxy
var _pa=function(action,src){
try{
var r=src||'';
// For 'play' with empty src: treat as resume (not a new play request).
// URL.createObjectURL returns '' for unsupported blobs; resolving '' against
// the page base would give the page's own URL, causing C++ to fetch HTML as
// audio and bump s_audioGen — killing any in-progress diary playback.
// For 'pause'/'stop': always send regardless of src so C++ can stop playback.
if(action==='play'&&!r){
fetch('/audio',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({action:'resume',src:''})}).catch(function(){});
return;
}
try{if(r)r=(new URL(r,window.location.href)).href;}catch(e){}
if(action==='play'&&!r)return; // paranoia: still empty after resolution
if(action==='play'&&r.indexOf('blob:')===0){
// blob: URL — fetch bytes then POST raw to /audio-raw for C++ playback
fetch(r).then(function(res){
var ct=res.headers.get('Content-Type')||'audio/mpeg';
return res.arrayBuffer().then(function(ab){
return fetch('/audio-raw',{method:'POST',headers:{'Content-Type':ct},body:ab});});
}).catch(function(){});
}else{
fetch('/audio',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({action:action,src:r})}).catch(function(){});
}
}catch(e){}
};
// _fireOn: call on* callback + any addEventListener listeners for an event
function _fireOn(obj,name){
var e=new Event(name);
var cb=obj['on'+name];
if(typeof cb==='function'){try{cb.call(obj,e);}catch(_){}}
var evts=obj._evts&&obj._evts[name];
if(evts){evts.slice().forEach(function(fn){try{fn.call(obj,e);}catch(_){}});}
}
// _scheduleFakeLoad: simulate media load completion so apps waiting for
// canplaythrough / readyState==4 unblock ~30ms after src is assigned
function _scheduleFakeLoad(audio){
setTimeout(function(){
audio.readyState=4;        // HAVE_ENOUGH_DATA
audio.duration=Infinity;   // streaming / unknown length is fine
['loadstart','loadedmetadata','loadeddata','canplay','canplaythrough']
.forEach(function(n){_fireOn(audio,n);});
},30);}
// Fake Audio constructor
window.Audio=function AB(src){
this._src='';
this._evts={};
this.currentTime=0;this.volume=1;this.muted=false;
this.paused=true;this.ended=false;this.loop=false;
this.readyState=0;this.duration=0;
this.onended=null;this.onerror=null;this.oncanplaythrough=null;
this.onloadedmetadata=null;this.oncanplay=null;this.onloadstart=null;
this.onplay=null;this.onpause=null;
if(src){this._src=src;_scheduleFakeLoad(this);}
};
// HTMLMediaElement constants
window.Audio.HAVE_NOTHING=0;
window.Audio.HAVE_METADATA=1;
window.Audio.HAVE_CURRENT_DATA=2;
window.Audio.HAVE_FUTURE_DATA=3;
window.Audio.HAVE_ENOUGH_DATA=4;
window.Audio.NETWORK_EMPTY=0;
window.Audio.NETWORK_IDLE=1;
window.Audio.NETWORK_LOADING=2;
window.Audio.NETWORK_NO_SOURCE=3;
window.Audio.prototype.HAVE_NOTHING=0;
window.Audio.prototype.HAVE_METADATA=1;
window.Audio.prototype.HAVE_CURRENT_DATA=2;
window.Audio.prototype.HAVE_FUTURE_DATA=3;
window.Audio.prototype.HAVE_ENOUGH_DATA=4;
window.Audio.prototype.NETWORK_EMPTY=0;
window.Audio.prototype.NETWORK_IDLE=1;
window.Audio.prototype.NETWORK_LOADING=2;
window.Audio.prototype.NETWORK_NO_SOURCE=3;
Object.defineProperty(window.Audio.prototype,'src',{
get:function(){return this._src;},
// Pre-fetch blob data immediately when src is set so the bytes are already
// in flight (or done) by the time play() is called — eliminates the extra
// round-trip delay between the user pressing play and audio starting.
set:function(v){
this._src=v;
if(v){_scheduleFakeLoad(this);}
}});
Object.defineProperty(window.Audio.prototype,'currentSrc',{
get:function(){return this._src||'';}});
window.Audio.prototype.addEventListener=function(name,fn){
if(!name||!fn)return;
if(!this._evts[name])this._evts[name]=[];
this._evts[name].push(fn);};
window.Audio.prototype.removeEventListener=function(name,fn){
if(!name||!this._evts[name])return;
this._evts[name]=this._evts[name].filter(function(f){return f!==fn;});};
window.Audio.prototype.dispatchEvent=function(e){_fireOn(this,e.type);return true;};
window.Audio.prototype.canPlayType=function(type){
// Return 'probably' for common audio types to satisfy feature detection
if(!type)return '';
var t=type.toLowerCase();
if(t.indexOf('audio/mpeg')!==-1||t.indexOf('audio/mp3')!==-1)return 'probably';
if(t.indexOf('audio/wav')!==-1||t.indexOf('audio/wave')!==-1)return 'probably';
if(t.indexOf('audio/ogg')!==-1)return 'probably';
if(t.indexOf('audio/webm')!==-1)return 'maybe';
return '';};
window.Audio.prototype.play=function(){
_paActive.push(this);
this.paused=false;this.ended=false;
_fireOn(this,'play');
_pa('play',this._src);
// Return a real Promise so await/then callers get proper async behaviour.
return(typeof Promise!=='undefined')?Promise.resolve():
{then:function(fn){if(fn)setTimeout(fn,0);return this;},catch:function(){return this;}};
};
window.Audio.prototype.pause=function(){
this.paused=true;
// Remove from _paActive so that a subsequent play() (resume) doesn't
// double-push this audio and cause __onAudioEnded to fire twice, which
// would advance the segment queue by 2 on each ended event.
var _pi=_paActive.indexOf(this);if(_pi!==-1)_paActive.splice(_pi,1);
_fireOn(this,'pause');
_pa('pause',this._src);
};
window.Audio.prototype.load=function(){_scheduleFakeLoad(this);};
// Basic AudioContext stub so apps don't break when probing for Web Audio support
if(typeof window.AudioContext==='undefined'){
window.AudioContext=function(){
this.state='suspended';
this.sampleRate=44100;
this.currentTime=0;
this.destination={};
};
window.AudioContext.prototype.createMediaElementSource=function(){return{};};
window.AudioContext.prototype.createGain=function(){return{gain:{value:1},connect:function(){}};};
window.AudioContext.prototype.createPanner=function(){return{connect:function(){}};};
window.AudioContext.prototype.resume=function(){return Promise.resolve();};
window.AudioContext.prototype.suspend=function(){return Promise.resolve();};
window.AudioContext.prototype.close=function(){return Promise.resolve();};
}
// Intercept document.createElement('audio') — some React audio libs
// bypass 'new Audio()' entirely and create elements this way
var _origCreate=document.createElement.bind(document);
document.createElement=function(tag){
if(typeof tag==='string'&&tag.toLowerCase()==='audio')return new window.Audio();
return _origCreate(tag);
};
// Patch HTMLAudioElement prototype if the engine exposes it
if(typeof HTMLAudioElement!=='undefined'){
try{
HTMLAudioElement.prototype.play=function(){
_pa('play',this.src||this.currentSrc||'');
return{then:function(fn){if(fn)fn();return this;},catch:function(){return this;}};
};
HTMLAudioElement.prototype.pause=function(){
_pa('pause',this.src||this.currentSrc||'');
};
}catch(e){}}
})();
)AUDIO";
}

} // namespace Injections
