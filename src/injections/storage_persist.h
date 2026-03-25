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
// Protects imported/restored values from being overwritten by app init.
(function(){
var _saveTimer=0;
var _origSetItem=Storage.prototype.setItem;
var _origRemoveItem=Storage.prototype.removeItem;
var _origClear=Storage.prototype.clear;
var _loaded=false;

// Snapshot localStorage BEFORE the app runs — captures restored data.
var _snapshot={};
for(var _si=0;_si<localStorage.length;_si++){
var _sk=localStorage.key(_si);
if(_sk&&_sk.indexOf('snpd-')!==0&&_sk[0]!=='_')
_snapshot[_sk]=localStorage.getItem(_sk);}

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

// Override Storage.prototype to catch all write patterns
Storage.prototype.setItem=function(k,v){
// Block writes that would shrink a snapshot value (app defaults replacing imported data)
if(_snapshot[k]&&typeof v==='string'&&v.length<_snapshot[k].length)return;
_origSetItem.call(this,k,v);
if(_loaded&&k.indexOf('snpd-')!==0)_scheduleSave();};

Storage.prototype.removeItem=function(k){
if(_snapshot[k])return;
_origRemoveItem.call(this,k);
if(_loaded&&k.indexOf('snpd-')!==0)_scheduleSave();};

Storage.prototype.clear=function(){
_origClear.call(this);
// Re-populate snapshot keys so imported data survives clear()
var ks=Object.keys(_snapshot);
for(var i=0;i<ks.length;i++)_origSetItem.call(this,ks[i],_snapshot[ks[i]]);
if(_loaded)_scheduleSave();};

// Restore saved data on load
fetch('/snpd-storage-get').then(function(r){return r.json();})
.then(function(data){
if(data&&typeof data==='object'){
var keys=Object.keys(data);
for(var i=0;i<keys.length;i++){
_origSetItem.call(localStorage,keys[i],data[keys[i]]);
if(keys[i].indexOf('snpd-')!==0&&keys[i][0]!=='_')
_snapshot[keys[i]]=data[keys[i]];}}
_loaded=true;
}).catch(function(){_loaded=true;});
})();
)STORAGE";
}

} // namespace Injections
