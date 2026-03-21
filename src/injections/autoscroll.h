#pragma once
#include <string>

namespace Injections {

// Middle-mouse-button autoscroll.  Ultralight doesn't support native
// autoscroll so we polyfill it.  A full-page overlay captures mousemove
// (Ultralight suppresses moves on the underlying page) and we scroll by
// directly manipulating scrollTop/scrollLeft on the nearest scrollable
// ancestor (scrollBy may not be available in Ultralight's WebKit).
inline std::string GetAutoscroll()
{
    return R"AUTOSCROLL(
// ── Middle-click autoscroll ─────────────────────────────────────────
(function(){
var _as=null;

function findScroller(el){
  while(el&&el!==document.body&&el!==document.documentElement){
    var cs=getComputedStyle(el);
    var oy=cs.overflowY,ox=cs.overflowX;
    if((oy==='auto'||oy==='scroll')&&el.scrollHeight>el.clientHeight+1)return el;
    if((ox==='auto'||ox==='scroll')&&el.scrollWidth>el.clientWidth+1)return el;
    el=el.parentElement;
  }
  return document.scrollingElement||document.documentElement;
}

function stop(){
  if(!_as)return;
  if(_as.tid)clearTimeout(_as.tid);
  if(_as.ov&&_as.ov.parentNode)_as.ov.parentNode.removeChild(_as.ov);
  _as=null;
}

function tick(){
  if(!_as)return;
  var dy=_as.cy-_as.oy;
  var dx=_as.cx-_as.ox;
  if(Math.abs(dy)>10){
    var sp=(dy>0?1:-1)*Math.pow((Math.abs(dy)-10)/8,1.6);
    _as.el.scrollTop=_as.el.scrollTop+sp;
  }
  if(Math.abs(dx)>10){
    var hs=(dx>0?1:-1)*Math.pow((Math.abs(dx)-10)/8,1.6);
    _as.el.scrollLeft=_as.el.scrollLeft+hs;
  }
  _as.tid=setTimeout(tick,16);
}

document.addEventListener('mousedown',function(e){
  if(e.button!==1)return;
  e.preventDefault();
  e.stopPropagation();
  if(_as){stop();return;}
  var target=e.target;
  var scroller=findScroller(target);
  // Full-page overlay to capture mousemove reliably
  var ov=document.createElement('div');
  ov.style.cssText='position:fixed;top:0;left:0;width:100vw;height:100vh;'
    +'z-index:2147483647;cursor:default;background:transparent;';
  var ind=document.createElement('div');
  ind.style.cssText='position:fixed;pointer-events:none;width:24px;height:24px;'
    +'margin:-12px 0 0 -12px;border-radius:50%;z-index:2147483647;'
    +'background:rgba(255,255,255,0.15);border:2px solid rgba(156,163,175,0.6);'
    +'box-shadow:0 0 6px rgba(0,0,0,0.4);'
    +'left:'+e.clientX+'px;top:'+e.clientY+'px;';
  var dot=document.createElement('div');
  dot.style.cssText='position:absolute;top:50%;left:50%;width:4px;height:4px;'
    +'margin:-2px 0 0 -2px;border-radius:50%;background:rgba(156,163,175,0.9);';
  ind.appendChild(dot);
  ov.appendChild(ind);
  ov.addEventListener('mousemove',function(me){if(_as){_as.cx=me.clientX;_as.cy=me.clientY;}});
  ov.addEventListener('pointermove',function(me){if(_as){_as.cx=me.clientX;_as.cy=me.clientY;}});
  ov.addEventListener('mousedown',function(me){me.preventDefault();me.stopPropagation();stop();});
  ov.addEventListener('wheel',function(){stop();});
  document.body.appendChild(ov);
  _as={el:scroller,ox:e.clientX,oy:e.clientY,cx:e.clientX,cy:e.clientY,ov:ov,tid:null};
  _as.tid=setTimeout(tick,16);
},true);

document.addEventListener('keydown',function(e){
  if(_as&&e.key==='Escape'){stop();e.preventDefault();}
},true);
// Cancel when the dashboard is hidden (ESC, hotkey, close button all
// dispatch visibilitychange on the iframe document via C++ JS_HIDE).
document.addEventListener('visibilitychange',function(){if(_as)stop();});
})();
)AUTOSCROLL";
}

} // namespace Injections
