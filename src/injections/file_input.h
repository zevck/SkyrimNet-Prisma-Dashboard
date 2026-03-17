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
      _snpdDialogOpen=false;
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
    .catch(function(e){_snpdDialogOpen=false;console.error('snpd open-dialog:',String(e));});
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
