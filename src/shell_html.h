#pragma once
// ── Embedded HTTP shell server ────────────────────────────────────────────────
// Serves a self-contained chrome page (topbar, border, drag) with the SkyrimNet
// dashboard inside an <iframe>.  The outer shell NEVER navigates so Ultralight
// never tears down and rebuilds the chrome — no flash, no hide/show hacks needed.

static std::string BuildShellHtml()
{
    // Raw HTML+CSS+JS for the persistent window chrome.
    // The iframe loads from /proxy (same origin) so contentWindow patching works.
    std::string html = R"SHELL(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{width:100%;height:100%;background:transparent;overflow:hidden}
#W{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);width:2000px;max-width:95vw;height:1200px;max-height:95vh;display:flex;flex-direction:column;background:#111827;border:2px solid #555;border-radius:8px;z-index:99999}
#B{display:flex;align-items:center;justify-content:space-between;padding:calc(8px * var(--ui-s)) calc(16px * var(--ui-s));background:#1f2937;border-bottom:1px solid #374151;border-radius:8px 8px 0 0;user-select:none;-webkit-user-select:none;cursor:grab;font-size:calc(14px * var(--ui-s))}
#L{display:flex;align-items:center;gap:calc(8px * var(--ui-s));color:#9ca3af;font-size:calc(14px * var(--ui-s));font-family:Consolas,"Courier New",monospace;font-weight:600;pointer-events:none}
#R{display:flex;align-items:center;gap:calc(8px * var(--ui-s))}
.btn{display:flex;align-items:center;gap:4px;padding:calc(4px * var(--ui-s)) calc(12px * var(--ui-s));background:#374151;border:none;border-radius:calc(4px * var(--ui-s));color:#fff;font-size:calc(12px * var(--ui-s));font-family:Consolas,"Courier New",monospace;cursor:pointer;pointer-events:auto}
.btn.icon{padding:calc(4px * var(--ui-s)) calc(7px * var(--ui-s))}
.btn:hover{background:#4b5563}
#KBB.active{background:#b45309}#KBB.active:hover{background:#d97706}
#KBB:not(.active){opacity:0.55}#KBB:not(.active):hover{opacity:1}
.btn:disabled{opacity:.35;cursor:default}
#XB{display:flex;align-items:center;padding:calc(4px * var(--ui-s)) calc(8px * var(--ui-s));background:#dc2626;border:none;border-radius:calc(4px * var(--ui-s));color:#fff;cursor:pointer}
.btn svg{width:calc(14px * var(--ui-s));height:calc(14px * var(--ui-s))}
#XB svg{width:calc(14px * var(--ui-s));height:calc(14px * var(--ui-s))}
#XB:hover{background:#ef4444}
#C{flex:1;overflow:hidden;min-height:0;border-radius:0 0 6px 6px;position:relative}
iframe{width:100%;height:100%;border:none;display:block}
#OL{display:none;position:absolute;inset:0;z-index:1;cursor:grabbing}
.rh{position:absolute;z-index:200}
.rh[data-r=n]{top:-5px;left:12px;right:12px;height:6px;cursor:n-resize}
.rh[data-r=s]{bottom:-5px;left:12px;right:12px;height:10px;cursor:s-resize}
.rh[data-r=e]{right:-8px;top:12px;bottom:12px;width:8px;cursor:e-resize}
.rh[data-r=w]{left:-5px;top:12px;bottom:12px;width:10px;cursor:w-resize}
.rh[data-r=nw]{top:-5px;left:-5px;width:10px;height:10px;cursor:nw-resize}
.rh[data-r=ne]{top:-5px;right:-5px;width:10px;height:10px;cursor:ne-resize}
.rh[data-r=se]{bottom:-5px;right:-5px;width:18px;height:18px;cursor:se-resize}
.rh[data-r=sw]{bottom:-5px;left:-5px;width:14px;height:14px;cursor:sw-resize}
#W.fs .rh{display:none}
#SNFI{-webkit-user-select:text!important;user-select:text!important;}
:root{--ui-s:1}
#ZL{font-size:calc(11px * var(--ui-s));min-width:calc(38px * var(--ui-s))}
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
.rh[data-r=e]:hover::after,.rh[data-r=w]:hover::after{content:'';position:absolute;top:calc(50% - 18px);left:calc(50% - 1.5px);width:3px;height:36px;border-radius:2px;background:rgba(99,102,241,.85)}
</style>
</head>
<body>
<div id="W">
  <div class="rh" data-r="n"></div><div class="rh" data-r="s"></div>
  <div class="rh" data-r="e"></div><div class="rh" data-r="w"></div>
  <div class="rh" data-r="nw"></div><div class="rh" data-r="ne"></div>
  <div class="rh" data-r="se"></div><div class="rh" data-r="sw"></div>
  <div id="B">
    <div id="L">
      <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="5 9 2 12 5 15"/><polyline points="9 5 12 2 15 5"/><polyline points="15 19 12 22 9 19"/><polyline points="19 9 22 12 19 15"/><line x1="2" y1="12" x2="22" y2="12"/><line x1="12" y1="2" x2="12" y2="22"/></svg>
      <span>SkyrimNet</span>
      <button class="btn icon" id="BB" title="Back" disabled><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="15 18 9 12 15 6"/></svg></button>
      <button class="btn icon" id="FWB" title="Forward" disabled><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg></button>
      <button class="btn icon" id="HB" title="Home"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/></svg></button>
      <button class="btn icon" id="RB" title="Refresh"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg></button>
    </div>
    <div id="R">
      <button class="btn" id="ZL" title="Reset zoom">100%</button>
      <button class="btn icon" id="KBB" title="Keep Background (off)"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/></svg></button>
      <button class="btn icon" id="SB" title="Settings"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg></button>
      <button class="btn" id="FB"></button>
      <button id="XB"><svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg></button>
    </div>
  </div>
  <div id="C"><div id="OL"></div>
    <div id="SNFB" style="display:none;position:absolute;bottom:8px;left:16px;z-index:500;background:#1f2937;border:1px solid #374151;border-radius:6px;padding:6px 10px;align-items:center;gap:6px;box-shadow:0 4px 12px #0008;">
      <input id="SNFI" type="text" placeholder="Find..." style="background:#111827;border:1px solid #374151;border-radius:4px;padding:4px 8px;color:#e5e7eb;font-family:Consolas,monospace;font-size:calc(12px * var(--ui-s));outline:none;width:calc(200px * var(--ui-s));">
      <button class="btn" id="SNFP" title="Previous" style="padding:2px 6px;">&#9650;</button>
      <button class="btn" id="SNFN" title="Next" style="padding:2px 6px;">&#9660;</button>
      <button class="btn" id="SNFX" title="Close" style="padding:2px 6px;">&#10005;</button>
    </div>
    <iframe id="snpd-frame" src="/proxy"></iframe>
  </div>
</div>
)SHELL";
    // Seed localStorage from values saved to INI so applyZoom()/applyFs() see
    // the correct initial state on every game launch (Ultralight localStorage
    // is in-memory only and doesn't survive between sessions on its own).
    {
        std::lock_guard<std::mutex> lk(s_cfgMtx);
        auto escQ = [](const std::string& s) {
            std::string r; for (auto c : s) { if (c=='\''||c=='\\') r+='\\'; r+=c; } return r; };
        html += "<script>(function(){var sl={x:'";
        html += escQ(s_cfg.winX)  + "',y:'";
        html += escQ(s_cfg.winY)  + "',w:'";
        html += escQ(s_cfg.winW)  + "',h:'";
        html += escQ(s_cfg.winH)  + "',zoom:'";
        html += escQ(s_cfg.winZoom);
        html += std::string("',fs:'") + (s_cfg.winFs ? "true" : "false") + "',keepBg:" + (s_cfg.keepBg ? "true" : "false") + "};";

        html += "if(sl.x)localStorage.setItem('snpd-x',sl.x);"
                "if(sl.y)localStorage.setItem('snpd-y',sl.y);"
                "if(sl.w)localStorage.setItem('snpd-w',sl.w);"
                "if(sl.h)localStorage.setItem('snpd-h',sl.h);"
                "if(sl.zoom)localStorage.setItem('snpd-zoom',sl.zoom);"
                "localStorage.setItem('snpd-fs',sl.fs);"
                "window.snpdInitKeepBg=sl.keepBg;"
                "})();</script>";

        // Restore persisted localStorage before iframe loads (same origin = shared localStorage)
        std::string storageJson = ReadStorage();
        if (storageJson.size() > 2) {
            std::string b64 = Base64Encode(storageJson);
            html += "<script>try{var d=JSON.parse(atob('";
            html += b64;
            html += "'));var k=Object.keys(d);for(var i=0;i<k.length;i++)localStorage.setItem(k[i],d[k[i]]);}catch(e){}</script>";
            logger::info("SkyrimNetDashboard: shell restore script injected ({} keys from {} bytes)", "?", storageJson.size());
        }
    }
    html +=
    R"SHELL2(
<script>
(function(){
  var W=document.getElementById('W'),
      B=document.getElementById('B'),
      FB=document.getElementById('FB'),
      XB=document.getElementById('XB'),
      HB=document.getElementById('HB'),
      RB=document.getElementById('RB'),
      SB=document.getElementById('SB'),
      ZL=document.getElementById('ZL'),
      BB=document.getElementById('BB'),
      FWB=document.getElementById('FWB'),
      KBB=document.getElementById('KBB'),
      OL=document.getElementById('OL'),
      FR=document.getElementById('snpd-frame');
  var fs=localStorage.getItem('snpd-fs')==='true';
  var zoom=parseFloat(localStorage.getItem('snpd-zoom')||'1');
  if(isNaN(zoom)||zoom<0.20||zoom>3)zoom=1;
  // Auto-scale UI for resolution (baseline 1440p)
  var uiScale=Math.max(1,Math.min(2.5,window.innerHeight/1080));
  document.documentElement.style.setProperty('--ui-s',uiScale);
  var _snpdLayoutTimer=null;
  function snpdSaveLayout(){var p={x:localStorage.getItem('snpd-x')||'',y:localStorage.getItem('snpd-y')||'',w:localStorage.getItem('snpd-w')||'',h:localStorage.getItem('snpd-h')||'',zoom:localStorage.getItem('snpd-zoom')||'1',fs:localStorage.getItem('snpd-fs')||'false'};fetch('/snpd-save-layout',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)}).catch(function(){});}
  function snpdSaveLayoutDebounced(){if(_snpdLayoutTimer)clearTimeout(_snpdLayoutTimer);_snpdLayoutTimer=setTimeout(snpdSaveLayout,500);}
  function applyZoom(){
    FR.style.transformOrigin='top left';
    FR.style.transform='scale('+zoom+')';
    FR.style.width=(100/zoom)+'%';
    FR.style.height=(100/zoom)+'%';
    ZL.textContent=Math.round(zoom*100)+'%';
    localStorage.setItem('snpd-zoom',String(zoom));
  }
  applyZoom();
  var drag=false,ox=0,oy=0;
  var MAX_SVG='<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/><line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/></svg>';
  var MIN_SVG='<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="4 14 10 14 10 20"/><polyline points="20 10 14 10 14 4"/><line x1="10" y1="14" x2="21" y2="3"/><line x1="3" y1="21" x2="14" y2="10"/></svg>';
  function applyFs(){
    if(fs){
      W.style.cssText='position:fixed;top:0;left:0;width:100vw;height:100vh;max-width:100vw;max-height:100vh;transform:none;display:flex;flex-direction:column;background:#111827;border:none;border-radius:0;box-shadow:none;z-index:99999';
      W.classList.add('fs');
      B.style.borderRadius='0';B.style.cursor='default';
      FB.innerHTML=MIN_SVG+'<span>Windowed</span>';
    } else {
      W.style.cssText='position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);width:2000px;max-width:95vw;height:1200px;max-height:95vh;display:flex;flex-direction:column;background:#111827;border:2px solid #444;border-radius:8px;box-shadow:0 0 30px rgba(0,0,0,.8);z-index:99999';
      W.classList.remove('fs');
      B.style.borderRadius='8px 8px 0 0';B.style.cursor='grab';
      FB.innerHTML=MAX_SVG+'<span>Fullscreen</span>';
      var sx=localStorage.getItem('snpd-x'),sy=localStorage.getItem('snpd-y');
      if(sx&&sy){W.style.transform='none';W.style.left=sx;W.style.top=sy;}
      var sw=localStorage.getItem('snpd-w'),sh=localStorage.getItem('snpd-h');
      if(sw&&sh){W.style.width=sw;W.style.height=sh;W.style.maxWidth='none';W.style.maxHeight='none';}
    }
  }
  applyFs();
  FB.addEventListener('mousedown',function(e){e.stopPropagation();});
  FB.addEventListener('click',function(e){
    e.stopPropagation();
    fs=!fs;localStorage.setItem('snpd-fs',String(fs));applyFs();snpdSaveLayout();
  });
  B.addEventListener('dblclick',function(e){
    if(e.target.closest('button'))return;
    fs=!fs;localStorage.setItem('snpd-fs',String(fs));applyFs();snpdSaveLayout();
  });
  HB.addEventListener('mousedown',function(e){e.stopPropagation();});
  HB.addEventListener('click',function(e){
    e.stopPropagation();
    FR.src='/proxy';
  });
  RB.addEventListener('mousedown',function(e){e.stopPropagation();});
  RB.addEventListener('click',function(e){
    e.stopPropagation();
    FR.contentWindow.location.reload();
  });
  ZL.addEventListener('mousedown',function(e){e.stopPropagation();});
  ZL.addEventListener('click',function(e){
    e.stopPropagation();
    zoom=1;applyZoom();snpdSaveLayout();
  });
  // Ctrl+Scroll zoom
  document.addEventListener('wheel',function(e){
    if(!e.ctrlKey)return;
    e.preventDefault();
    var delta=e.deltaY<0?0.1:-0.1;
    zoom=Math.min(3,Math.max(0.20,Math.round((zoom+delta)*100)/100));
    applyZoom();snpdSaveLayoutDebounced();
  },{passive:false});
  // Ctrl+Plus / Ctrl+Minus / Ctrl+0 keyboard zoom
  document.addEventListener('keydown',function(e){
    if(!e.ctrlKey)return;
    if(e.key==='='||e.key==='+'||e.keyCode===187||e.keyCode===107){
      e.preventDefault();zoom=Math.min(3,Math.round((zoom+0.1)*100)/100);applyZoom();snpdSaveLayoutDebounced();
    } else if(e.key==='-'||e.keyCode===189||e.keyCode===109){
      e.preventDefault();zoom=Math.max(0.20,Math.round((zoom-0.1)*100)/100);applyZoom();snpdSaveLayoutDebounced();
    } else if(e.key==='0'||e.keyCode===48||e.keyCode===96){
      e.preventDefault();zoom=1;applyZoom();snpdSaveLayoutDebounced();
    }
  },true);
  // Expose zoom functions for C++ Invoke (dispatch hook can't access IIFE vars)
  window.snpdZoomIn=function(){zoom=Math.min(3,Math.round((zoom+0.1)*100)/100);applyZoom();snpdSaveLayoutDebounced();};
  window.snpdZoomOut=function(){zoom=Math.max(0.20,Math.round((zoom-0.1)*100)/100);applyZoom();snpdSaveLayoutDebounced();};
  window.snpdZoomReset=function(){zoom=1;applyZoom();snpdSaveLayoutDebounced();};
  // ── Settings modal ──────────────────────────────────────────────────────────
  var STMO=null; // settings modal overlay element
  function snpdBuildModal(cfg){
    var ov=document.createElement('div');
    ov.style.cssText='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.75);z-index:999999;display:flex;align-items:center;justify-content:center;';
    var box=document.createElement('div');
    box.style.cssText='background:#1f2937;border:1px solid #374151;border-radius:8px;padding:24px;min-width:340px;max-width:480px;width:90%;color:#e5e7eb;font-family:Consolas,"Courier New",monospace;font-size:13px;transform:scale('+uiScale+');';
    var title=document.createElement('div');
    title.style.cssText='font-size:15px;font-weight:700;color:#f9fafb;margin-bottom:16px;border-bottom:1px solid #374151;padding-bottom:10px;';
    title.textContent='Dashboard Settings';
    box.appendChild(title);
    // Hotkey — click-to-capture box
    var hkRow=document.createElement('div');hkRow.style.cssText='margin-bottom:14px;';
    var hkLbl=document.createElement('label');hkLbl.style.cssText='display:block;margin-bottom:4px;color:#9ca3af;';hkLbl.textContent='Toggle Hotkey (click box, then press a key)';
    var hkCode=cfg.hotKey;
    var dxToName={};(cfg.keys||[]).forEach(function(k){dxToName[k.code]=k.name;});
    // event.code to DX (modern browsers)
    var C2D={F1:59,F2:60,F3:61,F4:62,F5:63,F6:64,F7:65,F8:66,F9:67,F10:68,F11:87,F12:88,
      Digit1:2,Digit2:3,Digit3:4,Digit4:5,Digit5:6,Digit6:7,Digit7:8,Digit8:9,Digit9:10,Digit0:11,
      KeyQ:16,KeyW:17,KeyE:18,KeyR:19,KeyT:20,KeyY:21,KeyU:22,KeyI:23,KeyO:24,KeyP:25,
      KeyA:30,KeyS:31,KeyD:32,KeyF:33,KeyG:34,KeyH:35,KeyJ:36,KeyK:37,KeyL:38,
      KeyZ:44,KeyX:45,KeyC:46,KeyV:47,KeyB:48,KeyN:49,KeyM:50,
      Backquote:41,Minus:12,Equal:13,BracketLeft:26,BracketRight:27,
      Semicolon:39,Quote:40,Backslash:86,Comma:51,Period:52,Slash:53,
      Home:199,Insert:210,Delete:211,PageUp:201,PageDown:209,
      ArrowUp:200,ArrowDown:208,ArrowLeft:203,ArrowRight:205,
      Numpad0:82,Numpad1:79,Numpad2:80,Numpad3:81,Numpad4:75,Numpad5:76,
      Numpad6:77,Numpad7:71,Numpad8:72,Numpad9:73,
      NumpadAdd:78,NumpadSubtract:74,NumpadMultiply:55,NumpadDivide:181};
    // event.key to DX (fallback for engines that don\'t populate event.code)
    var K2D={'F1':59,'F2':60,'F3':61,'F4':62,'F5':63,'F6':64,'F7':65,'F8':66,'F9':67,'F10':68,'F11':87,'F12':88,
      '1':2,'2':3,'3':4,'4':5,'5':6,'6':7,'7':8,'8':9,'9':10,'0':11,
      'q':16,'w':17,'e':18,'r':19,'t':20,'y':21,'u':22,'i':23,'o':24,'p':25,
      'a':30,'s':31,'d':32,'f':33,'g':34,'h':35,'j':36,'k':37,'l':38,
      'z':44,'x':45,'c':46,'v':47,'b':48,'n':49,'m':50,
      '`':41,'~':41,'-':12,'_':12,'=':13,'+':13,'[':26,']':27,
      ';':39,':':39,"'":40,'"':40,'\\':86,'|':86,',':51,'<':51,'.':52,'>':52,'/':53,'?':53,
      'Insert':210,'Delete':211,'Home':199,'End':207,'PageUp':201,'PageDown':209,
      'ArrowUp':200,'ArrowDown':208,'ArrowLeft':203,'ArrowRight':205};
    // event.keyCode to DX (last-resort fallback)
    var KC2D={112:59,113:60,114:61,115:62,116:63,117:64,118:65,119:66,120:67,121:68,122:87,123:88,
      49:2,50:3,51:4,52:5,53:6,54:7,55:8,56:9,57:10,48:11,
      81:16,87:17,69:18,82:19,84:20,89:21,85:22,73:23,79:24,80:25,
      65:30,83:31,68:32,70:33,71:34,72:35,74:36,75:37,76:38,
      90:44,88:45,67:46,86:47,66:48,78:49,77:50,
      96:82,97:79,98:80,99:81,100:75,101:76,102:77,103:71,104:72,105:73,
      107:78,109:74,106:55,111:181,
      45:210,46:211,36:199,35:207,33:201,34:209,38:200,40:208,37:203,39:205};
    var hkBox=document.createElement('div');
    hkBox.tabIndex=0;
    hkBox.textContent=dxToName[hkCode]||cfg.hotKeyName||'?';
    hkBox.style.cssText='width:100%;background:#111827;border:1px solid #374151;border-radius:4px;padding:6px 8px;color:#e5e7eb;cursor:pointer;box-sizing:border-box;user-select:none;outline:none;';
    function hkKeyDown(e){
      e.preventDefault();e.stopPropagation();
      var k=e.key||'';var dx=C2D[e.code]||K2D[k]||K2D[k.toLowerCase()]||KC2D[e.keyCode];
      if(dx!==undefined)hkCode=dx;
      hkBox.blur();
    }
    hkBox.addEventListener('focus',function(){hkBox.textContent='Press a key\u2026';hkBox.style.borderColor='#10b981';document.addEventListener('keydown',hkKeyDown,true);});
    hkBox.addEventListener('blur',function(){document.removeEventListener('keydown',hkKeyDown,true);hkBox.textContent=dxToName[hkCode]||('0x'+hkCode.toString(16));hkBox.style.borderColor='#374151';});
    hkRow.appendChild(hkLbl);hkRow.appendChild(hkBox);
    box.appendChild(hkRow);
    // Checkboxes
    function mkCheck(label,checked){
      var row=document.createElement('div');row.style.cssText='margin-bottom:10px;display:flex;align-items:center;gap:8px;';
      var cb=document.createElement('input');cb.type='checkbox';cb.checked=!!checked;cb.style.cssText='width:16px;height:16px;accent-color:#10b981;cursor:pointer;';
      var lbl=document.createElement('label');lbl.style.cssText='color:#d1d5db;cursor:pointer;';lbl.textContent=label;
      lbl.addEventListener('click',function(){cb.checked=!cb.checked;});
      row.appendChild(cb);row.appendChild(lbl);box.appendChild(row);return cb;
    }
    var cbKeepBg  =mkCheck('Keep menu open in background (stay rendered without focus)',cfg.keepBg);
    var cbDefHome =mkCheck('Default to Home page when opening',cfg.defaultHome);
    var cbPause   =mkCheck('Pause game while open',cfg.pauseGame);
    // Note
    var note=document.createElement('div');note.style.cssText='color:#6b7280;font-size:11px;margin-top:4px;margin-bottom:16px;';
    note.textContent='Settings are saved to the plugin INI file immediately.';
    box.appendChild(note);
    // Buttons
    var btnRow=document.createElement('div');btnRow.style.cssText='display:flex;gap:8px;justify-content:flex-end;';
    var cancelBtn=document.createElement('button');cancelBtn.textContent='Cancel';
    cancelBtn.style.cssText='background:#374151;border:1px solid #4b5563;color:#d1d5db;padding:7px 16px;border-radius:4px;cursor:pointer;font-family:inherit;font-size:13px;';
    var saveBtn=document.createElement('button');saveBtn.textContent='Save';
    saveBtn.style.cssText='background:#059669;border:none;color:#fff;padding:7px 16px;border-radius:4px;cursor:pointer;font-family:inherit;font-size:13px;font-weight:600;';
    cancelBtn.addEventListener('click',function(){if(STMO)document.body.removeChild(STMO);STMO=null;});
    saveBtn.addEventListener('click',function(){
      var payload={
        hotKey:hkCode,
        keepBg:cbKeepBg.checked,
        defaultHome:cbDefHome.checked,
        pauseGame:cbPause.checked
      };
      saveBtn.disabled=true;saveBtn.textContent='Saving...';
      fetch('/settings-save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
        .then(function(r){return r.json();})
        .then(function(j){
          if(j.saved){if(STMO)document.body.removeChild(STMO);STMO=null;}
          else{saveBtn.disabled=false;saveBtn.textContent='Save';}
        })
        .catch(function(){saveBtn.disabled=false;saveBtn.textContent='Save';});
    });
    btnRow.appendChild(cancelBtn);btnRow.appendChild(saveBtn);box.appendChild(btnRow);
    ov.appendChild(box);
    ov.addEventListener('mousedown',function(e){e.stopPropagation();});
    return ov;
  }
  var keepBg=!!(window.snpdInitKeepBg);
  function applyKbState(){KBB.title='Keep Background ('+(keepBg?'on':'off')+')';KBB.classList.toggle('active',keepBg);}
  applyKbState();
  KBB.addEventListener('mousedown',function(e){e.stopPropagation();});
  KBB.addEventListener('click',function(e){
    e.stopPropagation();
    fetch('/snpd-toggle-keepbg',{method:'POST'})
      .then(function(r){return r.json();})
      .then(function(j){keepBg=j.keepBg;applyKbState();})
      .catch(function(){});
  });
  SB.addEventListener('mousedown',function(e){e.stopPropagation();});
  SB.addEventListener('click',function(e){
    e.stopPropagation();
    if(STMO){document.body.removeChild(STMO);STMO=null;return;}
    fetch('/settings-get')
      .then(function(r){return r.json();})
      .then(function(cfg){STMO=snpdBuildModal(cfg);document.body.appendChild(STMO);})
      .catch(function(err){console.error('snpd settings-get failed',err);});
  });
  XB.addEventListener('mousedown',function(e){e.stopPropagation();});
  XB.addEventListener('click',function(e){
    e.stopPropagation();
    try{if(typeof window.closeDashboard==='function')window.closeDashboard('');}catch(_){}
  });
  B.addEventListener('mousedown',function(e){
    if(fs||e.button!==0)return;
    var r=W.getBoundingClientRect();
    W.style.transform='none';W.style.top=r.top+'px';W.style.left=r.left+'px';
    ox=e.clientX-r.left;oy=e.clientY-r.top;
    drag=true;OL.style.display='block';B.style.cursor='grabbing';e.preventDefault();
  });
  document.addEventListener('mousemove',function(e){
    if(!drag)return;
    W.style.left=(e.clientX-ox)+'px';W.style.top=(e.clientY-oy)+'px';
  });
  document.addEventListener('mouseup',function(){
    if(!drag)return;
    drag=false;OL.style.display='none';B.style.cursor='grab';
    localStorage.setItem('snpd-x',W.style.left);localStorage.setItem('snpd-y',W.style.top);
    snpdSaveLayout();
  });
)SHELL2"
    R"SHELL2B(
  window.confirm=function(){return true;};
  window.alert=function(){};
  window.prompt=function(m,d){return d!==undefined?d:'';}
  // Belt-and-suspenders: patch the iframe's window object directly (same origin).
  // Ultralight may route iframe dialogs through the top-level window context,
  // so overriding here catches those cases too.
  var snpdFr=FR;
  function snpdPatch(){try{var cw=snpdFr.contentWindow;if(!cw)return;cw.confirm=function(){return true;};cw.alert=function(m){try{var d=cw.document,b=d.createElement('div');b.style.cssText='position:fixed;top:10px;left:50%;transform:translateX(-50%);background:#991b1b;color:#fff;padding:8px 18px;border-radius:6px;font-size:13px;z-index:99999;max-width:84%;text-align:center;box-shadow:0 4px 14px #0008;cursor:pointer;';b.textContent=String(m||'');b.onclick=function(){b.remove();};d.body.appendChild(b);setTimeout(function(){try{b.remove();}catch(e){}},9000);}catch(e){}};cw.prompt=function(m,d){return d!==undefined?d:'';};cw.open=function(url,target,features){var loc={_h:url||''};try{Object.defineProperty(loc,'href',{get:function(){return loc._h;},set:function(u){loc._h=u;if(u&&u!=='about:blank'&&!stub.closed)setTimeout(function(){try{cw.location.href=u;}catch(e){}},0);},enumerable:true,configurable:true});}catch(e){loc.href=url||'';}loc.assign=function(u){loc.href=u;};loc.replace=function(u){loc.href=u;};var stub={closed:false,name:target||'',location:loc,document:{write:function(){},writeln:function(){},open:function(){return this;},close:function(){},createElement:function(){return document.createElement('span');}},close:function(){stub.closed=true;},focus:function(){},blur:function(){},postMessage:function(){}};if(url&&url!=='about:blank')setTimeout(function(){try{cw.location.href=url;}catch(e){}},0);return stub;};cw.close=function(){};try{cw.document.addEventListener('click',function(e){for(var el=e.target;el;el=el.parentElement){if(el.tagName==='A'&&(el.target==='_blank'||el.target==='_new')&&el.href&&el.href.indexOf('javascript:')<0){e.preventDefault();cw.location.href=el.href;break;}}},true);}catch(_){}
  cw.document.addEventListener('wheel',function(e){if(!e.ctrlKey)return;e.preventDefault();e.stopPropagation();var delta=e.deltaY<0?0.1:-0.1;zoom=Math.min(3,Math.max(0.20,Math.round((zoom+delta)*100)/100));applyZoom();snpdSaveLayoutDebounced();},{passive:false,capture:true});}catch(_){}}
  snpdFr.addEventListener('load',snpdPatch);
)SHELL2B"
    "(function(){"
    "var _lh='';"
    "try{_lh=snpdFr.contentWindow.location.href;pushNav(_lh);}catch(e){}"
    "setInterval(function(){"
    "try{"
    "var h=snpdFr.contentWindow.location.href;"
    "if(h&&h!==_lh){"
    "_lh=h;"
    "pushNav(h);"
    "fetch('/audio',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({action:'stop',src:''})}).catch(function(){});"
    "}"
    "}catch(e){}"
    "},500);"
    "})();\n"
    R"SHELL3(

  // ── Nav history (back / forward buttons) ────────────────────────────────────
  var navHist=[],navIdx=-1;
  function updNav(){BB.disabled=navIdx<1;FWB.disabled=navIdx>=navHist.length-1;}
  function pushNav(u){if(!u||u===navHist[navIdx])return;navHist=navHist.slice(0,navIdx+1);navHist.push(u);navIdx++;updNav();}
  BB.addEventListener('mousedown',function(e){e.stopPropagation();});
  BB.addEventListener('click',function(e){e.stopPropagation();if(navIdx>0){navIdx--;try{snpdFr.contentWindow.location.href=navHist[navIdx];}catch(_){snpdFr.src=navHist[navIdx];}updNav();}});
  FWB.addEventListener('mousedown',function(e){e.stopPropagation();});
  FWB.addEventListener('click',function(e){e.stopPropagation();if(navIdx<navHist.length-1){navIdx++;try{snpdFr.contentWindow.location.href=navHist[navIdx];}catch(_){snpdFr.src=navHist[navIdx];}updNav();}});

  // ── Find bar (Ctrl+F) ──────────────────────────────────────────────────────
  var _snfb=document.getElementById('SNFB'),_snfi=document.getElementById('SNFI');
  var _snfp=document.getElementById('SNFP'),_snfn=document.getElementById('SNFN'),_snfx=document.getElementById('SNFX');
  var _findOpen=false;
  function snpdShowFind(){
    _snfb.style.display='flex';_snfi.value='';_snfi.focus();_findOpen=true;
    try{window.snpdFindState('1');}catch(_){}
    try{FR.contentWindow.getSelection().removeAllRanges();}catch(_){}
  }
  function snpdHideFind(){
    _snfb.style.display='none';_findOpen=false;window._snpdFindFocused=false;
    try{window.snpdFindState('0');}catch(_){}
    try{FR.contentWindow.getSelection().removeAllRanges();}catch(_){}
    try{FR.contentWindow.focus();}catch(_){}
  }
  window.snpdToggleFind=function(){
    // If a CM6 editor is focused inside the iframe, let CM6 handle Ctrl+F
    if(!_findOpen){try{
      var ae=FR.contentWindow.document.activeElement;
      if(ae&&ae.closest&&ae.closest('.cm-editor'))return;
    }catch(_){}}
    if(_findOpen)snpdHideFind();else snpdShowFind();};
  function snpdDoFind(backwards){
    var q=_snfi.value;if(!q)return;
    try{FR.contentWindow.find(q,false,backwards,true,false,false,false);}catch(_){}
  }
  _snfi.addEventListener('keydown',function(e){
    if(e.key==='Enter'){e.preventDefault();snpdDoFind(e.shiftKey);}
    else if(e.key==='Escape'){e.preventDefault();snpdHideFind();}
  });
  _snfi.addEventListener('input',function(){var q=_snfi.value;if(q)snpdDoFind(false);});
  _snfp.addEventListener('click',function(e){e.stopPropagation();snpdDoFind(true);});
  _snfn.addEventListener('click',function(e){e.stopPropagation();snpdDoFind(false);});
  _snfx.addEventListener('click',function(e){e.stopPropagation();snpdHideFind();});
  _snfi.addEventListener('mousedown',function(e){e.stopPropagation();});
  _snfi.addEventListener('focus',function(){window._snpdFindFocused=true;});
  _snfi.addEventListener('blur',function(){window._snpdFindFocused=false;});
  _snfi.addEventListener('copy',function(e){e.preventDefault();},true);
  _snfi.addEventListener('cut',function(e){e.preventDefault();},true);
  // Block Ultralight's native clipboard — our C++ monitor handles it
  _snfi.addEventListener('keydown',function(e){
    if(e.ctrlKey&&(e.key==='c'||e.key==='C'||e.key==='x'||e.key==='X')){
      e.preventDefault();e.stopPropagation();}
    if(e.ctrlKey&&(e.key==='a'||e.key==='A')){e.preventDefault();_snfi.setSelectionRange(0,_snfi.value.length);}
  },true);
  // Manual drag selection — Ultralight suppresses mousemove during button hold
  (function(){
    var anchor=-1,cachedFont='',cachedRect=null,cachedPad=0,cachedWidths=null;
    var ctx=document.createElement('canvas').getContext('2d');
    function buildWidths(){
      var f=getComputedStyle(_snfi).font;
      if(f!==cachedFont||!cachedWidths){cachedFont=f;ctx.font=f;}
      cachedRect=_snfi.getBoundingClientRect();
      cachedPad=parseFloat(getComputedStyle(_snfi).paddingLeft)||0;
      var txt=_snfi.value;
      cachedWidths=new Array(txt.length+1);
      for(var i=0;i<=txt.length;i++)cachedWidths[i]=ctx.measureText(txt.substring(0,i)).width;
    }
    function charPosAt(x){
      var rel=x-cachedRect.left-cachedPad+_snfi.scrollLeft;
      // Binary search
      var lo=0,hi=cachedWidths.length-1;
      while(lo<hi){var mid=(lo+hi)>>1;if(cachedWidths[mid]<rel)lo=mid+1;else hi=mid;}
      return lo;
    }
    _snfi.addEventListener('pointerdown',function(e){
      buildWidths();
      anchor=charPosAt(e.clientX);
    });
    function endDrag(){anchor=-1;cachedWidths=null;}
    document.addEventListener('pointerup',endDrag);
    document.addEventListener('pointercancel',endDrag);
    _snfi.addEventListener('blur',endDrag);
    document.addEventListener('pointermove',function(e){
      if(anchor<0||document.activeElement!==_snfi)return;
      var pos=charPosAt(e.clientX);
      var s=Math.min(anchor,pos),end=Math.max(anchor,pos);
      _snfi.setSelectionRange(s,end,pos<anchor?'backward':'forward');
    });
  })();

  // ── Resize handles ─────────────────────────────────────────────────────────
  var rDir=null,rSX=0,rSY=0,rRect=null;
  var RMIN_W=320,RMIN_H=200;
  document.querySelectorAll('.rh').forEach(function(h){
    h.addEventListener('mousedown',function(e){
      if(fs||e.button!==0)return;
      e.preventDefault();e.stopPropagation();
      rDir=h.dataset.r;rSX=e.clientX;rSY=e.clientY;
      var r=W.getBoundingClientRect();
      rRect={left:r.left,top:r.top,width:r.width,height:r.height};
      W.style.transform='none';W.style.left=r.left+'px';W.style.top=r.top+'px';
      W.style.width=r.width+'px';W.style.height=r.height+'px';
      W.style.maxWidth='none';W.style.maxHeight='none';
      OL.style.display='block';
    });
  });
  document.addEventListener('mousemove',function(e){
    if(!rDir)return;
    var dx=e.clientX-rSX,dy=e.clientY-rSY,r=rRect;
    var nL=r.left,nT=r.top,nW=r.width,nH=r.height;
    if(rDir.indexOf('e')>=0){nW=Math.max(RMIN_W,r.width+dx);}
    if(rDir.indexOf('w')>=0){var w2=Math.max(RMIN_W,r.width-dx);nL=r.left+(r.width-w2);nW=w2;}
    if(rDir.indexOf('s')>=0){nH=Math.max(RMIN_H,r.height+dy);}
    if(rDir.indexOf('n')>=0){var h2=Math.max(RMIN_H,r.height-dy);nT=r.top+(r.height-h2);nH=h2;}
    W.style.left=nL+'px';W.style.top=nT+'px';W.style.width=nW+'px';W.style.height=nH+'px';
  });
  document.addEventListener('mouseup',function(){
    if(!rDir)return;
    rDir=null;OL.style.display='none';
    localStorage.setItem('snpd-x',W.style.left);localStorage.setItem('snpd-y',W.style.top);
    localStorage.setItem('snpd-w',W.style.width);localStorage.setItem('snpd-h',W.style.height);
    snpdSaveLayout();
  });

})();
</script>
</body>
</html>
)SHELL3";

    return html;
}
