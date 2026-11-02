'use strict';

// Build the overview totals and fermenter cards from the latest browser state.
function dashboard() {
  const active=state.fermenters.filter(f => f.power).length;
  const alarms=state.fermenters.filter(f => f.alarmActive).length;
  const c=state.controller;
  const uptime=finite(c.Uptime)+Math.floor((Date.now()-(state.controllerReadAt||Date.now()))/60000);
  return title('FERMENTATION, AT A GLANCE','Dashboard','Readings and control for your fermenters.',
    `<span class="refresh-label"><i class="pulse-dot"></i><span data-age></span></span>`+
    btn(icon(state.paused?'play':'pause'),'pause','ghost','aria-label="Pause or resume refresh"')+
    btn(icon('refresh')+'Refresh','refresh','secondary'))+
    `<div class="overview">${[
      ['Active fermenters',active+' / 4',(4-active)+' powered off'],
      ['Temperature control',alarms?'Check alarms':state.fermenters.some(f=>f.power&&f.tempControl)?'Enabled':'Off',alarms+' active alarms'],
      ['Running profiles',state.fermenters.filter(f=>f.running||f.paused).length,'Includes paused profiles'],
      ['Controller uptime',Math.floor(uptime/1440)+'d '+Math.floor(uptime%1440/60)+'h '+uptime%60+'m','WiFi signal '+shown(c.RSSI)+' dBm']
    ].map(([label,value,caption])=>`<div class="overview-item"><div class="overview-top">${esc(label)}</div><div class="overview-value" style="font-size:21px">${esc(value)}</div><div class="overview-caption">${esc(caption)}</div></div>`).join('')}</div>`+
    `<div class="section-heading"><h2>Fermenters <small>Live overview</small></h2><span class="subtle">Temperatures in °${state.unit}</span></div>`+
    `<div class="grid">${state.fermenters.map(fermCard).join('')}</div>`+
    `<div class="dashboard-note">${icon('shield')}Readings refresh separately from your edits. Settings are saved only when you choose Save.</div>`;
}

// Build a labelled sensor value that polling can update independently of its form.
function sensorReading(label, key, value) {
  return `<div><span data-live="${key}">${esc(value)}</span><small>${esc(label)}</small></div>`;
}

// Build the selected sensor tab with current readings, assignments and calibration forms.
function sensors() {
  const kind=state.sensorTab;
  const slots=state[kind];
  const controls=`<div class="tabs" role="tablist" aria-label="Sensor type">${[
    ['probes','Temperature probes'],['tilts','Tilts'],['ispindels','iSpindels']
  ].map(([key,label])=>`<button role="tab" aria-selected="${kind===key}" class="${kind===key?'active':''}" data-action="sensor-tab" data-value="${key}">${label}</button>`).join('')}</div>`;
  return title('GOOD DATA, BETTER CONTROL','Sensors','Assign each sensor and fine-tune its readings.')+controls+
    (kind==='tilts'?'<p class="notice info">Eight colours can be detected; up to four Tilts can be configured.</p>':'')+
    (slots.length?`<div class="grid">${slots.map((s,i)=>{
      const name=s.name || (kind==='probes'?'Probe '+s.index:'Empty slot '+i);
      const key=kind+'.'+i;
      const formKind=kind==='probes'?'probe':kind==='tilts'?'tilt':'ispindel';
      const detail=kind==='probes'?s.address:kind==='tilts'?(s.isPro?'Tilt Pro':'Tilt'):'ID '+shown(s.id);
      return `<article class="card panel"><div class="panel-header"><div><div class="slot-label">${esc(kind)} · ${i}</div><h2>${esc(name)}</h2><p>${esc(detail)}</p></div>`+
        `<span data-sensor-status="${key}">`+(kind==='tilts'?badge(s.active?'Reading received':'Not detected',s.active?'good':'quiet'):
          kind==='ispindels'?badge(s.fresh?'Fresh':'No recent report',s.fresh?'good':'warn'):'')+`</span></div>`+
        `<div class="sensor-reading">${sensorReading('Temperature',key+'.temp',temp(s.temp)+' °'+state.unit)}`+
        (kind==='probes'?'':sensorReading('Specific gravity',key+'.sg',s.sg>0?(+s.sg).toFixed(4):'—'))+
        (kind==='ispindels'?sensorReading('Battery (V)',key+'.battery',finite(s.battery).toFixed(2)):'')+`</div>`+
        (kind==='ispindels'?`<p class="review-caption" data-live="${key}.detail">${esc(spindelDetail(s))}</p>`:'')+
        `<form data-form="${formKind}-${i}"><div class="fields">${select(kind==='probes'?'Function':'Temperature reading','fn',s.fn,kind==='probes'?fnOptions:[[2,'Beer temperature'],[99,'Not used']])}`+
        select('Fermenter','fermenter',s.fermenter,fermOptions())+
        (kind==='ispindels'?select('Device gravity unit','unit',s.unit,[[0,'Specific gravity'],[1,'Plato']]):'')+
        field('Temperature adjustment (°'+state.unit+')','adjust',delta(s.adjust),'number','step="0.1" required')+
        (kind==='probes'?'':field('Gravity adjustment','sgAdjust',s.sgAdjust,'number','step="0.0001" required'))+
        `</div>${kind==='ispindels'&&s.gravityUnit?'<p class="review-caption">The device reports its own gravity unit; it can reset this selection on its next report.</p>':''}`+
        actions('Save changes',kind==='probes'?'':btn('Clear slot','clear-sensor','ghost',`data-kind="${kind}" data-id="${i}"`))+
        `</form></article>`;
    }).join('')}</div>`:'<p class="empty-state">No detected or configured sensors returned by the controller.</p>');
}

