#pragma once
#include <string>

namespace Injections {

// Drag-to-select text for general content (diary entries, memory text, etc.).
// Skips inputs, textareas, buttons, links, and CodeMirror editors.
inline std::string GetDragSelect()
{
    return R"DRAGSELECT(
// ── Drag-to-select for general text content (diary, memories, etc) ────
(function(){
var _textDragActive=false,_textDragStart=null,_textDragMoved=false;
document.addEventListener('mousedown',function(e){
// Skip if already handled by input/textarea drag handler or if clicking buttons
if(e.button!==0||e.shiftKey)return;
var t=e.target;
// Don't interfere with inputs, textareas, buttons, links, or CodeMirror
if(t.tagName==='INPUT'||t.tagName==='TEXTAREA'||t.tagName==='BUTTON'
||t.tagName==='A'||t.isContentEditable)return;
// Also skip if clicking inside CodeMirror editor
var p=t;while(p){if(p.classList&&p.classList.contains('cm-editor'))return;p=p.parentElement;}
// Use caretRangeFromPoint to find the text position under the cursor
var range=document.caretRangeFromPoint?document.caretRangeFromPoint(e.clientX,e.clientY):
document.caretPositionFromPoint?document.caretPositionFromPoint(e.clientX,e.clientY):null;
if(!range)return;
_textDragActive=true;_textDragMoved=false;
_textDragStart={x:e.clientX,y:e.clientY,range:range};
},true);
document.addEventListener('mousemove',function(e){
if(!_textDragActive||!_textDragStart)return;
// Check if the mouse has moved enough to consider it a drag
var dx=e.clientX-_textDragStart.x,dy=e.clientY-_textDragStart.y;
if(dx*dx+dy*dy>9){_textDragMoved=true;}
if(!_textDragMoved)return;
// Get the current position under the cursor
var endRange=document.caretRangeFromPoint?document.caretRangeFromPoint(e.clientX,e.clientY):
document.caretPositionFromPoint?document.caretPositionFromPoint(e.clientX,e.clientY):null;
if(!endRange)return;
// Create a selection from start to end
var sel=window.getSelection();sel.removeAllRanges();
var newRange=document.createRange();
var startNode=_textDragStart.range.startContainer||_textDragStart.range.offsetNode;
var startOff=_textDragStart.range.startOffset!==undefined?_textDragStart.range.startOffset:_textDragStart.range.offset||0;
var endNode=endRange.startContainer||endRange.offsetNode;
var endOff=endRange.startOffset!==undefined?endRange.startOffset:endRange.offset||0;
if(!startNode||!endNode)return;
try{
// Determine the direction of the selection
var cmp=startNode.compareDocumentPosition?startNode.compareDocumentPosition(endNode):0;
if(cmp&Node.DOCUMENT_POSITION_FOLLOWING||(cmp===0&&endOff>startOff)){
newRange.setStart(startNode,startOff);newRange.setEnd(endNode,endOff);
}else{newRange.setStart(endNode,endOff);newRange.setEnd(startNode,startOff);}
sel.addRange(newRange);
}catch(_){}},true);
// On mouseup, clear selection if it was just a click (not a drag)
var clearTextDrag=function(e){
if(_textDragActive&&!_textDragMoved){
try{window.getSelection().removeAllRanges();}catch(_){}}
_textDragActive=false;_textDragStart=null;_textDragMoved=false;};
document.addEventListener('mouseup',clearTextDrag,true);
document.addEventListener('pointerup',clearTextDrag,true);
document.addEventListener('pointercancel',clearTextDrag,true);
})();
)DRAGSELECT";
}

} // namespace Injections
