'use strict';

function profileValues(p) { return JSON.stringify({name:p.name,steps:p.steps}); }

function updateDirty(form) {
  const key=form.dataset.form;
  const changed=key.startsWith('profile-')
    ? profileValues(captureProfile())!==profileValues(state.profiles[+key.split('-')[1]])
    : JSON.stringify(formData(form))!==JSON.stringify(formBaselines.get(form));
  const label=$('.saved-label',form);
  if (label) { label.textContent=changed?'Unsaved changes':'No unsaved changes'; label.classList.toggle('dirty',changed); }
  const save=$('[data-save]',form);
  if (save) save.disabled=!changed || saving;
  form.dataset.dirty=String(changed);
  if (changed) drafts.set(key,formData(form)); else drafts.delete(key);
  if (unknownForms.has(key)) form.dataset.unknown='true';
}

function markProfileDraft() {
  const form=$('form[data-form^="profile-"]');
  if (form) updateDirty(form);
}

function profilePayload(index, profile) {
  const display=value=>state.unit==='F'?value*1.8+32:value;
  const steps=profile.steps.map(step=>({stepType:step.type,startTemp:display(step.start),
    endTemp:display(step.end),sgTrigger:step.trigger,days:step.days}));
  while (steps.length<15) steps.push({stepType:0,startTemp:0,endTemp:0,sgTrigger:0,days:0});
  return {index,name:profile.name,steps};
}

// Labels may be rounded for readability. Send the received number unchanged
// unless the user edited that field; a name-only save must not alter control.
function savedNumber(form,data,name,received) {
  const baseline=formBaselines.get(form);
  return baseline && data[name]===baseline[name] ? received : +data[name];
}

function fermenterPayload(form,data,index) {
  const fermenter=state.fermenters[index],raw=fermenter.raw;
  const body={Fermenter:fermenter.id,FermenterName:data.name,BeerName:data.beer,YeastName:data.yeast,
    OG:savedNumber(form,data,'og',raw.OG),TG:savedNumber(form,data,'tg',raw.TG),
    CeilingTemp:savedNumber(form,data,'ceiling',raw.CeilingTemp),
    FloorTemp:savedNumber(form,data,'floor',raw.FloorTemp),
    Hysteresis:savedNumber(form,data,'hysteresis',raw.Hysteresis),
    AlarmTolerance:savedNumber(form,data,'alarm',raw.AlarmTolerance),
    CompressorDelay:+data.delay,Power:data.power,TempControl:data.tempControl,
    ProfileNo:+data.profile,LiveTest:data.liveTest};
  // Config.h defines HTTP reporting at bits 0/1 and MQTT at bit 3.
  // Preserve any bits outside the three checkboxes instead of erasing them.
  const reportingBits=1|2|8;
  body.BrewServices=(raw.BrewServices&~reportingBits)|
    (data.brewers?1:0)|(data.brewfather?2:0)|(data.mqtt?8:0);

  // Match Config.h::checkTempBand() and the alarm bounds in WebAPI.cpp.
  const ceiling=toC(body.CeilingTemp),floor=toC(body.FloorTemp),hysteresis=deltaC(body.Hysteresis);
  if (deltaC(body.AlarmTolerance)<0 || deltaC(body.AlarmTolerance)>10) {
    throw new Error('Alarm tolerance must be 0–10 °C (0–18 °F).');
  }
  if (floor < -20 || ceiling>50 || floor>=ceiling || hysteresis<0 || hysteresis>10 || ceiling-floor+1e-6<2*hysteresis) {
    throw new Error('Temperature band must be within −20…50 °C, floor below ceiling, and at least twice hysteresis (0…10 °C).');
  }
  return body;
}

function sensorPayload(form,data,kind,index) {
  const sensors=kind==='probe'?state.probes:kind==='tilt'?state.tilts:state.ispindels;
  const sensor=sensors[index];
  const body={function:+data.fn,fermenter:+data.fermenter,
    tempAdjust:savedNumber(form,data,'adjust',finite(sensor.tempAdjust))};
  if (kind==='tilt') body.colour=index;
  else body.index=sensor.index;
  if (kind!=='probe') body.sgAdjust=savedNumber(form,data,'sgAdjust',sensor.sgAdjust);
  if (kind==='ispindel') body.unit=+data.unit;
  return body;
}

