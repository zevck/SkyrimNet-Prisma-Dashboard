#pragma once
#include <string>

namespace Injections {

// Returns the HTML for the persistent shell window chrome.
// The iframe inside loads from /proxy (same origin) so contentWindow patching works.
inline std::string GetShellHtml()
{
    return R"SHELL(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{width:100%;height:100%;background:transparent;overflow:hidden}
#W{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);width:2000px;max-width:95vw;height:1200px;max-height:95vh;display:flex;flex-direction:column;background:#111827;border:2px solid #444;border-radius:8px;box-shadow:0 0 30px rgba(0,0,0,.8);z-index:99999}
#B{display:flex;align-items:center;justify-content:space-between;padding:8px 16px;background:#1f2937;border-bottom:1px solid #374151;border-radius:8px 8px 0 0;user-select:none;-webkit-user-select:none;cursor:grab}
#L{display:flex;align-items:center;gap:8px;color:#9ca3af;font-size:14px;font-family:Consolas,"Courier New",monospace;font-weight:600;pointer-events:none}
#R{display:flex;align-items:center;gap:8px}
.btn{display:flex;align-items:center;gap:4px;padding:4px 12px;background:#374151;border:none;border-radius:4px;color:#fff;font-size:12px;font-family:Consolas,"Courier New",monospace;cursor:pointer;pointer-events:auto}
.btn.icon{padding:4px 7px}
.btn:hover{background:#4b5563}
#XB{display:flex;align-items:center;padding:4px 8px;background:#dc2626;border:none;border-radius:4px;color:#fff;cursor:pointer}
#XB:hover{background:#ef4444}
#C{flex:1;overflow:hidden;min-height:0;border-radius:0 0 6px 6px;position:relative}
iframe{width:100%;height:100%;border:none;display:block}
#OL{display:none;position:absolute;inset:0;z-index:1;cursor:grabbing}
.rh{position:absolute;z-index:200}
.rh[data-r=n]{top:-5px;left:12px;right:12px;height:6px;cursor:n-resize}
.rh[data-r=s]{bottom:-5px;left:12px;right:12px;height:10px;cursor:s-resize}
.rh[data-r=e]{right:-5px;top:12px;bottom:12px;width:10px;cursor:e-resize}
.rh[data-r=w]{left:-5px;top:12px;bottom:12px;width:10px;cursor:w-resize}
.rh[data-r=nw]{top:-5px;left:-5px;width:10px;height:10px;cursor:nw-resize}
.rh[data-r=ne]{top:-5px;right:-5px;width:10px;height:10px;cursor:ne-resize}
.rh[data-r=se]{bottom:-5px;right:-5px;width:18px;height:18px;cursor:se-resize}
.rh[data-r=sw]{bottom:-5px;left:-5px;width:14px;height:14px;cursor:sw-resize}
#W.fs .rh{display:none}
/* Persistent faint L-brackets in each corner */
.rh[data-r=se]::before,.rh[data-r=sw]::before,.rh[data-r=ne]::before,.rh[data-r=nw]::before{content:'';position:absolute;width:7px;height:7px}
.rh[data-r=se]::before{bottom:3px;right:3px;border-right:2px solid rgba(156,163,175,.35);border-bottom:2px solid rgba(156,163,175,.35)}
.rh[data-r=sw]::before{bottom:3px;left:3px;border-left:2px solid rgba(156,163,175,.35);border-bottom:2px solid rgba(156,163,175,.35)}
.rh[data-r=ne]::before{top:3px;right:3px;border-right:2px solid rgba(156,163,175,.35);border-top:2px solid rgba(156,163,175,.35)}
.rh[data-r=nw]::before{top:3px;left:3px;border-left:2px solid rgba(156,163,175,.35);border-top:2px solid rgba(156,163,175,.35)}
/* Hover: semi-transparent fill + bright indicators */
.rh:hover{background:rgba(99,102,241,.15)}
.rh[data-r=se]:hover::before,.rh[data-r=sw]:hover::before,.rh[data-r=ne]:hover::before,.rh[data-r=nw]:hover::before{border-color:rgba(99,102,241,.95)}
.rh[data-r=n]:hover::after,.rh[data-r=s]:hover::after{content:'';position:absolute;left:calc(50% - 18px);top:calc(50% - 1.5px);width:36px;height:3px;border-radius:2px;background:rgba(99,102,241,.85)}
.rh[data-r=e]:hover::after,.rh[data-r=w]:hover::after{content:'';position:absolute;top:calc(50% - 18px);left:calc(50% - 1.5px);height:36px;width:3px;border-radius:2px;background:rgba(99,102,241,.85)}
</style>
</head>
<body>
<div id="W">
  <div class="rh" data-r="n"></div>
  <div class="rh" data-r="s"></div>
  <div class="rh" data-r="e"></div>
  <div class="rh" data-r="w"></div>
  <div class="rh" data-r="nw"></div>
  <div class="rh" data-r="ne"></div>
  <div class="rh" data-r="se"></div>
  <div class="rh" data-r="sw"></div>
  <div id="B">
    <div id="L">
      <span>🎮</span>
      <span>Skyrim Dashboard</span>
    </div>
    <div id="R">
      <button id="RB" class="btn" title="Reload (F5)">↻</button>
      <button id="HB" class="btn" title="Home">🏠</button>
      <button id="SB" class="btn icon" title="Settings">⚙</button>
      <span id="ZL" class="btn" style="min-width:42px;justify-content:center;cursor:default;">100%</span>
      <button id="FB" class="btn icon" title="Toggle Fullscreen">⛶</button>
      <button id="XB" title="Close (F4)">✕</button>
    </div>
  </div>
  <div id="C"><div id="OL"></div><iframe id="snpd-frame" src="/proxy"></iframe></div>
</div>
)SHELL";
}

} // namespace Injections
