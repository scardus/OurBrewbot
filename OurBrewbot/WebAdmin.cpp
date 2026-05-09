/*
 * WebAdmin.cpp — Admin configuration page (PROGMEM HTML)
 */

#include "WebAPI.h"

// ============================================================
// ADMIN PAGE HTML — stored in flash via PROGMEM
// Served via chunked transfer to avoid RAM copy
// ============================================================

static const char ADMIN_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OurBrewbot Admin</title>
<style>
* {
  box-sizing: border-box;
  margin: 0;
  padding: 0;
}

body {
  font-family: system-ui, sans-serif;
  background: #1a1a2e;
  color: #e0e0e0;
  padding: 8px;
}

h2 {
  color: #e94560;
  margin: 0 0 12px;
}

/* ---- Tabs ---- */
.tabs {
  display: flex;
  gap: 4px;
  margin-bottom: 12px;
  flex-wrap: wrap;
}

.tabs button {
  padding: 8px 16px;
  border: 1px solid #444;
  background: #16213e;
  color: #e0e0e0;
  cursor: pointer;
  border-radius: 4px 4px 0 0;
  font-size: 14px;
}

.tabs button.active {
  background: #0f3460;
  border-bottom: 2px solid #e94560;
  color: #fff;
}

.tab {
  display: none;
}

.tab.active {
  display: block;
}

/* ---- Cards & rows ---- */
.card {
  background: #16213e;
  border: 1px solid #333;
  border-radius: 6px;
  padding: 12px;
  margin-bottom: 10px;
}

.card h3 {
  color: #e94560;
  margin-bottom: 8px;
  font-size: 15px;
}

.row {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-bottom: 6px;
  align-items: center;
}

.row label {
  min-width: 120px;
  font-size: 13px;
  color: #aaa;
}

.row input,
.row select {
  background: #0f3460;
  border: 1px solid #444;
  color: #e0e0e0;
  padding: 4px 8px;
  border-radius: 3px;
  font-size: 13px;
}

.row input[type=number] {
  width: 80px;
}

.row input[type=text] {
  width: 160px;
}

/* ---- Badges ---- */
.badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 10px;
  font-size: 12px;
  font-weight: bold;
}

