'use strict';
// Draft tracking and the save flow shared by every settings form.

// Read named form controls, keeping checkbox values as booleans.
function formData(form) {
  const data={};
  for (const input of $$('[name]',form)) {
    if (input.type==='file') continue;
    data[input.name]=input.type==='checkbox'?input.checked:input.value;
  }
  return data;
}

// Restore supplied values into matching form controls.
function applyData(form,data) {
  for (const input of $$('[name]',form)) {
    if (!(input.name in data)) continue;
    if (input.type==='checkbox') input.checked=data[input.name];
    else input.value=data[input.name];
  }
}

// Record form baselines and restore drafts after rendering a view.
// Profile forms also keep their exact temperatures, so a name-only edit cannot save rounded values.
function mountForms(root=document) {
  for (const form of $$('form[data-form]',root)) {
    const key=form.dataset.form;
    form.dataset.unit=state.unit;
    formBaselines.set(form,formData(form));
    if (key.startsWith('profile-')) {
      const index=+key.split('-')[1];
      profileFormValues.set(form,clone(profileDrafts.get(index)||state.profiles[index]));
    }
    if (formDrafts.has(key)) applyData(form,formDrafts.get(key));
    updateDirty(form);
  }
}

// Track form drafts and update save availability and unsaved-change labels.
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
  if (changed) formDrafts.set(key,formData(form)); else formDrafts.delete(key);
  if (unknownForms.has(key)) form.dataset.unknown='true';
}

// Display a validation or save message beside the relevant form.
function saveError(form,message) {
  $('.form-error',form).textContent=message;
}

// Keep a received numeric value unchanged unless its form field was edited.
// Fields may be rounded for readability, and a name-only save must not alter control.
function savedNumber(form,data,name,received) {
  const baseline=formBaselines.get(form);
  return baseline && data[name]===baseline[name] ? received : +data[name];
}

// Prevent duplicate validation and save attempts for the same form.
async function saveForm(form) {
  if (form.dataset.checking) return;
  form.dataset.checking='true';
  try { await saveFormChecked(form); }
  finally { delete form.dataset.checking; }
}

// Validate and save a changed form, then reread its endpoint before clearing the draft.
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
    if (data.unit!==state.unit && formDrafts.size>1) { saveError(form,'Save or discard other drafts before changing units.'); return; }
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
    formDrafts.delete(key);
    if (kind==='profile') profileDrafts.delete(index);
    if (kind==='ferm') $('#editor').close();
    if (!formDrafts.size && !profileDrafts.size) state.unitChanged=false;
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

// Reread view data and clear uncertain-save markers only for forms whose endpoint was reread.
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

// Discard a form's browser draft and restore its saved values and unit state.
function discardForm(form) {
  const key=form.dataset.form,oldUnit=form.dataset.unit;
  formDrafts.delete(key);
  if (key.startsWith('profile-')) {
    profileDrafts.delete(state.selectedProfile);
    render();
  } else {
    applyData(form,formBaselines.get(form));
    updateDirty(form);
    saveError(form,'');
  }
  if (!formDrafts.size && !profileDrafts.size) state.unitChanged=false;
  if (oldUnit!==state.unit) {
    render();
    if (key.startsWith('ferm-')) openFerm(+key.split('-')[1]);
  }
}