function plugPayload(data,index) {
  const role=data.role,fermenter=+data.fermenter;
  if ((role==='hot'||role==='cold') && fermenter===99) throw new Error('Choose a fermenter for this heater or cooler.');
  let plugFunction=99;
  if (role==='hot') plugFunction=fermenter*2;
  else if (role==='cold') plugFunction=fermenter*2+1;
  else if (role==='aux1') plugFunction=8;
  else if (role==='aux2') plugFunction=9;

  // Config.h::checkRfSettings() mirrors rc-switch's twelve protocols:
  // worst-case bit pulses, sync pulses, ten repeats, maximum burst 2000 ms.
  const pulses=[[4,32],[3,11],[15,101],[4,7],[3,20],[3,24],[7,64],[23,133],[23,137],[4,19],[3,37],[3,37]][+data.protocol-1];
  if (!pulses || Math.floor(10*(+data.bits*pulses[0]+pulses[1])*(+data.delay)/1000)>2000) {
    throw new Error('These RF timings would make a send exceed the firmware limit of 2 seconds.');
  }
  return {index:state.plugs[index].index,manufacturer:data.manufacturer,model:data.model,
    onCode:+data.onCode,offCode:+data.offCode,protocol:+data.protocol,bits:+data.bits,delay:+data.delay,
    function:plugFunction,fermenter};
}

