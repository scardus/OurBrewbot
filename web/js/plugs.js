'use strict';

// Build the smart-plug view and its link to the RF sniffer.
function plugs() {
  return title('HEAT WHEN NEEDED. COOL WHEN NEEDED.','Smart plugs',
    'Configure your RF plugs, then test each saved assignment.',btn('Open RF sniffer','open-rf','secondary'))+
    `<div class="grid">${state.plugs.map(plugCard).join('')}</div>`;
}

// Build one plug's assignment, RF settings, preset choices and explicit test controls.
function plugCard(p,i) {
  const fermenter=state.fermenters[p.fermenter]?.name||('Fermenter '+p.fermenter);
  let name=fermenter+(p.role==='hot'?' · Heater':' · Cooler');
  if (p.role==='none') name='Unassigned plug';
  else if (p.role==='aux1') name='Auxiliary 1';
  else if (p.role==='aux2') name='Auxiliary 2';
  let status=p.state?'On':'Off';
  let statusKind=p.state?'good':'quiet';
  if (p.role==='none') {
    status='Unassigned';
    statusKind='quiet';
  }
  return `<article class="card panel plug-card">
    <div class="panel-header"><div><div class="slot-label">PLUG ${String(p.index).padStart(2,'0')}</div>
      <h2>${esc(name)}</h2><p>${p.role==='none'?'Available slot':esc(p.manufacturer+' · '+p.model)}</p></div>
      <span data-plug-status="${i}">${badge(status,statusKind)}</span></div>
    <form data-form="plug-${i}">
      <div class="fields">
        ${select('Role','role',p.role,[['none','Unassigned'],['hot','Heater'],['cold','Cooler'],['aux1','Auxiliary 1'],['aux2','Auxiliary 2']])}
        ${select('Fermenter','fermenter',p.fermenter,fermOptions())}
      </div>
      <details><summary>RF codes & manufacturer</summary>
        <div class="fields">
          ${field('Manufacturer','manufacturer',p.manufacturer,'text','maxlength="23"')}
          ${field('Model','model',p.model,'text','maxlength="23"')}
          ${select('Preset','preset','',[['','Choose a preset'],...plugPresets.map((preset,n)=>[n,preset.n])])}
          ${select('Remote button','remote',0,[[0,'Button 1'],[1,'Button 2'],[2,'Button 3'],[3,'Button 4']])}
        </div>
        <div style="margin-top:10px">${btn('Apply preset to draft','plug-preset','small secondary',`data-id="${i}"`)}</div>
        <div class="fields" style="margin-top:15px">
          ${field('On code','onCode',p.onCode,'number','min="0" max="4294967295" step="1" required')}
          ${field('Off code','offCode',p.offCode,'number','min="0" max="4294967295" step="1" required')}
          ${field('Protocol','protocol',p.protocol,'number','min="1" max="12" step="1" required')}
          ${field('Bits','bits',p.bits,'number','min="1" max="32" step="1" required')}
          ${field('Pulse length (µs)','delay',p.delay,'number','min="1" max="65535" step="1" required')}
        </div>
      </details>
      ${actions('Save plug')}
      <div class="two-col-actions" style="margin-top:12px">
        ${btn('Test on','plug-test','small secondary',`data-id="${i}" data-value="on"`)}
        ${btn('Test off','plug-test','small ghost',`data-id="${i}" data-value="off"`)}
      </div>
      <p class="review-caption">Tests use the saved codes, not an unsaved draft.</p>
    </form>
  </article>`;
}
