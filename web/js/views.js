'use strict';
const $ = (s, root = document) => root.querySelector(s);
const $$ = (s, root = document) => [...root.querySelectorAll(s)];
const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const clone = value => JSON.parse(JSON.stringify(value));
const paths = {
  dashboard:'<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>',
  profiles:'<path d="M3 18h4V12h5V7h5V3h4M3 3v18h18"/>',
  sensors:'<path d="M10 14.5V5a2 2 0 0 1 4 0v9.5a5 5 0 1 1-4 0Z"/><path d="M12 10v8M17 5h3M17 9h2"/>',
  plugs:'<path d="M8 3v5m8-5v5M6 8h12v3a6 6 0 0 1-6 6v4M5 8h14"/>',
  reporting:'<path d="M5 17h3m3-4h3m3-5h3M3 3v18h18"/>',
  system:'<circle cx="12" cy="12" r="3"/><path d="m9 3 1 2h4l1-2 3 2-1 2 2 3h2v4h-2l-2 3 1 2-3 2-1-2h-4l-1 2-3-2 1-2-2-3H3v-4h2l2-3-1-2Z"/>',
  refresh:'<path d="M20 7v5h-5M4 17v-5h5M5 8a7 7 0 0 1 12-3l3 3M4 16l3 3a7 7 0 0 0 12-3"/>',
  arrow:'<path d="M5 12h14m-5-5 5 5-5 5"/>',
  check:'<path d="m5 12 4 4L19 6"/>',
  info:'<circle cx="12" cy="12" r="9"/><path d="M12 11v6m0-10v1"/>',
  clock:'<circle cx="12" cy="12" r="9"/><path d="M12 6v6l4 2"/>',
  drop:'<path d="M12 3c-2 4-7 8-7 12a7 7 0 0 0 14 0c0-4-5-8-7-12Z"/>',
  shield:'<path d="m12 3 8 3v6c0 5-8 9-8 9s-8-4-8-9V6Z"/><path d="m8 12 3 3 5-6"/>',
  file:'<path d="M14 3H5v18h14V8Zm0 0v5h5M8 12h8m-8 4h6"/>',
  pause:'<path d="M8 5v14M16 5v14"/>',
  play:'<path d="m8 4 12 8-12 8Z"/>',
  close:'<path d="m6 6 12 12M6 18 18 6"/>',
  download:'<path d="M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5"/>'
};
const icon = name => `<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">${paths[name] || paths.info}</svg>`;
const navItems = [['dashboard','Dashboard'],['profiles','Profiles'],['sensors','Sensors'],['plugs','Smart plugs'],['reporting','Reporting'],['system','System & diagnostics']];
const temp = value => value == null ? '—' : (state.unit === 'F' ? value*1.8+32 : value).toFixed(1);
const delta = value => state.unit === 'F' ? +(value*1.8).toFixed(2) : value;
const toC = value => state.unit === 'F' ? (value-32)/1.8 : value;
const deltaC = value => state.unit === 'F' ? value/1.8 : value;
const badge = (label,kind='good') => `<span class="badge ${kind}"><i></i>${esc(label)}</span>`;
const btn = (label,action,cls='secondary',extra='') => `<button type="button" class="button ${cls}" data-action="${action}" ${extra}>${label}</button>`;
function field(label,name,value,type='text',extra='',help='') {
  return `<label class="field">${esc(label)}
    <input name="${name}" type="${type}" value="${esc(value)}" ${extra}>
    ${help?`<small>${esc(help)}</small>`:''}</label>`;
}

function select(label,name,value,options,extra='') {
  const choices=options.map(([key,text])=>
    `<option value="${esc(key)}" ${String(value)===String(key)?'selected':''}>${esc(text)}</option>`).join('');
  return `<label class="field">${esc(label)}<select name="${name}" ${extra}>${choices}</select></label>`;
}
const fermOptions = () => [[99,'Unassigned'],...state.fermenters.map(f=>[f.id,f.name])];
const fnOptions = [[99,'Unassigned'],[2,'Beer temperature'],[3,'Ambient temperature'],[4,'Tilt probe'],[5,'iSpindel probe'],[6,'Air probe'],[7,'Control temperature']];
function toggle(label,name,checked,help='') {
  return `<label class="toggle-row"><span>${esc(label)}${help?`<small>${esc(help)}</small>`:''}</span>
    <input type="checkbox" name="${name}" ${checked?'checked':''}></label>`;
}

function actions(label='Save changes',extras='') {
  return `<div class="form-actions"><div class="form-error" role="alert"></div>
    <button type="submit" class="button" data-save>${label}</button>${extras}
    <button type="button" class="button ghost" data-action="discard-form">Discard</button>
    <span class="saved-label">No unsaved changes</span></div>`;
}

function title(kicker,name,description,buttons='') {
  return `<div class="page-title"><div><p class="eyebrow">${kicker}</p><h1>${name}</h1>
    <p class="subtitle">${description}</p></div><div class="title-actions">${buttons}</div></div>`;
}

function formData(form) {
  const data={};
  for (const input of $$('[name]',form)) {
    if (input.type==='file') continue;
    data[input.name]=input.type==='checkbox'?input.checked:input.value;
  }
  return data;
}

function applyData(form,data) {
  for (const input of $$('[name]',form)) {
    if (!(input.name in data)) continue;
    if (input.type==='checkbox') input.checked=data[input.name];
    else input.value=data[input.name];
  }
}