function mqttPayload(data) {
  // TextSafe.cpp::isValidBaseTopic() rejects wildcards, controls and a leading $.
  if (!data.baseTopic || /^[\$]|[+#\x00-\x1f\x7f]/.test(data.baseTopic)) {
    throw new Error('Base topic must be 1–31 bytes, without wildcards or control characters, and must not start with $.');
  }
  const body={enabled:data.enabled,host:data.host,port:+data.port,username:data.username,
    baseTopic:data.baseTopic,haDiscovery:data.haDiscovery,allowControl:data.allowControl,logEnabled:data.logEnabled};
  if (data.clearPassword) body.password='';
  else if (data.password) body.password=data.password;
  return body;
}

async function saveForm(form) {
  if (form.dataset.checking) return;
  form.dataset.checking='true';
  try { await saveFormChecked(form); }
  finally { delete form.dataset.checking; }
}

async function saveFormChecked(form) {
  if (saving || form.dataset.dirty!=='true' || !form.reportValidity()) return;
  if (state.unitChanged) { saveError(form,'Controller units changed. Discard all drafts and refresh before saving.'); return; }
  if (form.dataset.unknown) {
    saveError(form,'Read the current controller state using Refresh before explicitly saving again.'); return;
  }
  if (['ferm','profile','probe','tilt','ispindel'].includes(form.dataset.form.split('-')[0])) {
    await fetchData('controller');
    if (saving) return;
    if (state.unitChanged || (form.dataset.unit && form.dataset.unit!==state.unit)) { saveError(form,'Controller units changed. Discard and review drafts before saving.'); return; }
  }
  for (const el of $$('input[type="number"]',form)) {
    if (!Number.isFinite(Number(el.value))) { saveError(form,'Enter a finite number in every numeric field.'); el.focus(); return; }
  }
  const key=form.dataset.form,data=formData(form),[kind,slot]=key.split('-'),index=+slot;
  let limits={};
  if (kind==='ferm') limits={name:31,beer:31,yeast:31};
  else if (kind==='profile') limits={name:31};
  else if (kind==='plug') limits={manufacturer:23,model:23};
  else if (kind==='service') limits={serviceId:47,deviceName:31};
  else if (key==='mqtt') limits={host:63,username:31,password:47,baseTopic:31};
  else if (key==='syslog') limits={host:63};
  for (const [name,limit] of Object.entries(limits)) {
    if (new TextEncoder().encode(data[name]||'').length>limit) {
      saveError(form,name+' exceeds the controller limit of '+limit+' UTF-8 bytes.'); return;
    }
  }
  let path,body,refresh;
  if (kind==='ferm') {
    path='/fermenter'; refresh='fermenters';
    body=fermenterPayload(form,data,index);
  } else if (kind==='profile') {
    await fetchData('fermenters');
    if (saving) return;
    if (state.unitChanged || form.dataset.unit!==state.unit) { saveError(form,'Controller units changed. Discard and review drafts before saving.'); return; }
    if (state.profiles[index].used) { saveError(form,'This profile is running and cannot be edited.'); return; }
    path='/profile'; refresh='profiles'; body=profilePayload(index,captureProfile());
  } else if (['probe','tilt','ispindel'].includes(kind)) {
    path=kind==='probe'?'/probes':kind==='tilt'?'/tilt':'/ispindel/config';
    refresh=kind==='probe'?'probes':kind==='tilt'?'tilts':'ispindels';
    body=sensorPayload(form,data,kind,index);
  } else if (kind==='plug') {
    path='/smartplug'; refresh='smartplugs';
    body=plugPayload(data,index);
  } else if (kind==='service') {
    path='/brewservices'; refresh='brewservices'; body={index,enabled:data.enabled,serviceId:data.serviceId,deviceName:data.deviceName};
  } else if (key==='mqtt') {
    path='/mqtt'; refresh='mqtt'; body=mqttPayload(data);
  } else if (key==='global') {
    path='/controller'; refresh='controller'; body={Unit:data.unit==='C'?1:2,Resolution:+data.resolution,
      AlarmDwellSec:+data.alarm,MdnsEnabled:data.mdns,UpdateCheck:data.updates,CrashReports:data.crashes};
    if (data.unit!==state.unit && drafts.size>1) { saveError(form,'Save or discard other drafts before changing units.'); return; }
  } else if (key==='debug-mode') {
    path='/debug'; refresh='debug'; body={DebugMode:data.enabled};
  } else if (key==='syslog') {
    path='/syslog'; refresh='syslog'; body={enabled:data.enabled,host:data.host,port:+data.port,facility:+data.facility,minLevel:+data.level};
  } else return;

  saving=true;
  $('[data-save]',form).disabled=true;
  saveError(form,'Saving…');
  try {
    const reply=await post(path,body);
    try { await fetchData(refresh); }
    catch (error) { error.unknown=true; error.message='Save acknowledged, but the confirmation read failed. Refresh before saving again. '+error.message; throw error; }
    drafts.delete(key);
    if (kind==='profile') profileDrafts.delete(index);
    if (kind==='ferm') { $('#editor').close(); state.editorId=null; }
    if (!drafts.size && !profileDrafts.size) state.unitChanged=false;
    render();
    toast(reply.msg || 'Saved.');
  } catch (error) {
    saveError(form,error.message);
    if (error.unknown) { form.dataset.unknown='true'; unknownForms.add(key); }
  } finally {
    saving=false;
    if (form.isConnected) updateDirty(form);
  }
}

const backupFiles=[['global','/jsonGlobal.txt'],['fermenter','/jsonFermenter.txt'],['probe','/jsonProbe.txt'],
  ['smartplugs','/jsonSmartPlugs.txt'],['profile','/jsonProfile.txt'],['profileSteps','/jsonProfileSteps.txt'],
  ['tilt','/jsonTilt.txt'],['ispindel','/jsoniSpindel.txt'],['brewservices','/jsonBrewServices.txt'],
  ['mqtt','/jsonMqtt.txt'],['syslog','/jsonSyslog.txt']];

async function exportConfig() {
  const configs={},failed=[];
  for (const [key,path] of backupFiles) {
    try {
      const text=(await request('/fs/file?name='+encodeURIComponent(path),{text:true})).data;
      configs[key]=JSON.parse(text);
    } catch (error) { failed.push(key+': '+error.message); }
  }
  if (failed.length) { toast('Backup incomplete. No complete backup downloaded. '+failed.join('; ')); return; }
  download('ourbrewbot-config.json',JSON.stringify({timestamp:new Date().toISOString(),configs},null,2));
  toast('Saved configuration backup downloaded.');
}

async function importConfig(file) {
  let bundle;
  try { bundle=JSON.parse(await file.text()); }
  catch { toast('Choose a valid JSON configuration backup.'); return; }
  if (!bundle || !bundle.configs || typeof bundle.configs!=='object' || Array.isArray(bundle.configs)) {
    toast('The backup must contain a configs object.'); return;
  }
  const entries=backupFiles.filter(([key])=>Object.prototype.hasOwnProperty.call(bundle.configs,key));
  if (!entries.length) { toast('No supported configuration files were found.'); return; }
  confirmAction('Import configuration?', 'Import '+entries.length+' saved files from '+file.name+
    '. Existing settings will be replaced and the controller will restart. Unknown fields inside files are preserved.', 'Import', async()=>{
    saving=true;
    try {
      for (const [key,path] of entries) {
        await request('/fs/save?name='+encodeURIComponent(path),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(bundle.configs[key])});
      }
    } catch (error) {
      try { await post('/fs/import/abort',{}); toast('Import failed; rollback requested. '+error.message); }
      catch (rollback) { toast('Import failed. Rollback result requires checking: '+rollback.message); }
      saving=false;
      return;
    }
    try {
      await post('/reboot',{});
      toast('Import finished. Controller restarting; reconnect after it returns.');
    } catch (error) {
      toast('Files imported. Restart result unknown; check the controller before restarting it manually. '+error.message);
    } finally { saving=false; }
  });
}

