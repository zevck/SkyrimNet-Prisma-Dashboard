#pragma once
#include <string>

namespace Injections {

// File-input polyfill: intercepts <input type="file"> clicks and uses the
// C++ /open-dialog endpoint to show a native Windows file-open dialog, then
// fetches the chosen file bytes via /read-file and synthesises a File object
// that the page's upload handler can read from input.files as normal.
//
// Required because Ultralight has no OS file picker of its own.
inline std::string GetFileInputPolyfill()
{
    return R"FILEINP(
// ── Blob URL polyfill ────────────────────────────────────────────────────────
// Ultralight's blob: URLs don't resolve for <img> src.  Store blobs in a map
// keyed by fake URL, then resolve asynchronously when an <img> uses the URL.
(function(){
var _blobs={};
var _id=0;
var _origCreate=URL.createObjectURL;
var _origRevoke=URL.revokeObjectURL;
URL.createObjectURL=function(blob){
var key='snpd-blob:'+(++_id);
_blobs[key]=blob;
// Eagerly convert to data URL and patch any elements using this key
var fr=new FileReader();
fr.onloadend=function(){
if(!fr.result)return;
_blobs[key]=fr.result; // replace blob with data URL
var imgs=document.querySelectorAll('img[src="'+key+'"]');
for(var i=0;i<imgs.length;i++)imgs[i].src=fr.result;
// Also check background-image styles
var all=document.querySelectorAll('[style*="'+key+'"]');
for(var i=0;i<all.length;i++){
all[i].style.backgroundImage=all[i].style.backgroundImage.replace(key,fr.result);
}};
fr.readAsDataURL(blob);
return key;
};
URL.revokeObjectURL=function(url){
if(url&&_blobs[url]){delete _blobs[url];return;}
if(_origRevoke)_origRevoke.call(URL,url);
};
// Intercept img.src to resolve snpd-blob: URLs
var _srcDesc=Object.getOwnPropertyDescriptor(HTMLImageElement.prototype,'src');
if(_srcDesc&&_srcDesc.set){
Object.defineProperty(HTMLImageElement.prototype,'src',{
get:_srcDesc.get,
set:function(v){
if(v&&typeof v==='string'&&v.indexOf('snpd-blob:')===0){
var d=_blobs[v];
if(d&&typeof d==='string'){_srcDesc.set.call(this,d);return;}
// Data URL not ready yet — set a placeholder and wait
var img=this;
_srcDesc.set.call(this,'');
var check=setInterval(function(){
var d2=_blobs[v];
if(d2&&typeof d2==='string'){clearInterval(check);_srcDesc.set.call(img,d2);}
},50);
return;
}
_srcDesc.set.call(this,v);
},configurable:true,enumerable:true});
}
})();

// ── Native file picker for <input type="file"> ───────────────────────────────
(function(){
// Page-context accept override: if the page URL contains a known keyword,
// force a more specific accept string regardless of what the input declares.
function snpdResolveAccept(inp){
  var acc=inp.getAttribute('accept')||'';
  if(acc&&acc!=='*/*'&&acc!=='*')return acc;
  // Voice samples page: the upload input wants .wav/.fuz/.xwm files.
  var pm=window.location.pathname+window.location.hash;
  if(pm.indexOf('voice')>=0||pm.indexOf('sample')>=0)
    return '.wav,.fuz,.xwm';
  return acc||'*/*';
}
var _snpdDialogOpen=false;
var _oClick=HTMLInputElement.prototype.click;
HTMLInputElement.prototype.click=function(){
  if(this.type!=='file'){return _oClick.call(this);}
  // Guard: ignore click if a dialog is already open (e.g. double-click tail
  // fires after the native dialog closes and the cursor is still over the button).
  if(_snpdDialogOpen)return;
  _snpdDialogOpen=true;
  var inp=this;
  var acc=snpdResolveAccept(inp);
  fetch('/open-dialog?accept='+encodeURIComponent(acc))
    .then(function(r){return r.json();})
    .then(function(j){
      // Delay clearing the lock so any ghost clicks that arrive immediately
      // after the native dialog closes (double-click tail, game input replay)
      // are still blocked.
      setTimeout(function(){_snpdDialogOpen=false;},400);
      if(!j||j.cancelled||!j.path)return;
      return fetch('/read-file?path='+encodeURIComponent(j.path))
        .then(function(r){return r.arrayBuffer();})
        .then(function(ab){
          var fn=j.path.replace(/.*[\\/]/g,'');
          var f=new File([ab],fn,{type:j.mimeType||'application/octet-stream'});
          try{
            var dt=new DataTransfer();dt.items.add(f);
            Object.defineProperty(inp,'files',{value:dt.files,configurable:true});
          }catch(e){
            // DataTransfer not available — expose as minimal FileList-like object
            var fl={0:f,length:1,item:function(i){return fl[i]||null;}};
            Object.defineProperty(inp,'files',{get:function(){return fl;},configurable:true});
          }
          inp.dispatchEvent(new Event('change',{bubbles:true}));
          inp.dispatchEvent(new Event('input',{bubbles:true}));
        });
    })
    .catch(function(e){setTimeout(function(){_snpdDialogOpen=false;},400);console.error('snpd open-dialog:',String(e));});
};
// Also intercept <label> clicks that activate file inputs via the browser's
// default action — those bypass HTMLInputElement.prototype.click entirely.
document.addEventListener('click',function(e){
  for(var el=e.target;el;el=el.parentElement){
    if(el.tagName==='LABEL'){
      var fid=el.getAttribute('for');
      var fi=fid?document.getElementById(fid):el.querySelector('input[type=file]');
      if(fi&&fi.type==='file'){
        e.preventDefault();e.stopImmediatePropagation();fi.click();
      }
      break;
    }
  }
},true);
})();
)FILEINP";
}

} // namespace Injections
