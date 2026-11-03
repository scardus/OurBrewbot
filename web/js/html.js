'use strict';

// Find the first matching element within the supplied root.
const $ = (s, root = document) => root.querySelector(s);

// Return all matching elements as an array within the supplied root.
const $$ = (s, root = document) => [...root.querySelectorAll(s)];

// Escape device text before inserting it into HTML.
const htmlEscapes = {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'};
const esc = value => String(value ?? '').replace(/[&<>"']/g, c => htmlEscapes[c]);

// Copy JSON-compatible data so draft edits cannot change saved state.
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
  info:'<circle cx="12" cy="12" r="9"/><path d="M12 11v6m0-10v1"/>',
  shield:'<path d="m12 3 8 3v6c0 5-8 9-8 9s-8-4-8-9V6Z"/><path d="m8 12 3 3 5-6"/>',
  file:'<path d="M14 3H5v18h14V8Zm0 0v5h5M8 12h8m-8 4h6"/>',
  pause:'<path d="M8 5v14M16 5v14"/>',
  play:'<path d="m8 4 12 8-12 8Z"/>',
  close:'<path d="m6 6 12 12M6 18 18 6"/>'
};

// Build the requested inline SVG icon, falling back to the information icon.
const icon = name => `<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">${paths[name] || paths.info}</svg>`;

// Build a labelled status badge with the requested appearance.
const badge = (label,kind='good') => `<span class="badge ${kind}"><i></i>${esc(label)}</span>`;

// Build an action button for the shared click handler.
const btn = (label,action,cls='secondary',extra='') =>
  `<button type="button" class="button ${cls}" data-action="${action}" ${extra}>${label}</button>`;

// Build a labelled input with optional validation attributes and help text.
function field(label,name,value,type='text',extra='',help='') {
  return `<label class="field">${esc(label)}
    <input name="${name}" type="${type}" value="${esc(value)}" ${extra}>
    ${help?`<small>${esc(help)}</small>`:''}</label>`;
}

// Build a labelled dropdown with its current option selected.
function select(label,name,value,options,extra='') {
  const choices=options.map(([key,text])=>
    `<option value="${esc(key)}" ${String(value)===String(key)?'selected':''}>${esc(text)}</option>`).join('');
  return `<label class="field">${esc(label)}<select name="${name}" ${extra}>${choices}</select></label>`;
}

// Build a labelled checkbox switch with optional help text.
function toggle(label,name,checked,help='') {
  return `<label class="toggle-row"><span>${esc(label)}${help?`<small>${esc(help)}</small>`:''}</span>
    <input type="checkbox" name="${name}" ${checked?'checked':''}></label>`;
}

// Build the shared save, discard and validation-message controls.
function actions(label='Save changes',extras='') {
  return `<div class="form-actions"><div class="form-error" role="alert"></div>
    <button type="submit" class="button" data-save>${label}</button>${extras}
    <button type="button" class="button ghost" data-action="discard-form">Discard</button>
    <span class="saved-label">No unsaved changes</span></div>`;
}

// Build a page heading, description and optional action controls.
function title(kicker,name,description,buttons='') {
  return `<div class="page-title"><div><p class="eyebrow">${kicker}</p><h1>${name}</h1>
    <p class="subtitle">${description}</p></div><div class="title-actions">${buttons}</div></div>`;
}

// List fermenter choices together with the unassigned option.
const fermOptions = () => [[99,'Unassigned'],...state.fermenters.map(f=>[f.id,f.name])];

// Show a temporary message, replacing any previous dismissal timer.
function toast(text) {
  $('#toast').textContent=text;
  $('#toast').classList.add('show');
  clearTimeout(toastTimer);
  toastTimer=setTimeout(()=>$('#toast').classList.remove('show'),4200);
}

// Set the shared modal content and open it if necessary.
function showModal(html) {
  $('#modal-content').innerHTML=html;
  if (!$('#modal').open) $('#modal').showModal();
}

// Show a confirmation dialog and run its callback only after the user accepts.
function confirmAction(heading,description,label,callback) {
  showModal(`<h2>${esc(heading)}</h2><p>${esc(description)}</p>
    <div class="modal-actions">
      ${btn('Cancel','close-modal','ghost')}
      <button class="button" id="confirm-action">${esc(label)}</button>
    </div>`);
  // Run the accepted action once and show any reported failure.
  $('#confirm-action').onclick=async()=>{
    $('#confirm-action').disabled=true;
    $('#modal').close();
    try { await callback(); } catch (error) { toast(error.message); }
  };
}

// Download browser-held text through a temporary object URL.
function download(name,text,type='application/json') {
  const url=URL.createObjectURL(new Blob([text],{type}));
  const link=document.createElement('a');
  link.href=url;
  link.download=name;
  link.click();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}