async function sendBle(command) {
  if (!command || command.length>64) { toast('Enter an AT command of at most 64 characters.'); return; }
  await post('/ble/sniff/send',{cmd:command});
  state.console='ble';
}

function applyPreset(form) {
  const d=formData(form),preset=plugPresets[+d.preset];
  if (d.preset==='' || !preset) return;
  const button=+d.remote;
  for (const [name,value] of Object.entries({manufacturer:preset.m,model:preset.n,
    onCode:preset.c[button*2],offCode:preset.c[button*2+1],protocol:1,bits:24,delay:preset.d})) form.elements[name].value=value;
  updateDirty(form);
  toast('Preset applied to draft. Save to store it.');
}

// A refresh resolves uncertainty only for forms whose endpoint was reread.
async function refreshForms() {
  if (!await loadRoute(true)) return;
  const names=routeData();
  const endpoints={ferm:'fermenters',profile:'profiles',probe:'probes',tilt:'tilts',ispindel:'ispindels',
    plug:'smartplugs',service:'brewservices',global:'controller',mqtt:'mqtt',syslog:'syslog',debug:'debug'};
  for (const key of unknownForms) {
    if (names.includes(endpoints[key.split('-')[0]])) unknownForms.delete(key);
  }
  for (const form of $$('form[data-unknown]')) {
    if (!unknownForms.has(form.dataset.form)) delete form.dataset.unknown;
  }
}

function discardForm(form) {
  const key=form.dataset.form,oldUnit=form.dataset.unit;
  drafts.delete(key);
  if (key.startsWith('profile-')) {
    profileDrafts.delete(state.profile);
    render();
  } else {
    applyData(form,formBaselines.get(form));
    updateDirty(form);
    saveError(form,'');
  }
  if (!drafts.size && !profileDrafts.size) state.unitChanged=false;
  if (oldUnit!==state.unit) {
    render();
    if (key.startsWith('ferm-')) openFerm(+key.split('-')[1]);
  }
}

function chooseProfileCopy() {
  const choices=[[-1,'Choose an editable slot'],
    ...state.profiles.map((profile,index)=>[index,profile.name+(profile.used?' (in use)':'')])];
  showModal('<h2>Copy profile</h2>'+select('Destination slot','copy-slot',-1,choices)+
    `<div class="modal-actions">${btn('Cancel','close-modal','ghost')}${btn('Copy as draft','copy-confirm','')}</div>`);
}

function copyProfile() {
  const destination=+$('[name="copy-slot"]').value;
  if (destination<0 || destination===state.profile || state.profiles[destination].used) {
    toast('Choose a different editable slot.'); return;
  }
  if (profileDrafts.has(destination) || drafts.has('profile-'+destination)) {
    toast('Save or discard the destination draft first.'); return;
  }
  const profile=clone(profileDrafts.get(state.profile)||state.profiles[state.profile]);
  profile.used='';
  profileDrafts.set(destination,profile);
  state.profile=destination;
  $('#modal').close();
  render();
}

function editProfileStep(actionName,index) {
  if (state.profiles[state.profile].used) return;
  const profile=captureProfile(),steps=profile.steps;
  if (actionName==='step-add' && steps.length<15) steps.push({type:1,start:19,end:19,days:1,trigger:0});
  if (actionName==='step-delete') steps.splice(index,1);
  if (actionName==='step-up'&&index>0) [steps[index-1],steps[index]]=[steps[index],steps[index-1]];
  if (actionName==='step-down'&&index<steps.length-1) [steps[index],steps[index+1]]=[steps[index+1],steps[index]];
  profileDrafts.set(state.profile,profile);
  drafts.delete('profile-'+state.profile);
  render();
}

function controlProfile(index,command) {
  const fermenter=state.fermenters[index];
  confirmAction('Change profile state?',command+' for '+fermenter.name+' using the saved profile assignment.', 'Apply',async()=>{
    const body={Fermenter:fermenter.id,action:command};
    if (command==='start') body.ProfileIndex=fermenter.profile;
    await post('/fermenter/profile',body);
    await fetchData('fermenters');
    render();
    openFerm(index);
  });
}

