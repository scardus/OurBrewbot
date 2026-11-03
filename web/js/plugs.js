'use strict';
// The smart plugs view.

// Build the smart-plug view and its link to the RF sniffer.
function plugsView() {
  return pageTitle('HEAT WHEN NEEDED. COOL WHEN NEEDED.','Smart plugs',
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
  const roles=[['none','Unassigned'],['hot','Heater'],['cold','Cooler'],['aux1','Auxiliary 1'],['aux2','Auxiliary 2']];
  return `<article class="card panel plug-card">
    <div class="panel-header"><div><div class="slot-label">PLUG ${String(p.index).padStart(2,'0')}</div>
      <h2>${esc(name)}</h2><p>${p.role==='none'?'Available slot':esc(p.manufacturer+' · '+p.model)}</p></div>
      <span data-plug-status="${i}">${badge(status,statusKind)}</span></div>
    <form data-form="plug-${i}">
      <div class="fields">
        ${select('Role','role',p.role,roles)}
        ${select('Fermenter','fermenter',p.fermenter,fermOptions())}
      </div>
      <details><summary>RF codes & manufacturer</summary>
        <div class="fields">
          ${field('Manufacturer','manufacturer',p.manufacturer,'text','maxlength="23"')}
          ${field('Model','model',p.model,'text','maxlength="23"')}
          ${select('Preset','preset','',[['','Choose a preset'],...plugPresets.map((preset,n)=>[n,preset.name])])}
          ${select('Remote button','remote',0,[[0,'Button 1'],[1,'Button 2'],[2,'Button 3'],[3,'Button 4']])}
        </div>
        <div style="margin-top:10px">
          ${btn('Apply preset to draft','plug-preset','small secondary',`data-id="${i}"`)}
        </div>
        <div class="fields" style="margin-top:15px">
          ${field('On code','onCode',p.onCode,'number','min="0" max="4294967295" step="1" required')}
          ${field('Off code','offCode',p.offCode,'number','min="0" max="4294967295" step="1" required')}
          ${field('Protocol','protocol',p.protocol,'number','min="1" max="12" step="1" required')}
          ${field('Bits','bits',p.bits,'number','min="1" max="32" step="1" required')}
          ${field('Pulse length (µs)','delay',p.delay,'number','min="1" max="65535" step="1" required')}
        </div>
      </details>
      ${formActions('Save plug')}
      <div class="two-col-actions" style="margin-top:12px">
        ${btn('Test on','plug-test','small secondary',`data-id="${i}" data-value="on"`)}
        ${btn('Test off','plug-test','small ghost',`data-id="${i}" data-value="off"`)}
      </div>
      <p class="review-caption">Tests use the saved codes, not an unsaved draft.</p>
    </form>
  </article>`;
}

// Validate RF timings and translate a plug draft into the firmware's save contract.
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
  const protocolPulses=[[4,32],[3,11],[15,101],[4,7],[3,20],[3,24],[7,64],[23,133],[23,137],[4,19],[3,37],[3,37]];
  const pulses=protocolPulses[+data.protocol-1];
  if (!pulses || Math.floor(10*(+data.bits*pulses[0]+pulses[1])*(+data.delay)/1000)>2000) {
    throw new Error('These RF timings would make a send exceed the firmware limit of 2 seconds.');
  }
  return {index:state.plugs[index].index,manufacturer:data.manufacturer,model:data.model,
    onCode:+data.onCode,offCode:+data.offCode,protocol:+data.protocol,bits:+data.bits,delay:+data.delay,
    function:plugFunction,fermenter};
}

// Copy a saved RF preset into the plug draft without writing controller settings.
function applyPreset(form) {
  const d=formData(form),preset=plugPresets[+d.preset];
  if (d.preset==='' || !preset) return;
  const button=+d.remote;
  const values={manufacturer:preset.manufacturer,model:preset.name,onCode:preset.codes[button*2],
    offCode:preset.codes[button*2+1],protocol:1,bits:24,delay:preset.delay};
  for (const [name,value] of Object.entries(values)) form.elements[name].value=value;
  updateDirty(form);
  toast('Preset applied to draft. Save to store it.');
}

// Confirm transmitting a configured plug's saved code and refresh its state.
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
