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
  if (label) {
    label.textContent=changed?'Unsaved changes':'No unsaved changes';
    label.classList.toggle('dirty',changed);
  }
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

// How each form saves. A form's type is the first part of its key: "ferm-0" is a ferm form.
// path:      the endpoint the form posts to.
// refresh:   the endpoint read back afterwards to confirm the save.
// usesUnits: the form holds temperatures, so the controller's unit is checked before saving.
// limits:    the longest text the firmware's buffers accept for each field, in UTF-8 bytes.
// buildBody: validates the form and returns the JSON to post. Throws an error to refuse the save.
// afterSave: optional clean-up once the save is confirmed.
const formTypes = {
  ferm: {path:'/fermenter', refresh:'fermenters', usesUnits:true, limits:{name:31,beer:31,yeast:31},
    buildBody:(form,data,index)=>fermenterPayload(form,data,index),
    afterSave:()=>$('#editor').close()},
  profile: {path:'/profile', refresh:'profiles', usesUnits:true, limits:{name:31},
    buildBody:(form,data,index)=>checkedProfilePayload(form,index),
    afterSave:index=>profileDrafts.delete(index)},
  probe: {path:'/probes', refresh:'probes', usesUnits:true,
    buildBody:(form,data,index)=>sensorPayload(form,data,'probe',index)},
  tilt: {path:'/tilt', refresh:'tilts', usesUnits:true,
    buildBody:(form,data,index)=>sensorPayload(form,data,'tilt',index)},
  ispindel: {path:'/ispindel/config', refresh:'ispindels', usesUnits:true,
    buildBody:(form,data,index)=>sensorPayload(form,data,'ispindel',index)},
  plug: {path:'/smartplug', refresh:'smartplugs', limits:{manufacturer:23,model:23},
    buildBody:(form,data,index)=>plugPayload(data,index)},
  service: {path:'/brewservices', refresh:'brewservices', limits:{serviceId:47,deviceName:31},
    buildBody:(form,data,index)=>({index,enabled:data.enabled,serviceId:data.serviceId,deviceName:data.deviceName})},
  mqtt: {path:'/mqtt', refresh:'mqtt', limits:{host:63,username:31,password:47,baseTopic:31},
    buildBody:(form,data)=>mqttPayload(data)},
  global: {path:'/controller', refresh:'controller',
    buildBody:(form,data)=>globalPayload(data)},
  debug: {path:'/debug', refresh:'debug',
    buildBody:(form,data)=>({DebugMode:data.enabled})},
  syslog: {path:'/syslog', refresh:'syslog', limits:{host:63},
    buildBody:(form,data)=>({enabled:data.enabled,host:data.host,port:+data.port,
      facility:+data.facility,minLevel:+data.level})}
};

// Prevent duplicate validation and save attempts for the same form, and show why a save was refused.
async function saveForm(form) {
  if (form.dataset.checking) return;
  form.dataset.checking='true';
  try { await saveFormChecked(form); }
  catch (error) { saveError(form,error.message); }
  finally { delete form.dataset.checking; }
}

// Validate a changed form and build its JSON, then send it. Throws an error to refuse the save.
async function saveFormChecked(form) {
  if (saving || form.dataset.dirty!=='true' || !form.reportValidity()) return;
  if (state.unitChanged) throw new Error('Controller units changed. Discard all drafts and refresh before saving.');
  if (form.dataset.unknown) {
    throw new Error('Read the current controller state using Refresh before explicitly saving again.');
  }
  const key=form.dataset.form,[kind,slot]=key.split('-'),index=+slot;
  const type=formTypes[kind];
  if (type.usesUnits) {
    await fetchData('controller');
    if (saving) return;
    checkFormUnits(form);
  }
  checkNumberFields(form);
  const data=formData(form);
  checkTextLimits(data,type.limits);
  const body=await type.buildBody(form,data,index);
  // Building a profile reads the controller, so another save may have started meanwhile.
  if (!body) return;
  await sendSave(form,type,body,index);
}

// Refuse a save if the controller's unit changed after the form was drawn.
function checkFormUnits(form) {
  if (state.unitChanged || (form.dataset.unit && form.dataset.unit!==state.unit)) {
    throw new Error('Controller units changed. Discard and review drafts before saving.');
  }
}

// Refuse a save if a numeric field holds something like "1e999".
function checkNumberFields(form) {
  for (const el of $$('input[type="number"]',form)) {
    if (!Number.isFinite(Number(el.value))) {
      el.focus();
      throw new Error('Enter a finite number in every numeric field.');
    }
  }
}

// Refuse a save if any text is too long for the firmware's buffers.
function checkTextLimits(data,limits={}) {
  for (const [name,limit] of Object.entries(limits)) {
    if (new TextEncoder().encode(data[name]||'').length>limit) {
      throw new Error(name+' exceeds the controller limit of '+limit+' UTF-8 bytes.');
    }
  }
}

// Post a validated form, then reread its endpoint before clearing the draft.
async function sendSave(form,type,body,index) {
  const key=form.dataset.form;
  saving=true;
  $('[data-save]',form).disabled=true;
  saveError(form,'Saving…');
  try {
    const reply=await post(type.path,body);
    try { await fetchData(type.refresh); }
    catch (error) {
      error.unknown=true;
      error.message='Save acknowledged, but the confirmation read failed. Refresh before saving again. '+error.message;
      throw error;
    }
    formDrafts.delete(key);
    if (type.afterSave) type.afterSave(index);
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
  for (const key of unknownForms) {
    if (names.includes(formTypes[key.split('-')[0]].refresh)) unknownForms.delete(key);
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