function clearSensor(kind,index) {
  const tilt=kind==='tilts',sensor=tilt?state.tilts[index]:state.ispindels[index];
  confirmAction('Clear sensor slot?','The saved assignment and calibration will be cleared.','Clear',async()=>{
    await post(tilt?'/tilt':'/ispindel/config',tilt?{colour:index,_clear:true}:{index:sensor.index,_clear:true});
    drafts.delete((tilt?'tilt-':'ispindel-')+index);
    await fetchData(tilt?'tilts':'ispindels');
    render();
  });
}

function testPlug(index,command) {
  if (!state.plugs.some(plug=>plug.index===index && (plug.onCode||plug.offCode))) {
    toast('Choose a configured plug.'); return;
  }
  confirmAction('Test plug '+index+' '+command+'?',
    'This transmits the saved code and switches the real device.','Test '+command,async()=>{
      const reply=await post('/smartplug/test',{index,action:command});
      toast(reply.msg);
      await fetchData('smartplugs');
    });
}

async function viewFile(index) {
  const file=state.files[index];
  const text=(await request('/fs/file?name='+encodeURIComponent(file.name),{text:true})).data;
  showModal('<h2>'+esc(file.name)+'</h2><pre class="code-view">'+esc(text)+'</pre>'+
    '<div class="modal-actions">'+btn('Close','close-modal','ghost')+'<button id="download-file" class="button">Download</button></div>');
  $('#download-file').onclick=()=>download(file.name,text,'text/plain');
}

// Keep the small fixed action list here; substantial operations have named helpers above.
async function action(target) {
  const actionName=target.dataset.action,index=+target.dataset.id,value=target.dataset.value;
  if (saving && !['close-modal','close-editor'].includes(actionName)) return;
  if (actionName==='edit-ferm') openFerm(index);
  else if (actionName==='close-editor') { $('#editor').close(); state.editorId=null; }
  else if (actionName==='refresh'||actionName==='reconnect') await refreshForms();
  else if (actionName==='pause') { state.paused=!state.paused; render(); }
  else if (actionName==='discard-form') discardForm(target.closest('form'));
  else if (actionName==='select-profile') { state.profile=index; render(); }
  else if (actionName==='copy-profile') chooseProfileCopy();
  else if (actionName==='copy-confirm') copyProfile();
  else if (actionName.startsWith('step-')) editProfileStep(actionName,index);
  else if (actionName==='profile-control') controlProfile(index,value);
  else if (actionName==='sensor-tab'||actionName==='system-tab') {
    state.console=null;
    if (actionName==='sensor-tab') state.sensorTab=value;
    else state.systemTab=value;
    await loadRoute();
  } else if (actionName==='clear-sensor') clearSensor(target.dataset.kind,index);
  else if (actionName==='plug-preset') applyPreset(target.closest('form'));
  else if (actionName==='plug-test'||actionName==='rf-test') {
    testPlug(actionName==='rf-test'?+$('#rf-plug').value:state.plugs[index].index,value);
  } else if (actionName==='test-service'||actionName==='test-mqtt'||actionName==='discover') {
    let path='/mqtt/discover';
    if (actionName==='test-service') path='/brewservices/test';
    else if (actionName==='test-mqtt') path='/mqtt/test';
    const reply=await post(path,actionName==='test-service'?{index}:{});
    toast(reply.msg);
  } else if (actionName==='open-rf') { state.systemTab='rf'; location.hash='system'; await loadRoute(); }
  else if (actionName==='export') await exportConfig();
  else if (actionName==='import') $('#import-file').click();
  else if (actionName==='file-view') await viewFile(index);
  else if (actionName==='console-start') { state.console=value; lastConsolePoll=0; }
  else if (actionName==='console-stop') state.console=null;
  else if (actionName==='rf-clear') $('#rf-output').textContent='';
  else if (actionName==='ble-clear') $('#ble-output').textContent='';
  else if (actionName==='ble-send') await sendBle(value);
  else if (actionName==='update-check') {
    Object.assign(state.controller,await post('/update/check',{}, {timeout:30000}));
    $('#update-result').innerHTML=updateResult(state.controller);
  } else if (actionName==='close-modal') $('#modal').close();
  else if (actionName==='debug') await showDebug(index);
}
