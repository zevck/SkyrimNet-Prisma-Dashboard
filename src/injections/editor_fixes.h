#pragma once
#include <string>

namespace Injections {

// Styles and scripts for CodeMirror 6 editor selection and keyboard compatibility.
// Injected into <head> before app scripts load.
inline std::string GetEditorFixes()
{
    return R"EDITOR(
<style>
/* ── Editor selection + key-repeat fixes ────────────────────────── */
/* Force text selection on in CM6's contenteditable.
   Ultralight defaults contenteditable to user-select:none which
   breaks drag-to-select entirely. */
.cm-editor,.cm-content,.cm-line{
-webkit-user-select:text!important;user-select:text!important;
cursor:text!important;}

/* Enable text selection in all standard inputs and textareas. */
input,textarea,[contenteditable]{
-webkit-user-select:text!important;user-select:text!important;}

/*Make CM6's custom selection-background divs visible even if the
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

/* ── Native caret while CM6 is bypassed ──────────────────────────── */
.cm-editing .cm-cursorLayer{display:none!important;}
.cm-editing .cm-content{caret-color:#e5e7eb!important;}
/* Hide CM6 selection highlights while bypassed — native selection
   handles visual feedback; stale CM6 divs cause ghost highlights. */
.cm-editing .cm-selectionLayer{display:none!important;}
</style>
<script>
// CM6 full bypass: block all 5 CM6 input paths while editing.
// Native contenteditable handles chars/backspace/delete.
// Enter is handled via execCommand. Sync to CM6 on mouse click
// (so selection maps correctly) and on focusout.
(function(){
var _OrigMO=window.MutationObserver;
var _active=null;
var _syncing=false;
var _keyActive=false;

function _inCmEditor(el){
var p=el;
while(p){
if(p.nodeType===1&&p.classList&&p.classList.contains('cm-editor'))return p;
p=p.parentElement;}
return null;}

function _getView(editor){
var ct=editor.querySelector('.cm-content');
if(!ct)return null;
var cv=ct.cmView;
return (cv&&cv.view&&cv.view.dispatch)?cv.view:self._snpdView;}

function _readDOM(editor){
var ct=editor.querySelector('.cm-content');
if(!ct)return '';
var nodes=ct.childNodes;
if(!nodes.length)return '';
var parts=[];
for(var i=0;i<nodes.length;i++){
var nd=nodes[i];
if(nd.nodeType===1)parts.push(nd.textContent);
else if(nd.nodeType===3&&nd.textContent){
if(parts.length>0)parts[parts.length-1]+=nd.textContent;
else parts.push(nd.textContent);}}
return parts.join(String.fromCharCode(10)).replace(/\u00A0/g,' ');}

function _syncToCM6(editor){
var v=_getView(editor);
if(!v)return;
var domText=_readDOM(editor);
var cmText=v.state.doc.toString();
if(domText===cmText)return;
_syncing=true;
v.dispatch({changes:{from:0,to:cmText.length,insert:domText}});
setTimeout(function(){_syncing=false;},0);}

function _activate(editor){
if(_active===editor)return;
if(_active)_deactivate();
_active=editor;
editor.classList.add('cm-editing');}

function _deactivate(){
if(!_active)return;
var editor=_active;
_active=null;
_syncToCM6(editor);
editor.classList.remove('cm-editing');}

// Block keydown — native contenteditable handles chars/backspace/delete.
// Enter: execCommand since native may not handle it in CM6's DOM.
document.addEventListener('keydown',function(e){
if(!e.isTrusted)return;
if(e.ctrlKey||e.metaKey)return;
var k=e.key;
if(k==='Escape')return;
var editor=_inCmEditor(e.target);
if(!editor)return;
_activate(editor);
_keyActive=true;
if(k==='Enter'){
document.execCommand('insertParagraph',false,null);
e.preventDefault();}
// Arrow keys: force Ultralight to repaint the native caret.
// Without a DOM mutation the software renderer may skip painting it.
if(k==='ArrowLeft'||k==='ArrowRight'||k==='ArrowUp'||k==='ArrowDown'
||k==='Home'||k==='End'){
var ct=editor.querySelector('.cm-content');
if(ct){ct.style.outline='0px solid transparent';
requestAnimationFrame(function(){ct.style.outline='';});}}
e.stopImmediatePropagation();
},true);

document.addEventListener('keyup',function(){
_keyActive=false;
},true);

// Block beforeinput/input from CM6
document.addEventListener('beforeinput',function(e){
if(!_active)return;
if(_inCmEditor(e.target))e.stopImmediatePropagation();
},true);

document.addEventListener('input',function(e){
if(!_active)return;
if(_inCmEditor(e.target))e.stopImmediatePropagation();
},true);

// Block selectionchange always while active — CM6 maps selection
// against its stale doc state and jumps the cursor.
document.addEventListener('selectionchange',function(e){
if(_active)e.stopImmediatePropagation();
},true);

// On click inside active editor: deactivate (sync), let CM6 process
// the click normally to position cursor, then reactivate.
document.addEventListener('mousedown',function(e){
if(!_active)return;
if(!_inCmEditor(e.target))return;
var editor=_active;
_deactivate();
// Reactivate on next keypress — _active is null so click flows
// through to CM6 as normal (cursor positioning, selection, etc).
},true);

// Deactivate on focusout
document.addEventListener('focusout',function(e){
if(!_active)return;
var editor=_active;
setTimeout(function(){
var ae=document.activeElement;
if(!ae||!editor.contains(ae))_deactivate();
},0);
},true);

// MO wrapper: suppress CM6 MO while active or syncing
window.MutationObserver=function(callback){
var _isCM=false;
var _obs;
var _wrappedCb=function(mutations,observer){
if(!_isCM){callback(mutations,observer);return;}
if(!_active&&!_syncing)callback(mutations,observer);};
_obs=new _OrigMO(_wrappedCb);
var _origTR=_obs.takeRecords.bind(_obs);
_obs.takeRecords=function(){
var real=_origTR();
if(!_isCM||(!_active&&!_syncing))return real;
return [];};
var _origObs=_obs.observe.bind(_obs);
_obs.observe=function(target,options){
if(!_isCM){var p=target;
while(p){if(p.classList&&p.classList.contains('cm-editor')){
_isCM=true;break;}p=p.parentElement;}}
return _origObs(target,options);};
_obs.disconnect=_obs.disconnect.bind(_obs);
return _obs;};
window.MutationObserver.prototype=_OrigMO.prototype;
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
// One-shot 4px threshold — only gates the very first movement so that
// a stationary click doesn't fire. Once exceeded, _dragStarted=true and
// all subsequent moves (including back toward the start) fire normally.
if(!_dragStarted){
var dx=e.clientX-_dragStartX,dy=e.clientY-_dragStartY;
if(dx*dx+dy*dy<16)return;
_dragStarted=true;}
var n=Date.now();if(n-_lastDragMs<16)return;_lastDragMs=n;
// Reach the EditorView via _dragCm.cmView.
// _dragCm = .cm-content = view.contentDOM in CM6.
// CM6 sets contentDOM.cmView = DocView; DocView.view = EditorView.
// Property names are never mangled by terser so posAtCoords/dispatch work.
// Fall back to self._snpdView (set by the bundle patch on each doc change).
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
// Canvas-based char-position measurement — accurate for proportional fonts,
// padded inputs, and horizontally scrolled fields.  caretRangeFromPoint is
// not used here because it cannot reach inside form-control shadow DOM.
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
