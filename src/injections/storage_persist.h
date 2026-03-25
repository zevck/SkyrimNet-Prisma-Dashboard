#pragma once
#include <string>

namespace Injections {

// Persists the iframe's localStorage to disk via the shell server.
// On load: fetches saved data from /snpd-storage-get and populates localStorage.
// On changes: debounced POST to /snpd-storage-save with all localStorage data.
inline std::string GetStoragePersist()
{
    return R"STORAGE(
// ── localStorage persistence ─────────────────────────────────────────
// Restores localStorage from disk on load and saves changes back.
(function(){
var _saveTimer=0;
var _origSetItem=localStorage.setItem.bind(localStorage);
var _origRemoveItem=localStorage.removeItem.bind(localStorage);
var _origClear=localStorage.clear.bind(localStorage);
var _loaded=false;

function _getAllStorage(){
var data={};
for(var i=0;i<localStorage.length;i++){
var k=localStorage.key(i);
if(k&&k.indexOf('snpd-')!==0)data[k]=localStorage.getItem(k);}
return data;}

function _scheduleSave(){
if(_saveTimer)clearTimeout(_saveTimer);
_saveTimer=setTimeout(function(){_saveTimer=0;
var data=_getAllStorage();
fetch('/snpd-storage-save',{method:'POST',
headers:{'Content-Type':'application/json'},
body:JSON.stringify(data)}).catch(function(){});
},500);}

// Override setItem to trigger save
localStorage.setItem=function(k,v){
_origSetItem(k,v);
if(_loaded&&k.indexOf('snpd-')!==0)_scheduleSave();};

localStorage.removeItem=function(k){
_origRemoveItem(k);
if(_loaded&&k.indexOf('snpd-')!==0)_scheduleSave();};

localStorage.clear=function(){
_origClear();
if(_loaded)_scheduleSave();};

// Restore saved data on load
fetch('/snpd-storage-get').then(function(r){return r.json();})
.then(function(data){
if(data&&typeof data==='object'){
var keys=Object.keys(data);
for(var i=0;i<keys.length;i++){
_origSetItem(keys[i],data[keys[i]]);}}
_loaded=true;
}).catch(function(){_loaded=true;});
})();
)STORAGE";
}

} // namespace Injections