// Keep the displayed baseline separate from exact profile temperatures.
// A name-only edit must not save a rounded graph label back to the controller.
function mountForms(root=document) {
  for (const form of $$('form[data-form]',root)) {
    const key=form.dataset.form;
    form.dataset.unit=state.unit;
    formBaselines.set(form,formData(form));
    if (key.startsWith('profile-')) {
      const index=+key.split('-')[1];
      profileFormValues.set(form,clone(profileDrafts.get(index)||state.profiles[index]));
    }
    if (drafts.has(key)) applyData(form,drafts.get(key));
    updateDirty(form);
  }
}

function toast(text) {
  $('#toast').textContent=text;
  $('#toast').classList.add('show');
  clearTimeout(toastTimer);
  toastTimer=setTimeout(()=>$('#toast').classList.remove('show'),4200);
}

function closeNav() {
  $('#sidebar').classList.remove('open');
  $('#nav-scrim').hidden=true;
  $('#menu-toggle').setAttribute('aria-expanded','false');
}

function render() {
  const requested=location.hash.slice(1).split('/')[0];
  state.route=navItems.some(item=>item[0]===requested)?requested:'dashboard';
  const main=$('#main');
  const sameView=main.dataset.route===state.route;
  // Replacing the view must not move a horizontal list the user has scrolled.
  const profileScroll=sameView?$('.profile-list',main)?.scrollLeft:0;
  const tabScroll=sameView?$('.tabs',main)?.scrollLeft:0;
  $('#navigation').innerHTML=navItems.map(([key,label])=>`
    <a href="#${key}" class="nav-item ${state.route===key?'active':''}" ${state.route===key?'aria-current="page"':''}>
      ${icon(key)}<span>${label}</span>
      ${key==='dashboard'?`<span class="nav-count">${state.fermenters.length}</span>`:''}</a>`).join('');
  $('#crumb').textContent=navItems.find(n=>n[0]===state.route)[1];
  $('#unit-toggle').textContent='°'+state.unit;
  const renderers={dashboard:dashboard,profiles:profiles,sensors:sensors,plugs:plugs,reporting:reporting,system:system};
  const offline=state.scenario==='offline'?`<div class="notice error">${icon('info')}<div>
    <strong>Controller is unreachable.</strong><br>Showing the last received readings. Your unsaved edits are safe.
    ${btn('Try reconnecting','reconnect','small ghost')}</div></div>`:'';
  main.innerHTML=offline+renderers[state.route]();
  main.dataset.route=state.route;
  const profileList=$('.profile-list',main),tabs=$('.tabs',main);
  if (profileList && profileScroll) profileList.scrollLeft=profileScroll;
  if (tabs && tabScroll) tabs.scrollLeft=tabScroll;
  mountForms(main);
  markProfileDraft();
  closeNav();
  updateAge();
  document.title=navItems.find(item=>item[0]===state.route)[1]+' · OurBrewbot';
}
const stepNames=['Temperature over time','Time at temperature','Free rise','Specific gravity','Attenuation','Temperature reached','Gravity reached','Attenuation reached','Time and gravity','Time and attenuation'];
function system() {
  const tabs=[['overview','Controller'],['settings','Settings'],['files','Files & backup'],
    ['rf','RF sniffer'],['ble','BLE console'],['update','Firmware']];
  return title('LOOK AFTER YOUR CONTROLLER','System & diagnostics','Settings, maintenance and tools for your brew room.')+`
    <div class="tabs" role="tablist" aria-label="System section">${tabs.map(([key,label])=>`
      <button role="tab" aria-selected="${state.systemTab===key}" class="${state.systemTab===key?'active':''}"
        data-action="system-tab" data-value="${key}">${label}</button>`).join('')}</div>${systemPanels[state.systemTab]()}`;
}

function showModal(html) {
  $('#modal-content').innerHTML=html;
  if (!$('#modal').open) $('#modal').showModal();
}

function download(name,text,type='application/json') {
  const url=URL.createObjectURL(new Blob([text],{type}));
  const link=document.createElement('a');
  link.href=url;
  link.download=name;
  link.click();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}

function captureProfile() {
  const index=state.profile;
  const form=$(`form[data-form="profile-${index}"]`);
  const profile=clone(profileFormValues.get(form)||profileDrafts.get(index)||state.profiles[index]);
  if (!form) return profile;
  const data=formData(form),baseline=formBaselines.get(form)||{};
  profile.name=data.name;
  profile.steps.forEach((step,n)=>{
    step.type=+data['type-'+n];
    if (data['start-'+n]!==baseline['start-'+n]) step.start=toC(+data['start-'+n]);
    if (data['end-'+n]!==baseline['end-'+n]) step.end=toC(+data['end-'+n]);
    if (data['days-'+n]!==undefined) step.days=+data['days-'+n];
    if (data['trigger-'+n]!==undefined) step.trigger=+data['trigger-'+n];
  });
  return profile;
}

function profileChanged() {
  const profile=captureProfile();
  if (profileValues(profile)===profileValues(state.profiles[state.profile])) profileDrafts.delete(state.profile);
  else profileDrafts.set(state.profile,profile);
}

function saveError(form,message) {
  $('.form-error',form).textContent=message;
}