// Summarize an iSpindel's extra readings and the age of its last report.
function spindelDetail(s) {
  const age=s.minutesSince>=65535?'Never seen':s.minutesSince===0?'Less than a minute ago':s.minutesSince+' min ago';
  return 'Angle '+shown(s.angle)+'° · RSSI '+shown(s.rssi)+' dBm · Velocity '+shown(s.velocity)+
    ' · Corrected gravity '+shown(s.corrGravity)+' · Runtime '+shown(s.runTime)+' s · Unit '+shown(s.gravityUnit)+' · '+age;
}

// Update live text and status badges without replacing editing forms.
function updateLiveReadings() {
  $$('[data-controller]').forEach(node=>{
    node.textContent=controllerValue(node.dataset.controller);
  });
  $$('[data-plug-status]').forEach(node=>{
    const plug=state.plugs[+node.dataset.plugStatus];
    if (!plug) return;
    let label=plug.state?'On':'Off';
    if (plug.role==='none') label='Unassigned';
    node.innerHTML=badge(label,plug.state?'good':'quiet');
  });
  $$('[data-sensor-status]').forEach(node=>{
    const [kind,index]=node.dataset.sensorStatus.split('.'),sensor=state[kind]?.[+index];
    if (!sensor) return;
    if (kind==='tilts') node.innerHTML=badge(sensor.active?'Reading received':'Not detected',sensor.active?'good':'quiet');
    if (kind==='ispindels') node.innerHTML=badge(sensor.fresh?'Fresh':'No recent report',sensor.fresh?'good':'warn');
  });
  $$('[data-live]').forEach(node=>{
    const [kind,index,field]=node.dataset.live.split('.');
    const sensor=state[kind]?.[+index];
    if (!sensor) return;
    let reading=shown(sensor[field]);
    if (field==='temp') reading=temp(sensor.temp)+' °'+state.unit;
    else if (field==='detail') reading=spindelDetail(sensor);
    else if (field==='sg') reading=sensor.sg>0?(+sensor.sg).toFixed(4):'—';
    else if (field==='battery') reading=finite(sensor.battery).toFixed(2);
    node.textContent=reading;
  });
  updateAge();
}

// Format controller and reset information with the appropriate units.
function controllerValue(field) {
  const value=field==='resetReason'?state.health.resetReason:state.controller[field];
  if (field==='RSSI') return shown(value)+' dBm';
  if (field==='FreeHeap') return shown(value)+' bytes';
  if (field==='Uptime') return shown(value)+' min';
  return shown(value);
}