.badge-idle  { background: #333;    color: #888; }
.badge-heat  { background: #e94560; color: #fff; }
.badge-cool  { background: #0f3460; color: #53d8fb; }
.badge-alarm { background: #ff0;    color: #000; }
.badge-off   { background: #222;    color: #555; }
.badge-prof  { background: #8b5cf6; color: #fff; }

.live {
  color: #53d8fb;
  font-size: 13px;
  margin-bottom: 8px;
}

/* ---- Buttons ---- */
button.save {
  background: #e94560;
  color: #fff;
  border: none;
  padding: 6px 16px;
  border-radius: 4px;
  cursor: pointer;
  font-size: 13px;
  margin-top: 4px;
}

button.save:hover {
  background: #c73650;
}

button.test {
  background: #0f3460;
  color: #53d8fb;
  border: 1px solid #53d8fb;
  padding: 4px 10px;
  border-radius: 3px;
  cursor: pointer;
  font-size: 12px;
  margin: 0 2px;
}

button.test:hover {
  background: #1a4a8a;
}

button.danger {
  background: #600;
  color: #faa;
  border: 1px solid #a33;
  padding: 6px 16px;
  border-radius: 4px;
  cursor: pointer;
  font-size: 13px;
  margin: 4px 4px 0 0;
}

/* ---- Status messages ---- */
.msg {
  font-size: 12px;
  margin-top: 4px;
  min-height: 16px;
}

.msg.ok  { color: #4f4; }
.msg.err { color: #f44; }

/* ---- Tables ---- */
.tbl {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.tbl th {
  text-align: left;
  padding: 4px 8px;
  border-bottom: 1px solid #444;
  color: #aaa;
  font-size: 12px;
}

.tbl td {
  padding: 4px 8px;
  border-bottom: 1px solid #222;
}

/* ---- Toggle switch ---- */
.sw {
  position: relative;
  display: inline-block;
  width: 36px;
  height: 20px;
}

.sw input {
  opacity: 0;
  width: 0;
  height: 0;
}

.sw .sl {
  position: absolute;
  cursor: pointer;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: #333;
  border-radius: 20px;
  transition: .2s;
}

.sw input:checked + .sl {
  background: #e94560;
}

.sw .sl:before {
  content: "";
  position: absolute;
  height: 14px;
  width: 14px;
  left: 3px;
  bottom: 3px;
  background: #fff;
  border-radius: 50%;
  transition: .2s;
}

.sw input:checked + .sl:before {
  transform: translateX(16px);
}

/* ---- System-info panel ---- */
.info {
  background: #0f3460;
  border: 1px solid #333;
  border-radius: 6px;
  padding: 12px;
  margin-bottom: 10px;
  font-size: 13px;
}

.info .r {
  display: flex;
  justify-content: space-between;
  padding: 2px 0;
}

.info .r .v {
  color: #53d8fb;
}
</style>
</head>
<body>
  <h2>OurBrewbot Admin</h2>
  <div class="tabs">
    <button onclick="showTab(0)" id="tb0" class="active">Fermenters</button>
    <button onclick="showTab(1)" id="tb1">Profiles</button>
    <button onclick="showTab(2)" id="tb2">Probes</button>
    <button onclick="showTab(3)" id="tb3">Tilts</button>
    <button onclick="showTab(4)" id="tb4">iSpindels</button>
    <button onclick="showTab(5)" id="tb5">Smart Plugs</button>
    <button onclick="showTab(6)" id="tb6">Reporting</button>
    <button onclick="showTab(7)" id="tb7">System Settings</button>
  </div>
  <div id="t0" class="tab active"></div>
  <div id="t1" class="tab"></div>
  <div id="t2" class="tab"></div>
  <div id="t3" class="tab"></div>
  <div id="t4" class="tab"></div>
  <div id="t5" class="tab"></div>
  <div id="t6" class="tab"></div>
  <div id="t7" class="tab"></div>

<script>
var T = 0;
var R = null;
var dirty = false;
var dirtyTimer = null;

// Mark the form as edited; auto-refresh resumes after a 30-second idle.
function markDirty() {
  dirty = true;
  if (dirtyTimer) clearTimeout(dirtyTimer);
  dirtyTimer = setTimeout(function () { dirty = false; }, 30000);
}

// Switch the active tab and trigger a re-render of its contents.
function showTab(n) {
  T = n;
  dirty = false;
  for (var i = 0; i < 8; i++) {
    document.getElementById('t' + i).className = 'tab' + (i == n ? ' active' : '');
    document.getElementById('tb' + i).className = i == n ? 'active' : '';
  }
  loadTab();
}

// Shorthand for document.getElementById.
function $(s) {
  return document.getElementById(s);
}

// Show a status message in the element with the given id, styled ok (green) or err (red).
function msg(id, t, ok) {
  var e = $(id);
  if (e) {
    e.textContent = t;
    e.className = 'msg ' + (ok ? 'ok' : 'err');
  }
}

// Confirm with the user, then clear WiFi credentials and reboot into the setup portal.
function resetWiFiSettings() {
  if (!confirm('Are you sure you want to clear WiFi settings? The controller will reboot and reopen the setup portal.')) return;
  fetch('/wifi/reset', { method: 'POST' })
    .then(function (r) { return r.json(); })
    .then(function (d) { alert(d.msg || 'Rebooting into setup portal...'); })
    .catch(function (e) { alert('Error: ' + e); });
}

function loadTab(){
if(T==0)loadFermenters();
else if(T==1)loadProfiles();
else if(T==2)loadProbes();
else if(T==3)loadTilts();
else if(T==4)loadISpindels();
else if(T==5)loadPlugs();
else if(T==6)loadReporting();
else if(T==7)loadSystemSettings();
}

function statusBadge(s,pwr){
if(!pwr)return'<span class="badge badge-off">OFF</span>';
if(s==1)return'<span class="badge badge-heat">Heating</span>';
if(s==2)return'<span class="badge badge-cool">Cooling</span>';
if(s==3)return'<span class="badge badge-alarm">ALARM</span>';
return'<span class="badge badge-idle">Idle</span>';
}

function sw(id,chk){return'<label class="sw"><input type="checkbox" id="'+id+'"'+(chk?' checked':'')+'><span class="sl"></span></label>'}

// ---- FERMENTERS TAB ----
var SVC=[],MQEN=false,PNAMES=[];
function loadFermenters(){
Promise.all([fetch('/brewservices').then(function(r){return r.json()}),fetch('/mqtt').then(function(r){return r.json()}),fetch('/fermenters').then(function(r){return r.json()}),fetch('/profiles').then(function(r){return r.json()})]).then(function(res){
SVC=res[0].services||[];MQEN=res[1].enabled;var d=res[2];
var profs=res[3].profiles||[];PNAMES=[];for(var p=0;p<profs.length;p++)PNAMES.push(profs[p].name);
var h='';
for(var i=0;i<d.length;i++){var f=d[i];
h+='<div class="card"><h3>Fermenter '+i+' '+statusBadge(f.Status,f.Power)+' '+(f.ProfileNo>=1?'<span class="badge badge-prof">'+f.ProfileName+'</span>':'<span class="badge badge-idle">Standard</span>')+'</h3>';
h+='<div class="live">Beer: '+(f.BeerTemp>-100?f.BeerTemp.toFixed(1)+'&deg;'+f.TempUnit+(f.BeerTempSource&&f.BeerTempSource!='None'?' ('+f.BeerTempSource+')':''):'--')
  +' &nbsp; Ambient: '+(f.AmbientTemp>-100?f.AmbientTemp.toFixed(1)+'&deg;'+f.TempUnit:'--')
  +' &nbsp; SG: '+(f.SG>0?f.SG.toFixed(3)+(f.GravitySource?' ('+f.GravitySource+')':''):'--')
  +(f.ProfileRunning?' &nbsp; Step: '+(f.CurrentStep+1)+'/'+f.TotalSteps+' &nbsp; Hour: '+f.CurrentHour:'')+'</div>';
h+='<div class="row"><label>Name</label><input type="text" id="fn'+i+'" value="'+f.FermenterName+'"></div>';
h+='<div class="row"><label>Beer</label><input type="text" id="bn'+i+'" value="'+f.BeerName+'"></div>';
h+='<div class="row"><label>Ceiling Temp</label><input type="number" step="0.1" id="ct'+i+'" value="'+f.CeilingTemp+'"></div>';
h+='<div class="row"><label>Floor Temp</label><input type="number" step="0.1" id="ft'+i+'" value="'+f.FloorTemp+'"></div>';
h+='<div class="row"><label>Hysteresis</label><input type="number" step="0.1" id="hy'+i+'" value="'+f.Hysteresis+'"></div>';
h+='<div class="row"><label>Compressor Delay</label><input type="number" id="cd'+i+'" value="'+f.CompressorDelay+'"> min</div>';
h+='<div class="row"><label>Yeast</label><input type="text" id="yn'+i+'" value="'+f.YeastName+'"></div>';
h+='<div class="row"><label>OG</label><input type="number" step="0.001" min="1.0" max="1.2" id="og'+i+'" value="'+f.OG+'" style="width:80px"> <label style="min-width:auto">TG</label><input type="number" step="0.001" min="1.0" max="1.2" id="tg'+i+'" value="'+f.TG+'" style="width:80px"></div>';
h+='<div class="row"><label>Power</label>'+sw('pw'+i,f.Power)+'</div>';
h+='<div class="row"><label>Temp Control</label>'+sw('tc'+i,f.TempControl)+'</div>';
h+='<div class="row"><label>Profile</label><select id="fp'+i+'"><option value="0"'+(f.ProfileNo==0?' selected':'')+'>Standard</option>';
for(var p=0;p<PNAMES.length;p++)h+='<option value="'+(p+1)+'"'+(f.ProfileNo==(p+1)?' selected':'')+'>'+PNAMES[p]+'</option>';
h+='</select></div>';
if(f.ProfileNo>=1)h+='<div class="row"><label>Profile Control</label>';
if(f.ProfileNo>=1&&!f.ProfileRunning)h+='<button class="test" onclick="profAction('+i+',\'start\')">Start</button> ';
if(f.ProfileRunning)h+='<button class="test" onclick="profAction('+i+',\'pause\')">Pause</button> ';
if(f.ProfileNo>=1)h+='<button class="test" onclick="profAction('+i+',\'stop\')">Stop</button> ';
if(f.ProfileRunning){h+='<button class="test" onclick="profAction('+i+',\'prev\')">&laquo; Prev</button> ';
h+='<button class="test" onclick="profAction('+i+',\'next\')">Next &raquo;</button>';}
if(f.ProfileNo>=1)h+='</div>';
if(f.ProfileNo>=1)h+='<div class="row"><label>Test Mode</label>'+sw('lt'+i,f.LiveTest)+'</div>';
var bs=f.BrewServices||0;
var hasSvc=false;
for(var s=0;s<SVC.length;s++){if(SVC[s].enabled){
h+='<div class="row"><label>'+SVC[s].name+'</label>'+sw('bsv'+i+'_'+s,!!(bs&(1<<s)))+'</div>';
hasSvc=true;}}
if(MQEN){h+='<div class="row"><label>MQTT</label>'+sw('bsv'+i+'_3',!!(bs&(1<<3)))+'</div>';hasSvc=true;}
if(!hasSvc)h+='<div class="row"><label>Brew Services</label><span style="color:#888;font-size:12px">None enabled — configure in Reporting tab</span></div>';
h+='<button class="save" onclick="saveFerm('+i+')">Save</button> <span class="msg" id="fm'+i+'"></span>';
h+='</div>';
}
$('t0').innerHTML=h;
})}

function saveFerm(i){
var bs=0;
for(var s=0;s<SVC.length;s++){var el=$('bsv'+i+'_'+s);if(el&&el.checked)bs|=(1<<s);}
var mqel=$('bsv'+i+'_3');if(mqel&&mqel.checked)bs|=(1<<3);
var body={Fermenter:i,
FermenterName:$('fn'+i).value,
BeerName:$('bn'+i).value,
YeastName:$('yn'+i).value,
OG:parseFloat($('og'+i).value),
TG:parseFloat($('tg'+i).value),
CeilingTemp:parseFloat($('ct'+i).value),
FloorTemp:parseFloat($('ft'+i).value),
Hysteresis:parseFloat($('hy'+i).value),
CompressorDelay:parseInt($('cd'+i).value),
Power:$('pw'+i).checked,
TempControl:$('tc'+i).checked,
ProfileNo:parseInt($('fp'+i).value),
LiveTest:$('lt'+i)?$('lt'+i).checked:false,
BrewServices:bs};
fetch('/fermenter',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(r=>r.json()).then(function(d){msg('fm'+i,d.msg,d.status=='ok');dirty=false;setTimeout(loadFermenters,500)})
.catch(function(e){msg('fm'+i,'Error: '+e,false)})}

function profAction(i,a){
var body={Fermenter:i,action:a};
if(a=='start')body.ProfileIndex=parseInt($('fp'+i).value);
fetch('/fermenter/profile',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('fm'+i,d.msg,d.status=='ok');dirty=false;setTimeout(loadFermenters,500)})
.catch(function(e){msg('fm'+i,'Error: '+e,false)})}

// ---- PROFILES TAB ----
var STYPES=[];
// Fields relevant per step type: s=startTemp, e=endTemp, g=sgTrigger, d=days
var SFIELDS={0:'segd',1:'sed',2:'sed',3:'sgd',4:'sg',5:'se',6:'sg',7:'sg',8:'segd',9:'sed'};
function stepFieldsEnabled(t){var f=SFIELDS[t]||'segd';return{s:f.indexOf('s')>=0,e:f.indexOf('e')>=0,g:f.indexOf('g')>=0,d:f.indexOf('d')>=0}}
function onStepTypeChange(p,s){var t=parseInt($('pst'+p+'_'+s).value);var fl=stepFieldsEnabled(t);$('pss'+p+'_'+s).disabled=!fl.s;$('pse'+p+'_'+s).disabled=!fl.e;$('psg'+p+'_'+s).disabled=!fl.g;$('psd'+p+'_'+s).disabled=!fl.d}

function loadProfiles(){
fetch('/profiles').then(function(r){return r.json()}).then(function(d){
STYPES=d.stepTypes||[];
var ps=d.profiles||[];
var h='';
for(var p=0;p<ps.length;p++){var pr=ps[p];
h+='<div class="card"><h3>Profile '+(p+1)+'</h3>';
h+='<div class="row"><label>Name</label><input type="text" id="ppn'+p+'" value="'+pr.name+'" style="width:200px"></div>';
h+='<table class="tbl"><tr><th>#</th><th>Step Type</th><th>Start Temp</th><th>End Temp</th><th>SG Trigger</th><th>Days</th></tr>';
for(var s=0;s<pr.steps.length;s++){var st=pr.steps[s];var fl=stepFieldsEnabled(st.stepType);
h+='<tr><td>'+(s+1)+'</td>';
h+='<td><select id="pst'+p+'_'+s+'" onchange="onStepTypeChange('+p+','+s+')">';
for(var t=0;t<STYPES.length;t++)h+='<option value="'+STYPES[t].id+'"'+(STYPES[t].id==st.stepType?' selected':'')+'>'+STYPES[t].name+'</option>';
h+='</select></td>';
h+='<td><input type="number" step="0.1" id="pss'+p+'_'+s+'" value="'+st.startTemp+'" style="width:70px"'+(fl.s?'':' disabled')+'></td>';
h+='<td><input type="number" step="0.1" id="pse'+p+'_'+s+'" value="'+st.endTemp+'" style="width:70px"'+(fl.e?'':' disabled')+'></td>';
h+='<td><input type="number" step="0.001" id="psg'+p+'_'+s+'" value="'+st.sgTrigger+'" style="width:80px"'+(fl.g?'':' disabled')+'></td>';
h+='<td><input type="number" step="0.1" id="psd'+p+'_'+s+'" value="'+st.days+'" style="width:60px"'+(fl.d?'':' disabled')+'></td></tr>';}
h+='</table>';
h+='<button class="save" onclick="saveProfile('+p+')">Save Profile '+(p+1)+'</button> <span class="msg" id="ppm'+p+'"></span>';
h+='</div>';}
$('t1').innerHTML=h;
})}

function saveProfile(p){
var body={index:p,name:$('ppn'+p).value,steps:[]};
for(var s=0;s<15;s++){
body.steps.push({stepType:parseInt($('pst'+p+'_'+s).value)||0,startTemp:parseFloat($('pss'+p+'_'+s).value)||0,endTemp:parseFloat($('pse'+p+'_'+s).value)||0,sgTrigger:parseFloat($('psg'+p+'_'+s).value)||0,days:parseFloat($('psd'+p+'_'+s).value)||0});}
fetch('/profile',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('ppm'+p,d.msg,d.status=='ok');dirty=false;setTimeout(loadProfiles,500)})
.catch(function(e){msg('ppm'+p,'Error: '+e,false)})}

// ---- PROBES TAB ----
var fnNames={2:'Beer',3:'Ambient',99:'Unassigned'};
function fnOpts(sel){var h='';for(var k in fnNames)h+='<option value="'+k+'"'+(k==sel?' selected':'')+'>'+fnNames[k]+'</option>';return h}
function fermOpts(sel){var h='<option value="99"'+(sel==99?' selected':'')+'>None</option>';for(var i=0;i<4;i++)h+='<option value="'+i+'"'+(sel==i?' selected':'')+'>Fermenter '+i+'</option>';return h}

function loadProbes(){
fetch('/probes').then(r=>r.json()).then(function(d){
var p=d.probes;
var h='<div class="card"><table class="tbl"><tr><th>Address</th><th>Temp</th><th>Name</th><th>Function</th><th>Fermenter</th><th>Adjust</th><th></th></tr>';
if(p.length==0)h+='<tr><td colspan="7" style="color:#888">No probes detected. Connect DS18B20 probes to the Green Jack.</td></tr>';
for(var i=0;i<p.length;i++){var q=p[i];
h+='<tr><td style="font-family:monospace;font-size:12px">'+q.address+'</td>';
h+='<td>'+(q.temperature>-100?q.temperature.toFixed(1)+'&deg;':'<span style="color:#f44">--</span>')+'</td>';
h+='<td><input type="text" id="pn'+q.index+'" value="'+q.name+'" style="width:100px"></td>';
h+='<td><select id="pf'+q.index+'">'+fnOpts(q.function)+'</select></td>';
h+='<td><select id="pr'+q.index+'">'+fermOpts(q.fermenter)+'</select></td>';
h+='<td><input type="number" step="0.1" id="pa'+q.index+'" value="'+q.tempAdjust+'" style="width:60px"></td>';
h+='<td><button class="save" onclick="saveProbe('+q.index+')">Save</button></td></tr>';
}
h+='</table><div class="msg" id="pm"></div></div>';
$('t2').innerHTML=h;
})}

function saveProbe(i){
var body={};
body['index']=i;
body['name']=$('pn'+i).value;
body['function']=parseInt($('pf'+i).value);
body['fermenter']=parseInt($('pr'+i).value);
body['tempAdjust']=parseFloat($('pa'+i).value);
fetch('/probes',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('pm',d.msg,d.status=='ok');dirty=false;setTimeout(loadProbes,500)})
.catch(function(e){msg('pm','Error: '+e,false)})}

// ---- SMART PLUGS TAB ----
var plugFnNames={0:'F1 Hot',1:'F1 Cold',2:'F2 Hot',3:'F2 Cold',4:'F3 Hot',5:'F3 Cold',6:'F4 Hot',7:'F4 Cold',8:'Aux 1',9:'Aux 2',99:'Unassigned'};
function plugFnOpts(sel){var h='';for(var k in plugFnNames)h+='<option value="'+k+'"'+(k==sel?' selected':'')+'>'+plugFnNames[k]+'</option>';return h}

// Preset codes: [btn1_on, btn1_off, btn2_on, btn2_off, btn3_on, btn3_off, btn4_on, btn4_off]
// Firmware array order is (on,off) pairs, buttons [3,4,1,2]. Corrected here to [1,2,3,4].
// Dial codesets 1-4 confirmed via RF sniffer. Others are estimates — verify with /rf/sniff if unsure.
var P=[
{n:'Dial Codeset 1',m:'Dial',d:410,c:[1381717,1381716,1394005,1394004,1397077,1397076,1397845,1397844]},
{n:'Dial Codeset 2',m:'Dial',d:410,c:[4527445,4527444,4539733,4539732,4542805,4542804,4543573,4543572]},
{n:'Dial Codeset 3',m:'Dial',d:410,c:[5313877,5313876,5326165,5326164,5329237,5329236,5330005,5330004]},
{n:'Dial Codeset 4',m:'Dial',d:410,c:[5510485,5510484,5522773,5522772,5525845,5525844,5526613,5526612]},
{n:'Codeset 3a',m:'Dial',d:410,c:[10958860,10958852,10958858,10958850,10958857,10958849,10958861,10958853]},
{n:'Codeset 2a',m:'Dial',d:410,c:[1070387,1070396,1070531,1070540,1070851,1070860,1072387,1072396]},
{n:'Codeset 4 alt',m:'Dial',d:410,c:[12068364,12068356,12068362,12068354,12068363,12068355,12068365,12068357]},
{n:'ZAP USA 0308',m:'ZAP',d:160,c:[4281651,4281660,4282243,4282252,4281795,4281804,4282115,4282124]},
{n:'ZAP USA 0313',m:'ZAP',d:160,c:[1332531,1332540,1332675,1332684,1332995,1333004,1334531,1334540]},
{n:'Arlec Code 1',m:'Arlec',d:160,c:[4461875,4461884,4462019,4462028,4462339,4462348,4463875,4463884]},
{n:'Arlec Code 2',m:'Arlec',d:160,c:[5526835,5526844,5526979,5526988,5527299,5527308,5528835,5528844]},
{n:'Arlec Code 3',m:'Arlec',d:160,c:[16468316,15835788,16028133,16395413,16148014,16569774,15742919,16710407]}
];

function presetOpts(){var h='<option value="">-- Select Preset --</option>';for(var i=0;i<P.length;i++)h+='<option value="'+i+'">'+P[i].n+'</option>';return h}
function btnOpts(){return'<option value="0">Button 1</option><option value="1">Button 2</option><option value="2">Button 3</option><option value="3">Button 4</option>'}

function applyPreset(i){
var ps=$('sps'+i);var bs=$('sbs'+i);
if(ps.value==='')return;
var p=P[parseInt(ps.value)];var b=parseInt(bs.value);
$('sm'+i).value=p.m;
$('smo'+i).value=p.n;
$('son'+i).value=p.c[b*2];
$('sof'+i).value=p.c[b*2+1];
$('spr'+i).value=1;
$('sbi'+i).value=24;
$('sdl'+i).value=p.d;
msg('sm'+i+'m','Preset applied - click Save to store',true)}

function loadPlugs(){
fetch('/smartplugs').then(function(r){return r.json()}).then(function(d){
var p=d.plugs||[];
var h='';
for(var i=0;i<p.length;i++){var q=p[i];
var active=q.onCode>0||q.offCode>0;
var hdr='Plug '+q.index;
if(active)hdr+=' <span style="color:#53d8fb;font-size:12px">'+q.manufacturer+(q.model?' - '+q.model:'')+'</span>';
if(q.state)hdr+=' <span class="badge badge-heat">ON</span>';
h+='<div class="card"><h3>'+hdr+'</h3>';
h+='<div class="row" style="background:#0a1628;padding:6px;border-radius:4px;margin-bottom:8px"><label>Preset</label><select id="sps'+i+'" style="width:160px">'+presetOpts()+'</select>';
h+=' <select id="sbs'+i+'" style="width:100px">'+btnOpts()+'</select>';
h+=' <button class="test" onclick="applyPreset('+i+')">Apply</button></div>';
h+='<div class="row"><label>Manufacturer</label><input type="text" id="sm'+i+'" value="'+q.manufacturer+'" style="width:120px">';
h+=' <label style="min-width:auto">Model</label><input type="text" id="smo'+i+'" value="'+q.model+'" style="width:120px"></div>';
h+='<div class="row"><label>On Code</label><input type="number" id="son'+i+'" value="'+q.onCode+'" style="width:120px">';
h+=' <label style="min-width:auto">Off Code</label><input type="number" id="sof'+i+'" value="'+q.offCode+'" style="width:120px"></div>';
h+='<div class="row"><label>Protocol</label><input type="number" id="spr'+i+'" value="'+q.protocol+'" style="width:60px">';
h+=' <label style="min-width:auto">Bits</label><input type="number" id="sbi'+i+'" value="'+q.bits+'" style="width:60px">';
h+=' <label style="min-width:auto">Delay &mu;s</label><input type="number" id="sdl'+i+'" value="'+q.delay+'" style="width:80px"></div>';
h+='<div class="row"><label>Function</label><select id="sf'+i+'">'+plugFnOpts(q['function'])+'</select>';
h+=' <label style="min-width:auto">Fermenter</label><select id="sr'+i+'">'+fermOpts(q.fermenter)+'</select></div>';
h+='<div class="row">';
h+='<button class="save" onclick="savePlug('+i+')">Save</button> ';
h+='<button class="test" onclick="testPlug('+i+',\'on\')">Test On</button> ';
h+='<button class="test" onclick="testPlug('+i+',\'off\')">Test Off</button>';
h+='</div><div class="msg" id="sm'+i+'m"></div>';
h+='</div>';
}
$('t5').innerHTML=h;
})}

function savePlug(i){
var body={};
body['index']=i;
body['manufacturer']=$('sm'+i).value;
body['model']=$('smo'+i).value;
body['onCode']=parseInt($('son'+i).value)||0;
body['offCode']=parseInt($('sof'+i).value)||0;
body['protocol']=parseInt($('spr'+i).value)||1;
body['bits']=parseInt($('sbi'+i).value)||24;
body['delay']=parseInt($('sdl'+i).value)||160;
body['function']=parseInt($('sf'+i).value);
body['fermenter']=parseInt($('sr'+i).value);
fetch('/smartplug',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('sm'+i+'m',d.msg,d.status=='ok');dirty=false;setTimeout(loadPlugs,500)})
.catch(function(e){msg('sm'+i+'m','Error: '+e,false)})}

function testPlug(i,a){
fetch('/smartplug/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:i,action:a})})
.then(function(r){return r.json()}).then(function(d){msg('sm'+i+'m',d.msg,d.status=='ok')})
.catch(function(e){msg('sm'+i+'m','Error: '+e,false)})}

// ---- SETTINGS TAB ----
var bsNames=["Brewer's Friend",'Brewfather'];
var bsIdLabel=["API Key",'Stream ID'];

function fmtUptime(m){var h=Math.floor(m/60);var mn=m%60;if(h>0)return h+'h '+mn+'m';return mn+'m'}

function loadReporting(){
Promise.all([fetch('/brewservices').then(function(r){return r.json()}),fetch('/mqtt').then(function(r){return r.json()})]).then(function(res){
var svcs=res[0].services||[],mq=res[1];
var h='';
for(var s=0;s<svcs.length;s++){var sv=svcs[s];
h+='<div class="card"><h3>'+sv.name+'</h3>';
h+='<div class="row"><label>Enabled</label>'+sw('sven'+s,sv.enabled)+'</div>';
h+='<div class="row"><label>Device Name</label><input type="text" id="svn'+s+'" value="'+(sv.deviceName||'OurBrewbot')+'" style="width:180px"></div>';
h+='<div class="row"><label>'+bsIdLabel[s]+'</label><input type="text" id="svi'+s+'" value="'+(sv.serviceId||'')+'" style="width:260px"></div>';
h+='<button class="save" onclick="saveSvc('+s+')">Save</button> ';
h+='<button class="test" onclick="testSvc('+s+')">Test</button> ';
h+='<span class="msg" id="svm'+s+'"></span>';
h+='</div>';}
h+='<div class="card"><h3>MQTT</h3>';
h+='<div class="row"><label>Enabled</label>'+sw('mqen',mq.enabled)+'</div>';
h+='<div class="row"><label>Broker Host</label><input type="text" id="mqhost" value="'+(mq.host||'')+'" style="width:220px"></div>';
h+='<div class="row"><label>Port</label><input type="number" id="mqport" value="'+(mq.port||1883)+'" style="width:80px"></div>';
h+='<div class="row"><label>Username</label><input type="text" id="mquser" value="'+(mq.username||'')+'" style="width:180px"></div>';
h+='<div class="row"><label>Password</label><input type="password" id="mqpass" value="'+(mq.password||'')+'" style="width:180px"></div>';
h+='<div class="row"><label>Base Topic</label><input type="text" id="mqtopic" value="'+(mq.baseTopic||'ourbrewbot')+'" style="width:180px"></div>';
h+='<div class="row"><label>HA Discovery</label>'+sw('mqha',mq.haDiscovery||false)+'</div>';
h+='<div class="row"><label>Allow HA Control</label>'+sw('mqctl',mq.allowControl||false)+'</div>';
h+='<button class="save" onclick="saveMqtt()">Save</button> ';
h+='<button class="test" onclick="testMqtt()">Test</button> ';
h+='<button class="test" onclick="discoverMqtt()">Discover</button> ';
h+='<span class="msg" id="mqm"></span>';
h+='</div>';
$('t6').innerHTML=h;
})}

function loadSystemSettings(){
markDirty();
Promise.all([fetch('/controller').then(function(r){return r.json()}),fetch('/fs/files').then(function(r){return r.json()}),fetch('/syslog').then(function(r){return r.json()})]).then(function(res){
var d=res[0],fs=res[1],sl=res[2];
var syslogFacilities=['0 Kernel','1 User','2 Mail','3 Daemon','4 Auth','5 Syslog','6 LPR','7 News','8 UUCP','9 Cron','10 Security','11 FTP','12 NTP','13 Audit','14 Alert','15 Clock','16 Local0','17 Local1','18 Local2','19 Local3','20 Local4','21 Local5','22 Local6','23 Local7'];
var syslogLevels=['0 Emergency','1 Alert','2 Critical','3 Error','4 Warning','5 Notice','6 Info','7 Debug'];
var h='<div class="card"><h3>Global Settings</h3>';
h+='<div class="row"><label>Temp Unit</label><select id="su"><option value="1"'+(d.Unit==1?' selected':'')+'>Celsius</option><option value="2"'+(d.Unit==2?' selected':'')+'>Fahrenheit</option></select></div>';
h+='<div class="row"><label>Resolution</label><select id="sres">';
for(var r=9;r<=12;r++)h+='<option value="'+r+'"'+(d.Resolution==r?' selected':'')+'>'+r+'-bit</option>';
h+='</select></div>';
h+='<button class="save" onclick="saveSettings()">Save</button> <span class="msg" id="setm"></span>';
h+='</div>';
h+='<div class="card"><h3>Syslog</h3>';
h+='<div class="row"><label>Enabled</label>'+sw('slen',sl.enabled||false)+'</div>';
h+='<div class="row"><label>Host</label><input type="text" id="slhost" value="'+(sl.host||'')+'" style="width:220px"></div>';
h+='<div class="row"><label>Port</label><input type="number" id="slport" value="'+(sl.port||514)+'" style="width:80px"></div>';
h+='<div class="row"><label>Facility</label><select id="slfac">';
for(var fi=0;fi<syslogFacilities.length;fi++)h+='<option value="'+fi+'"'+(fi==(sl.facility!=null?sl.facility:16)?' selected':'')+'>'+syslogFacilities[fi]+'</option>';
h+='</select></div>';
h+='<div class="row"><label>Min Log Level</label><select id="sllvl">';
for(var li=0;li<syslogLevels.length;li++)h+='<option value="'+li+'"'+(li==(sl.minLevel!=null?sl.minLevel:7)?' selected':'')+'>'+syslogLevels[li]+'</option>';
h+='</select></div>';
h+='<button class="save" onclick="saveSyslog()">Save</button> <span class="msg" id="slm"></span>';
h+='</div>';
h+='<div class="info"><h3 style="color:#e94560;margin-bottom:8px">System Info</h3>';
h+='<div class="r"><span>Firmware</span><span class="v">'+d.FirmwareVersion+'</span></div>';
h+='<div class="r"><span>IP Address</span><span class="v">'+d.IP+'</span></div>';
h+='<div class="r"><span>mDNS Name</span><span class="v"><a href="http://'+d.mDNSName+'/" style="color:#53d8fb">'+d.mDNSName+'</a></span></div>';
h+='<div class="r"><span>WiFi SSID</span><span class="v">'+d.WiFiSSID+'</span></div>';
h+='<div class="r"><span>RSSI</span><span class="v">'+d.RSSI+' dBm</span></div>';
h+='<div class="r"><span>Free Heap</span><span class="v">'+d.FreeHeap+' bytes</span></div>';
h+='<div class="r"><span>Uptime</span><span class="v">'+fmtUptime(d.Uptime)+'</span></div>';
h+='<div class="r"><span>Chip ID</span><span class="v">'+d.ChipId+'</span></div>';
h+='</div>';
h+='<div class="card"><h3>Actions</h3>';
h+='<button class="danger" onclick="if(confirm(\'Reboot device?\'))fetch(\'/reboot\').then(function(){alert(\'Rebooting...\')})">Reboot</button>';
h+='<button class="danger" onclick="if(confirm(\'Reset ALL configuration to defaults?\'))fetch(\'/reset?target=config\').then(function(){alert(\'Resetting...\')})">Factory Reset</button>';
h+='<button class="danger" onclick="window.location.href=\'/update\'">Firmware Update</button>';
h+='<button class="danger" onclick="resetWiFiSettings()">Reset WiFi Settings</button>';
h+='<button class="danger" onclick="window.location.href=\'/rf/sniff\'">RF Sniffer</button>';
h+='<button class="danger" onclick="window.location.href=\'/ble/sniff\'">BT Sniffer</button>';
h+='</div>';
var files=fs.files||[];
h+='<div class="card"><h3>LittleFS Files</h3>';
if(files.length==0){h+='<p style="color:#888;font-size:13px">No files found.</p>';}
else{
h+='<div style="max-height:200px;overflow-y:auto"><table class="tbl"><tr><th>File</th><th>Size</th></tr>';
for(var i=0;i<files.length;i++){var f=files[i];
h+='<tr style="cursor:pointer" onclick="loadFileContent(\''+f.name+'\')" id="sfr'+i+'">';
h+='<td style="font-family:monospace">'+f.name+'</td><td>'+f.size+' B</td></tr>';}
h+='</table></div>';}
h+='</div>';
h+='<div class="card" id="sysfc_card" style="display:none">';
h+='<h3 id="sysfc_title"></h3>';
h+='<textarea id="sysfc" readonly style="width:100%;height:140px;background:#0a1628;border:1px solid #333;color:#e0e0e0;font-family:monospace;font-size:12px;padding:6px;border-radius:3px;resize:vertical"></textarea>';
h+='<div style="margin-top:6px"><button class="save" onclick="downloadFile()">Download</button></div>';
h+='</div>';
$('t7').innerHTML=h;
}).catch(function(e){$('t7').innerHTML='<div class="card"><p style="color:#f44">Error: '+e+'</p></div>'})}

function saveSettings(){
var body={Unit:parseInt($('su').value),
Resolution:parseInt($('sres').value)};
fetch('/controller',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('setm',d.msg,d.status=='ok');dirty=false})
.catch(function(e){msg('setm','Error: '+e,false)})}

function saveSvc(s){
var body={index:s,enabled:$('sven'+s).checked,serviceId:$('svi'+s).value,deviceName:$('svn'+s).value};
fetch('/brewservices',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('svm'+s,d.msg,d.status=='ok');dirty=false})
.catch(function(e){msg('svm'+s,'Error: '+e,false)})}

function testSvc(s){
msg('svm'+s,'Testing...',true);
fetch('/brewservices/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:s})})
.then(function(r){return r.json()}).then(function(d){msg('svm'+s,d.msg,d.status=='ok')})
.catch(function(e){msg('svm'+s,'Error: '+e,false)})}

function saveMqtt(){
var body={enabled:$('mqen').checked,host:$('mqhost').value,port:parseInt($('mqport').value),username:$('mquser').value,password:$('mqpass').value,baseTopic:$('mqtopic').value,haDiscovery:$('mqha').checked,allowControl:$('mqctl').checked};
fetch('/mqtt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('mqm',d.msg,d.status=='ok');dirty=false})
.catch(function(e){msg('mqm','Error: '+e,false)})}

function saveSyslog(){
var body={enabled:$('slen').checked,host:$('slhost').value,port:parseInt($('slport').value),facility:parseInt($('slfac').value),minLevel:parseInt($('sllvl').value)};
fetch('/syslog',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('slm',d.msg,d.status=='ok');dirty=false})
.catch(function(e){msg('slm','Error: '+e,false)})}

function testMqtt(){
msg('mqm','Testing...',true);
fetch('/mqtt/test',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})
.then(function(r){return r.json()}).then(function(d){msg('mqm',d.msg,d.status=='ok')})
.catch(function(e){msg('mqm','Error: '+e,false)})}

function discoverMqtt(){
msg('mqm','Publishing discovery...',true);
fetch('/mqtt/discover',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})
.then(function(r){return r.json()}).then(function(d){msg('mqm',d.msg,d.status=='ok')})
.catch(function(e){msg('mqm','Error: '+e,false)})}

// ---- TILTS TAB ----
var tiltColourNames=['Red','Green','Black','Purple','Orange','Blue','Yellow','Pink'];
var tiltDotColours=['#e94560','#4caf50','#444','#9c27b0','#ff9800','#2196f3','#ffeb3b','#e91e63'];
var tiltFnNames={2:'Beer',99:'Not used'};
function tiltFnOpts(sel){var s=(sel==2)?2:99;var h='';for(var k in tiltFnNames)h+='<option value="'+k+'"'+(k==s?' selected':'')+'>'+tiltFnNames[k]+'</option>';return h}

function loadTilts(){
fetch('/tilts').then(function(r){return r.json()}).then(function(d){
var ts=d.tilts||[];
var h='';
if(ts.length==0){
h='<div class="card"><p style="color:#888">No Tilts detected or configured.<br>Tilts are discovered automatically via BLE scanning every 30 seconds.<br>Configure a Tilt slot below by colour to set fermenter assignment and calibration offsets.</p>';
h+='<p style="color:#888;font-size:12px;margin-top:8px">To pre-configure a Tilt before it is seen, select its colour and click Save.</p></div>';
// Show all 8 colours as unconfigured
for(var c=0;c<8;c++){h+=buildTiltCard(c,null);}
}else{
// Show configured/seen tilts
var seen={};for(var i=0;i<ts.length;i++)seen[ts[i].colour]=ts[i];
for(var c=0;c<8;c++){h+=buildTiltCard(c,seen[c]||null);}
}
$('t3').innerHTML=h;
}).catch(function(e){$('t3').innerHTML='<div class="card"><p style="color:#f44">Error loading Tilt data: '+e+'</p></div>'})}

function buildTiltCard(colour,t){
var dot='<span style="display:inline-block;width:12px;height:12px;border-radius:50%;background:'+tiltDotColours[colour]+';margin-right:6px;vertical-align:middle"></span>';
var active=t&&t.active;
var label=(t&&t.isPro)?'Tilt Pro':'Tilt';
var h='<div class="card"><h3>'+dot+tiltColourNames[colour]+' '+label;
if(active)h+=' <span class="badge badge-idle">Active</span>';
if(t&&t.isPro)h+=' <span class="badge" style="background:#ff9800;color:#000">Pro</span>';
h+='</h3>';
if(active&&t){h+='<div class="live">SG: '+(t.sg>0?t.sg.toFixed(4):'--')+' &nbsp; Temp: '+(t.temperature>-100?t.temperature.toFixed(1)+'&deg;':'--')+'</div>';}
var fn=t?t.function:99;var ferm=t?t.fermenter:99;var ta=t?t.tempAdjust:0;var sa=t?t.sgAdjust:0;
h+='<div class="row"><label>Fermenter</label><select id="tlr'+colour+'">'+fermOpts(ferm)+'</select></div>';
h+='<div class="row"><label>SG Adjust</label><input type="number" step="0.0001" id="tlsa'+colour+'" value="'+sa+'" style="width:80px"></div>';
h+='<div class="row"><label>Temp Adjust</label><input type="number" step="0.1" id="tlta'+colour+'" value="'+ta+'" style="width:70px"> &deg;C</div>';
h+='<div class="row"><label>Temperature reading</label><select id="tlf'+colour+'">'+tiltFnOpts(fn)+'</select></div>';
h+='<button class="save" onclick="saveTilt('+colour+')">Save</button>';
if(t)h+=' <button class="test" onclick="clearTilt('+colour+')" style="background:#333">Clear</button>';
h+=' <span class="msg" id="tlm'+colour+'"></span></div>';
return h}

function saveTilt(colour){
var body={colour:colour,function:parseInt($('tlf'+colour).value),fermenter:parseInt($('tlr'+colour).value),tempAdjust:parseFloat($('tlta'+colour).value)||0,sgAdjust:parseFloat($('tlsa'+colour).value)||0};
fetch('/tilt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('tlm'+colour,d.msg,d.status=='ok');dirty=false;if(d.status=='ok')setTimeout(loadTilts,500)})
.catch(function(e){msg('tlm'+colour,'Error: '+e,false)})}

function clearTilt(colour){
if(!confirm('Remove '+tiltColourNames[colour]+' Tilt configuration?'))return;
// Reset to unassigned
var body={colour:colour,function:99,fermenter:99,tempAdjust:0,sgAdjust:0,_clear:true};
fetch('/tilt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('tlm'+colour,d.msg,d.status=='ok');dirty=false;if(d.status=='ok')setTimeout(loadTilts,500)})
.catch(function(e){msg('tlm'+colour,'Error: '+e,false)})}

// ---- iSpindels Tab ----
var iSpindelUnitNames={0:'SG',1:'Plato'};
function loadISpindels(){
fetch('/ispindels').then(function(r){return r.json()}).then(function(d){
var ds=d.ispindels||[];
var h='';
if(ds.length==0){h='<div class="card"><p style="color:#888">No iSpindel slots configured.</p></div>';}
for(var i=0;i<ds.length;i++){h+=buildISpindelCard(i,ds[i]);}
$('t4').innerHTML=h;
}).catch(function(e){$('t4').innerHTML='<div class="card"><p style="color:#f44">Error loading iSpindel data: '+e+'</p></div>'})}

function buildISpindelCard(idx,s){
var empty=(s.name=='None'||s.name=='');
var hasData=(s.sg>0||s.temperature>0);
var h='<div class="card"><h3>Slot '+idx;
if(!empty)h+=': '+s.name;
if(hasData&&!empty)h+=' <span class="badge badge-idle">Active</span>';
h+='</h3>';
if(!empty){
h+='<div class="live">';
if(hasData){h+='SG: '+s.sg.toFixed(4)+' &nbsp; Temp: '+s.temperature.toFixed(1)+'&deg; &nbsp; Angle: '+s.angle.toFixed(1)+'&deg; &nbsp; Batt: '+s.battery.toFixed(2)+'V &nbsp; RSSI: '+s.rssi+'dBm';if(s.corrGravity>0||s.velocity>0){h+='<br>Corr.SG: '+s.corrGravity.toFixed(4)+' &nbsp; Velocity: '+s.velocity.toFixed(4)+' &nbsp; Cycle: '+s.runTime.toFixed(1)+'s'+(s.gravityUnit?' &nbsp; Unit: '+s.gravityUnit:'');}}
h+='</div>';
h+='<div class="row"><label>Device ID</label><span style="color:#53d8fb;font-size:13px">'+(s.id||'—')+'</span></div>';
}
h+='<div class="row"><label>Fermenter</label><select id="isf'+idx+'">'+fermOpts(s.fermenter)+'</select></div>';
h+='<div class="row"><label>Unit</label><select id="isu'+idx+'"><option value="0"'+(s.unit==0?' selected':'')+'>SG</option><option value="1"'+(s.unit==1?' selected':'')+'>Plato</option></select></div>';
h+='<div class="row"><label>Temperature reading</label><select id="isfn'+idx+'">'+tiltFnOpts(s.function)+'</select></div>';
h+='<button class="save" onclick="saveISpindel('+idx+')">Save</button>';
if(!empty)h+=' <button class="test" onclick="clearISpindel('+idx+')" style="background:#333">Clear</button>';
h+=' <span class="msg" id="ism'+idx+'"></span></div>';
return h}

function saveISpindel(idx){
var body={index:idx,fermenter:parseInt($('isf'+idx).value),unit:parseInt($('isu'+idx).value),function:parseInt($('isfn'+idx).value)};
fetch('/ispindel/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('ism'+idx,d.msg,d.status=='ok');dirty=false;if(d.status=='ok')setTimeout(loadISpindels,500)})
.catch(function(e){msg('ism'+idx,'Error: '+e,false)})}

function clearISpindel(idx){
if(!confirm('Reset iSpindel slot '+idx+'?'))return;
var body={index:idx,_clear:true};
fetch('/ispindel/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(function(r){return r.json()}).then(function(d){msg('ism'+idx,d.msg,d.status=='ok');dirty=false;if(d.status=='ok')setTimeout(loadISpindels,500)})
.catch(function(e){msg('ism'+idx,'Error: '+e,false)})}

var sysFn='';

function loadFileContent(name){
markDirty();sysFn=name;
var card=$('sysfc_card');var ta=$('sysfc');var title=$('sysfc_title');
if(card)card.style.display='none';
if(ta)ta.value='Loading...';
fetch('/fs/file?name='+encodeURIComponent(name)).then(function(r){
if(!r.ok)throw new Error('HTTP '+r.status);
return r.text();
}).then(function(text){
if(card)card.style.display='';
if(title)title.textContent=name;
if(ta)ta.value=text;
markDirty();
}).catch(function(e){if(ta)ta.value='Error: '+e;if(card)card.style.display=''})}

function downloadFile(){
var ta=$('sysfc');
if(!ta||!sysFn)return;
var blob=new Blob([ta.value],{type:'text/plain'});
var url=URL.createObjectURL(blob);
var a=document.createElement('a');
var base=sysFn.replace(/.*\//,'');
a.href=url;a.download=base||'file.txt';
document.body.appendChild(a);a.click();
document.body.removeChild(a);URL.revokeObjectURL(url)}

// ---- AUTO REFRESH ----
function startRefresh(){if(R)clearInterval(R);R=setInterval(function(){if(!dirty)loadTab()},10000)}
loadTab();startRefresh();
document.body.addEventListener('focusin',function(e){if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT')markDirty()});
document.body.addEventListener('input',function(){markDirty()});
</script>
</body>
</html>)rawliteral";

// ============================================================
// ADMIN PAGE HANDLER
// ============================================================

void handleAdmin(ESP8266WebServer& server) {
  server.setContentLength(strlen_P(ADMIN_PAGE));
  server.send(200, "text/html", "");
  server.sendContent_P(ADMIN_PAGE);
}
