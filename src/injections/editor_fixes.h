#pragma once
#include <string>

namespace Injections {

// Styles and scripts for CodeMirror 6 editor selection and keyboard compatibility.
// Injected into <head> before app scripts load.
inline std::string GetEditorFixes()
{
    return R"EDITOR(
<style>
/* ── Editor selection fixes ──────────────────────────────────── */
/* Force text selection on in CM6's contenteditable.
   Ultralight defaults contenteditable to user-select:none which
   breaks drag-to-select entirely. */
.cm-editor,.cm-content,.cm-line{
-webkit-user-select:text!important;user-select:text!important;
cursor:text!important;}

/* Enable text selection in all standard inputs and textareas. */
input,textarea,[contenteditable]{
-webkit-user-select:text!important;user-select:text!important;}

/* Make CM6's custom selection-background divs visible even if the
   engine doesn't render ::selection pseudo-elements. */
.cm-selectionBackground{
background:#3b82f6!important;opacity:0.35!important;
pointer-events:none!important;}
.cm-focused .cm-selectionBackground{
background:#3b82f6!important;opacity:0.45!important;}

/* Dropdowns: Ultralight ignores background overrides on <select> and
   keeps a native light gray. Use black text so it's readable on that bg. */
select{
color:#000000!important;
border:1px solid #4b5563!important;border-radius:4px!important;
padding:2px 4px!important;}
select option{
color:#000000!important;}

/* ── Repaint cost reduction ─────────────────────────────────────────
   Kill CM6 cursor blink animation — continuous CSS animation forces
   Ultralight to repaint the cursor layer every frame even when idle. */
.cm-cursor,.cm-cursorLayer{
animation:none!important;-webkit-animation:none!important;
opacity:1!important;}

</style>
<script>
// ── CM6 MutationObserver throttle ────────────────────────────────────
// CM6 processes every DOM mutation synchronously (observe → reconcile →
// re-render).  In Ultralight this is expensive enough to cause frame
// drops during fast typing.  We batch CM6's MO callbacks to once per
// animation frame — multiple keystrokes within one frame get processed
// as a single update instead of individually.
// ── CM6 MO throttle + input blocking ─────────────────────────────────
// CM6's MO reconciliation is expensive in Ultralight.  We batch MO
// callbacks to once per rAF during typing.  Programmatic changes (like
// switching prompts) pass through immediately so content updates instantly.
// beforeinput/input are blocked so CM6 doesn't process them synchronously.
(function(){
var _OrigMO=window.MutationObserver;
var _typing=false,_typingT=0;

function _inCm(el){
var p=el;while(p){
if(p.nodeType===1&&p.classList&&p.classList.contains('cm-editor'))return true;
p=p.parentElement;}return false;}

// Track typing state — only throttle MO during active typing.
document.addEventListener('keydown',function(e){
if(!e.isTrusted||!_inCm(e.target))return;
_typing=true;clearTimeout(_typingT);
_typingT=setTimeout(function(){_typing=false;},150);
},true);

window.MutationObserver=function(callback){
var _isCM=false;
var _pending=[];
var _rafId=0;
var _wrappedCb=function(mutations){
if(!_isCM){callback(mutations);return;}
if(!_typing){
// Not typing — pass through immediately (prompt switch, etc.)
callback(mutations);return;}
for(var i=0;i<mutations.length;i++)_pending.push(mutations[i]);
if(!_rafId){_rafId=requestAnimationFrame(function(){
_rafId=0;
var batch=_pending;_pending=[];
if(batch.length)callback(batch);});}};
var _obs=new _OrigMO(_wrappedCb);
var _origTR=_obs.takeRecords.bind(_obs);
_obs.takeRecords=function(){
var real=_origTR();
if(!_isCM)return real;
var ours=_pending.splice(0,_pending.length);
return ours.concat(real);};
var _origObs=_obs.observe.bind(_obs);
_obs.observe=function(target,options){
if(!_isCM){var p=target;
while(p){if(p.classList&&p.classList.contains('cm-editor')){
_isCM=true;break;}p=p.parentElement;}}
return _origObs(target,options);};
_obs.disconnect=_obs.disconnect.bind(_obs);
return _obs;};
window.MutationObserver.prototype=_OrigMO.prototype;

document.addEventListener('beforeinput',function(e){
if(_inCm(e.target))e.stopImmediatePropagation();},true);
document.addEventListener('input',function(e){
if(_inCm(e.target))e.stopImmediatePropagation();},true);
})();

// ── CM6 drag-selection relay ──────────────────────────────────────────
// Ultralight suppresses mousemove/pointermove while a button is held AND
// may report e.buttons=0 on move events even when a button is down.
// Two-pronged fix:
//  1. setPointerCapture on pointerdown forces pointermove delivery to the
//     captured element regardless of Ultralight's normal suppression.
//  2. Use a _dragDown boolean flag instead of checking e.buttons, so we
//     don't bail out if the browser incorrectly reports buttons=0.
// On each move event we synthesize a shift+mousedown at the new position;
// CM6 treats that as "extend selection to here" — the same path shift+click uses.
(function(){
var _dragCm=null,_dragDown=false,_dragStarted=false,_lastDragMs=0,_dragStartX=0,_dragStartY=0;
// pointerdown: record cm-content target + force capture so pointermove fires.
document.addEventListener('pointerdown',function(e){
if(e.button!==0||e.shiftKey)return;
_dragDown=true;_dragStarted=false;_dragCm=null;_dragStartX=e.clientX;_dragStartY=e.clientY;
var p=e.target;
while(p){
if(p.className&&typeof p.className==='string'
&&p.className.indexOf('cm-content')>=0){
_dragCm=p;
try{p.setPointerCapture(e.pointerId);}catch(_){}
break;}
p=p.parentElement;}
},true);
// mousedown fallback (if pointerdown doesn't fire).
document.addEventListener('mousedown',function(e){
if(e.button!==0||e.shiftKey||_dragCm)return;
_dragDown=true;_dragStarted=false;_dragStartX=e.clientX;_dragStartY=e.clientY;
var p=e.target;
while(p){
if(p.className&&typeof p.className==='string'
&&p.className.indexOf('cm-content')>=0){_dragCm=p;break;}
p=p.parentElement;}
},true);
var _clear=function(){_dragDown=false;_dragStarted=false;_dragCm=null;};
document.addEventListener('mouseup',_clear,true);
document.addEventListener('pointerup',_clear,true);
document.addEventListener('pointercancel',_clear,true);
function _onDragMove(e){
if(!_dragCm||!_dragDown)return;
if(!_dragStarted){
var dx=e.clientX-_dragStartX,dy=e.clientY-_dragStartY;
if(dx*dx+dy*dy<16)return;
_dragStarted=true;}
var n=Date.now();if(n-_lastDragMs<16)return;_lastDragMs=n;
var cv=_dragCm.cmView;
var view=(cv&&cv.view&&cv.view.posAtCoords)?cv.view
:((cv&&cv.posAtCoords)?cv:self._snpdView);
if(!view||!view.posAtCoords||!view.dispatch)return;
var pos=view.posAtCoords({x:e.clientX,y:e.clientY},false);
if(pos===null||pos===undefined)return;
var anchor=view.state.selection.main.anchor;
view.dispatch({selection:{anchor:anchor,head:pos}});
}
document.addEventListener('mousemove',_onDragMove,{capture:true,passive:true});
document.addEventListener('pointermove',_onDragMove,{capture:true,passive:true});
})();

// ── input/textarea drag-selection relay ───────────────────────────────
// Ultralight suppresses mousemove while a mouse button is held, so
// native drag-to-select inside <input> and <textarea> doesn't work.
// We relay moves as synthesized shift+click events which the browser
// interprets as 'extend selection to here'.
(function(){
var _iEl=null,_iDown=false,_iAnchor=-1,_iSX=0,_iSY=0,_iStarted=false,_iLast=0,_iCvs=null;
document.addEventListener('pointerdown',function(e){
if(e.button!==0||e.shiftKey)return;
var t=e.target;
if(t.tagName!=='INPUT'&&t.tagName!=='TEXTAREA')return;
_iEl=t;_iDown=true;_iStarted=false;_iSX=e.clientX;_iSY=e.clientY;
_iAnchor=t.selectionStart!==undefined?t.selectionStart:0;
try{t.setPointerCapture(e.pointerId);}catch(_){}
},true);
document.addEventListener('mousedown',function(e){
if(e.button!==0||e.shiftKey||_iEl)return;
var t=e.target;
if(t.tagName!=='INPUT'&&t.tagName!=='TEXTAREA')return;
_iEl=t;_iDown=true;_iStarted=false;_iSX=e.clientX;_iSY=e.clientY;
_iAnchor=t.selectionStart!==undefined?t.selectionStart:0;
},true);
var _iClear=function(){_iDown=false;_iEl=null;_iAnchor=-1;_iStarted=false;};
document.addEventListener('mouseup',_iClear,true);
document.addEventListener('pointerup',_iClear,true);
document.addEventListener('pointercancel',_iClear,true);
function _iMove(e){
if(!_iEl||!_iDown)return;
if(!_iStarted){
var dx=e.clientX-_iSX,dy=e.clientY-_iSY;
if(dx*dx+dy*dy<16)return;
_iStarted=true;}
var n=Date.now();if(n-_iLast<16)return;_iLast=n;
var t=_iEl;
if(!_iCvs)_iCvs=document.createElement('canvas');
var pos=-1;
{var r=t.getBoundingClientRect();
var cs=window.getComputedStyle(t);
var pl=parseFloat(cs.paddingLeft)||0;
var fn=cs.font||(cs.fontSize+' '+cs.fontFamily);
var ctx=_iCvs.getContext('2d');ctx.font=fn;
var val=t.value||'';
if(t.tagName==='TEXTAREA'){
var pt=parseFloat(cs.paddingTop)||0;
var lh=parseFloat(cs.lineHeight);if(!lh||lh<=0)lh=parseFloat(cs.fontSize)*1.2||16;
var ty=e.clientY-r.top-pt+(t.scrollTop||0);
var li=Math.max(0,Math.floor(ty/lh));
var lns=val.split('\n');li=Math.min(li,lns.length-1);
var ls=0;for(var j=0;j<li;j++)ls+=lns[j].length+1;
var line=lns[li]||'';
var tx=e.clientX-r.left-pl+(t.scrollLeft||0);
var _blo=0,_bhi=line.length,_bbest=0;
while(_blo<=_bhi){var _bm=(_blo+_bhi)>>1;
if(ctx.measureText(line.slice(0,_bm)).width<=tx){_bbest=_bm;_blo=_bm+1;}else _bhi=_bm-1;}
pos=ls+_bbest;
}else{
var tx=e.clientX-r.left-pl+(t.scrollLeft||0);
var _blo=0,_bhi=val.length,_bbest=0;
while(_blo<=_bhi){var _bm=(_blo+_bhi)>>1;
if(ctx.measureText(val.slice(0,_bm)).width<=tx){_bbest=_bm;_blo=_bm+1;}else _bhi=_bm-1;}
pos=_bbest;
}}
if(pos<0)return;
var anch=_iAnchor>=0?_iAnchor:0;
var lo=Math.min(anch,pos),hi=Math.max(anch,pos);
try{t.setSelectionRange(lo,hi,pos<anch?'backward':'forward');}catch(_){}
}
document.addEventListener('mousemove',_iMove,{capture:true,passive:true});
document.addEventListener('pointermove',_iMove,{capture:true,passive:true});
})();
</script>
)EDITOR";
}

} // namespace Injections