const systemPanels={
  // Build controller information and links to the standalone maintenance pages.
  overview() {
    return `<div class="system-layout"><article class="card panel"><h2>Controller information</h2>`+
      [['Firmware','FirmwareVersion'],['WiFi network','WiFiSSID'],['IP address','IP'],['mDNS name','mDNSName'],
        ['Signal strength','RSSI'],['Free heap','FreeHeap'],['Uptime','Uptime'],
        ['Chip ID','ChipId'],['Reset reason','resetReason']].map(([label,field])=>
        `<div class="list-row"><span>${esc(label)}</span><strong data-controller="${field}">${esc(controllerValue(field))}</strong></div>`).join('')+
      `</article><article class="card panel"><h2>Maintenance</h2><p class="subtitle">These links open the controller's standalone confirmation pages.</p>`+
      [['/reboot','Restart controller'],['/reset','Reset configuration'],['/config','Reset WiFi'],['/update','Upload firmware'],['/','API directory']]
        .map(([href,label])=>`<a class="file-row" href="${href}">${label} ${icon('arrow')}</a>`).join('')+'</article></div>';
  },
  // Build the global settings, runtime debug and syslog configuration forms.
  settings() {
    const g=state.global,s=state.syslog;
    return `<div class="grid"><article class="card panel"><h2>Global settings</h2><form data-form="global"><div class="fields">`+
      select('Temperature unit','unit',state.unit,[['C','Celsius (°C)'],['F','Fahrenheit (°F)']])+
      select('Probe resolution','resolution',g.resolution,[[9,'9 bit'],[10,'10 bit'],[11,'11 bit'],[12,'12 bit']])+
      field('Alarm delay (seconds)','alarm',g.alarm,'number','min="0" max="3600" step="1" required')+'</div>'+
      toggle('mDNS discovery','mdns',g.mdns)+toggle('Check for firmware updates','updates',g.updates)+
      toggle('Send crash reports','crashes',g.crashes)+actions()+'</form></article>'+
      `<article class="card panel"><h2>Debug mode</h2><form data-form="debug-mode">`+
      toggle('Enable debug mode','enabled',g.debug,'Shows runtime sensor overrides in fermenter settings.')+actions('Apply debug mode')+'</form></article>'+
      `<article class="card panel"><h2>Remote syslog</h2><form data-form="syslog">`+
      toggle('Enable syslog','enabled',s.enabled)+`<div class="fields">`+field('Server host','host',s.host)+
      field('Port','port',s.port,'number','min="1" max="65535" step="1" required')+
      select('Facility','facility',s.facility,Array.from({length:24},(_,i)=>[i,i>=16?'Local '+(i-16):'Facility '+i]))+
      select('Minimum level','level',s.minLevel,['Emergency','Alert','Critical','Error','Warning','Notice','Info','Debug'].map((n,i)=>[i,n]))+
      '</div>'+actions()+'</form></article></div>';
  },
  // Build backup actions and the saved-file preview list.
  files() {
    return `<div class="system-layout"><article class="card panel"><h2>Configuration backup</h2><p class="subtitle">Export saved configuration files. Backups can contain credentials; keep them private.</p>`+
      `<div class="two-col-actions" style="margin-top:20px">${btn('Export configuration','export','secondary')}${btn('Import configuration…','import','ghost')}</div>`+
      `<input type="file" id="import-file" accept=".json" hidden><p id="backup-result" class="review-caption" role="status"></p></article>`+
      `<article class="card panel"><h2>LittleFS files</h2><div class="file-list">${state.files.map((f,i)=>
        `<button class="file-row" data-action="file-view" data-id="${i}"><span>${icon('file')} ${esc(f.name)}</span><small>${finite(f.size)} B</small></button>`).join('')}</div></article></div>`;
  },
  // Build RF listening controls and explicit tests for configured plugs.
  rf() {
    return `<article class="card panel"><h2>RF code sniffer</h2><p class="notice info">Start listening to enable the receiver. Tests transmit the selected plug's saved codes and operate the real plug.</p>`+
      `<div class="two-col-actions">${btn('Start listening','console-start','secondary','data-value="rf"')}${btn('Stop listening','console-stop','ghost')}${btn('Clear','rf-clear','ghost')}</div>`+
      `<div class="console" id="rf-output">Receiver polling is stopped.</div><h3>Test a configured plug</h3>`+
      `<div class="console-input"><select id="rf-plug" aria-label="Configured plug">${state.plugs.filter(p=>p.onCode||p.offCode).map(p=>
        `<option value="${p.index}">Plug ${p.index} · ${esc(p.manufacturer)} ${esc(p.model)}</option>`).join('')}</select>`+
      btn('Test on','rf-test','secondary','data-value="on"')+btn('Test off','rf-test','ghost','data-value="off"')+'</div></article>';
  },
  // Build BLE console controls, common AT commands and the manual command form.
  ble() {
    return `<article class="card panel"><h2>BLE AT console</h2><p class="notice">Tilt scanning pauses while this console is active. It resumes after polling stops and the device's timeout expires.</p>`+
      `<div class="two-col-actions">${btn('Start console','console-start','secondary','data-value="ble"')}${btn('Stop console','console-stop','ghost')}</div>`+
      `<div class="command-buttons">${['AT','AT+VERS?','AT+ROLE?','AT+IMME?','AT+DISI?','AT+ADDR?','AT+BAUD?'].map(c=>btn(c,'ble-send','small secondary',`data-value="${c}"`)).join('')}</div>`+
      `<div class="console" id="ble-output">Console polling is stopped.</div><form id="ble-form" class="console-input"><input name="cmd" maxlength="64" required placeholder="Enter AT command…" aria-label="AT command"><button class="button" type="submit">Send</button>${btn('Clear','ble-clear','ghost')}</form></article>`;
  },
  // Build firmware upload navigation and the current release-check result.
  update() {
    const c=state.controller;
    return `<div class="system-layout"><article class="card panel"><h2>Firmware update</h2><p class="subtitle">Current firmware: ${esc(shown(c.FirmwareVersion))}. The standalone uploader accepts a local .bin file and remains usable if the main controls fail to load.</p>`+
      `<a class="button secondary" style="display:inline-block;margin-top:20px" href="/update">Open firmware uploader</a></article>`+
      `<article class="card panel"><h2>Release check</h2><p class="subtitle">Requires internet access. Local controls work offline.</p>`+
      btn('Check for updates','update-check','secondary')+`<p id="update-result" class="review-caption">${updateResult(c)}</p></article></div>`;
  }
};

// Summarize the controller's latest release-check state.
function updateDescription(c) {
  if (c.UpdateCheckError) return c.UpdateCheckError;
  if (!c.UpdateChecked) return 'No completed check.';
  return c.UpdateAvailable?'Available version: '+c.LatestVersion:'No newer release found. Last check age: '+shown(c.LastUpdateCheck)+' min';
}

// Build escaped release-check text and a validated release-notes link.
function updateResult(controller) {
  const description=esc(updateDescription(controller));
  if (!controller.UpdateAvailable) return description;
  const url=/^https?:\/\//.test(controller.UpdateNotesUrl)?controller.UpdateNotesUrl:'https://github.com/scardus/OurBrewbot/releases';
  return description+` · <a href="${esc(url)}" target="_blank" rel="noopener noreferrer">Release notes</a>`;
}

// Refresh reading-age labels, temperature units and the connection indicator.
function updateAge() {
  $('#unit-toggle').textContent='°'+state.unit;
  const age=state.lastUpdated?Math.floor((Date.now()-state.lastUpdated)/1000):null;
  let description='Updated '+age+'s ago';
  if (state.scenario==='offline') description='Connection lost · last update '+shown(age)+'s ago';
  else if (state.paused) description='Refresh paused';
  else if (age==null) description='Waiting for readings';
  else if (age<3) description='Updated just now';
  $$('[data-age]').forEach(el=>{ el.textContent=description; });
  $('#connection').innerHTML=state.scenario==='offline'
    ? '<i style="background:var(--amber)"></i> Connection lost'
    : '<i></i> Controller online';
}
