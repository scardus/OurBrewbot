'use strict';
// The system view: settings, backup, firmware and the RF and BLE consoles.

// Build the System tabs and the currently selected diagnostics panel.
function systemView() {
  const tabs=[['overview','Controller'],['settings','Settings'],['files','Files & backup'],
    ['rf','RF sniffer'],['ble','BLE console'],['update','Firmware']];
  return pageTitle('LOOK AFTER YOUR CONTROLLER','System & diagnostics',
    'Settings, maintenance and tools for your brew room.')+`
    ${tabBar('System section','system-tab',tabs,state.systemTab)}
    ${systemPanels[state.systemTab]()}`;
}

const systemPanels={
  // Build controller information and links to the standalone maintenance pages.
  overview() {
    const details=[
      ['Firmware','FirmwareVersion'],['WiFi network','WiFiSSID'],['IP address','IP'],['mDNS name','mDNSName'],
      ['Signal strength','RSSI'],['Free heap','FreeHeap'],['Uptime','Uptime'],
      ['Chip ID','ChipId'],['Reset reason','resetReason']
    ].map(([label,key])=>`
      <div class="list-row">
        <span>${esc(label)}</span><strong data-controller="${key}">${esc(controllerValue(key))}</strong>
      </div>`).join('');
    const links=[
      ['/reboot','Restart controller'],['/reset','Reset configuration'],['/config','Reset WiFi'],
      ['/update','Upload firmware'],['/','API directory']
    ].map(([href,label])=>`<a class="file-row" href="${href}">${label} ${icon('arrow')}</a>`).join('');
    return `<div class="system-layout">
      <article class="card panel"><h2>Controller information</h2>${details}</article>
      <article class="card panel"><h2>Maintenance</h2>
        <p class="subtitle">These links open the controller's standalone confirmation pages.</p>
        ${links}
      </article>
    </div>`;
  },
  // Build the global settings, runtime debug and syslog configuration forms.
  settings() {
    const g=state.global,s=state.syslog;
    const facilities=Array.from({length:24},(_,i)=>[i,i>=16?'Local '+(i-16):'Facility '+i]);
    const resolutions=[[9,'9 bit'],[10,'10 bit'],[11,'11 bit'],[12,'12 bit']];
    const levels=['Emergency','Alert','Critical','Error','Warning','Notice','Info','Debug'].map((name,i)=>[i,name]);
    return `<div class="grid">
      <article class="card panel"><h2>Global settings</h2>
        <form data-form="global">
          <div class="fields">
            ${select('Temperature unit','unit',state.unit,[['C','Celsius (°C)'],['F','Fahrenheit (°F)']])}
            ${select('Probe resolution','resolution',g.resolution,resolutions)}
            ${field('Alarm delay (seconds)','alarm',g.alarm,'number','min="0" max="3600" step="1" required')}
          </div>
          ${toggle('mDNS discovery','mdns',g.mdns)}
          ${toggle('Check for firmware updates','updates',g.updates)}
          ${toggle('Send crash reports','crashes',g.crashes)}
          ${formActions()}
        </form>
      </article>
      <article class="card panel"><h2>Debug mode</h2>
        <form data-form="debug-mode">
          ${toggle('Enable debug mode','enabled',g.debug,'Shows runtime sensor overrides in fermenter settings.')}
          ${formActions('Apply debug mode')}
        </form>
      </article>
      <article class="card panel"><h2>Remote syslog</h2>
        <form data-form="syslog">
          ${toggle('Enable syslog','enabled',s.enabled)}
          <div class="fields">
            ${field('Server host','host',s.host)}
            ${field('Port','port',s.port,'number','min="1" max="65535" step="1" required')}
            ${select('Facility','facility',s.facility,facilities)}
            ${select('Minimum level','level',s.minLevel,levels)}
          </div>
          ${formActions()}
        </form>
      </article>
    </div>`;
  },
  // Build backup actions and the saved-file preview list.
  files() {
    const files=state.files.map((f,i)=>`
      <button class="file-row" data-action="file-view" data-id="${i}">
        <span>${icon('file')} ${esc(f.name)}</span><small>${toFinite(f.size)} B</small>
      </button>`).join('');
    return `<div class="system-layout">
      <article class="card panel"><h2>Configuration backup</h2>
        <p class="subtitle">Export saved configuration files. Backups can contain credentials; keep them private.</p>
        <div class="two-col-actions" style="margin-top:20px">
          ${btn('Export configuration','export','secondary')}
          ${btn('Import configuration…','import','ghost')}
        </div>
        <input type="file" id="import-file" accept=".json" hidden>
      </article>
      <article class="card panel"><h2>LittleFS files</h2>
        <div class="file-list">${files}</div>
      </article>
    </div>`;
  },
  // Build RF listening controls and explicit tests for configured plugs.
  rf() {
    const plugChoices=state.plugs.filter(p=>p.onCode||p.offCode).map(p=>
      `<option value="${p.index}">Plug ${p.index} · ${esc(p.manufacturer)} ${esc(p.model)}</option>`).join('');
    return `<article class="card panel"><h2>RF code sniffer</h2>
      <p class="notice info">Start listening to enable the receiver. Tests transmit the selected plug's saved codes
        and operate the real plug.</p>
      <div class="two-col-actions">
        ${btn('Start listening','console-start','secondary','data-value="rf"')}
        ${btn('Stop listening','console-stop','ghost')}
        ${btn('Clear','rf-clear','ghost')}
      </div>
      <div class="console" id="rf-output">Receiver polling is stopped.</div>
      <h3>Test a configured plug</h3>
      <div class="console-input">
        <select id="rf-plug" aria-label="Configured plug">${plugChoices}</select>
        ${btn('Test on','rf-test','secondary','data-value="on"')}
        ${btn('Test off','rf-test','ghost','data-value="off"')}
      </div>
    </article>`;
  },
  // Build BLE console controls, common AT commands and the manual command form.
  ble() {
    const commands=['AT','AT+VERS?','AT+ROLE?','AT+IMME?','AT+DISI?','AT+ADDR?','AT+BAUD?'];
    return `<article class="card panel"><h2>BLE AT console</h2>
      <p class="notice">Tilt scanning pauses while this console is active. It resumes after polling stops
        and the device's timeout expires.</p>
      <div class="two-col-actions">
        ${btn('Start console','console-start','secondary','data-value="ble"')}
        ${btn('Stop console','console-stop','ghost')}
      </div>
      <div class="command-buttons">
        ${commands.map(c=>btn(c,'ble-send','small secondary',`data-value="${c}"`)).join('')}
      </div>
      <div class="console" id="ble-output">Console polling is stopped.</div>
      <form id="ble-form" class="console-input">
        <input name="cmd" maxlength="64" required placeholder="Enter AT command…" aria-label="AT command">
        <button class="button" type="submit">Send</button>
        ${btn('Clear','ble-clear','ghost')}
      </form>
    </article>`;
  },
  // Build firmware upload navigation and the current release-check result.
  update() {
    const c=state.controller;
    return `<div class="system-layout">
      <article class="card panel"><h2>Firmware update</h2>
        <p class="subtitle">Current firmware: ${esc(orDash(c.FirmwareVersion))}. The standalone uploader accepts a local
          .bin file and remains usable if the main controls fail to load.</p>
        <a class="button secondary" style="display:inline-block;margin-top:20px"
          href="/update">Open firmware uploader</a>
      </article>
      <article class="card panel"><h2>Release check</h2>
        <p class="subtitle">Requires internet access. Local controls work offline.</p>
        ${btn('Check for updates','update-check','secondary')}
        <p id="update-result" class="review-caption">${updateResult(c)}</p>
      </article>
    </div>`;
  }
};

// Build the global settings JSON. Changing units would invalidate any other open draft.
function globalPayload(data) {
  if (data.unit!==state.unit && formDrafts.size>1) {
    throw new Error('Save or discard other drafts before changing units.');
  }
  return {Unit:unitCode(data.unit),Resolution:+data.resolution,AlarmDwellSec:+data.alarm,
    MdnsEnabled:data.mdns,UpdateCheck:data.updates,CrashReports:data.crashes};
}

// Format controller and reset information with the appropriate units.
function controllerValue(key) {
  const value=key==='resetReason'?state.health.resetReason:state.controller[key];
  if (key==='RSSI') return orDash(value)+' dBm';
  if (key==='FreeHeap') return orDash(value)+' bytes';
  if (key==='Uptime') return orDash(value)+' min';
  return orDash(value);
}

// Summarize the controller's latest release-check state.
function updateDescription(c) {
  if (c.UpdateCheckError) return c.UpdateCheckError;
  if (!c.UpdateChecked) return 'No completed check.';
  if (c.UpdateAvailable) return 'Available version: '+c.LatestVersion;
  return 'No newer release found. Last check age: '+orDash(c.LastUpdateCheck)+' min';
}

// Build escaped release-check text and a validated release-notes link.
function updateResult(controller) {
  const description=esc(updateDescription(controller));
  if (!controller.UpdateAvailable) return description;
  const url=/^https?:\/\//.test(controller.UpdateNotesUrl)?controller.UpdateNotesUrl:
    'https://github.com/scardus/OurBrewbot/releases';
  return description+` · <a href="${esc(url)}" target="_blank" rel="noopener noreferrer">Release notes</a>`;
}

const backupFiles=[['global','/jsonGlobal.txt'],['fermenter','/jsonFermenter.txt'],['probe','/jsonProbe.txt'],
  ['smartplugs','/jsonSmartPlugs.txt'],['profile','/jsonProfile.txt'],['profileSteps','/jsonProfileSteps.txt'],
  ['tilt','/jsonTilt.txt'],['ispindel','/jsoniSpindel.txt'],['brewservices','/jsonBrewServices.txt'],
  ['mqtt','/jsonMqtt.txt'],['syslog','/jsonSyslog.txt']];

// Download a complete backup only after every supported saved file has been read.
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

// Validate a backup and confirm its import, with rollback requested if a write fails.
async function importConfig(file) {
  let bundle;
  try { bundle=JSON.parse(await file.text()); }
  catch { toast('Choose a valid JSON configuration backup.'); return; }
  if (!bundle || !bundle.configs || typeof bundle.configs!=='object' || Array.isArray(bundle.configs)) {
    toast('The backup must contain a configs object.'); return;
  }
  const entries=backupFiles.filter(([key])=>Object.prototype.hasOwnProperty.call(bundle.configs,key));
  if (!entries.length) { toast('No supported configuration files were found.'); return; }
  const description='Import '+entries.length+' saved files from '+file.name+
    '. Existing settings will be replaced and the controller will restart. Unknown fields inside files are preserved.';
  confirmAction('Import configuration?',description,'Import',async()=>{
    saving=true;
    try {
      for (const [key,path] of entries) {
        await request('/fs/save?name='+encodeURIComponent(path),{method:'POST',
          headers:{'Content-Type':'application/json'},body:JSON.stringify(bundle.configs[key])});
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
      toast('Files imported. Restart result unknown; check the controller before restarting it manually. '+
        error.message);
    } finally { saving=false; }
  });
}

// Read a saved file into an escaped preview with a browser download option.
async function viewFile(index) {
  const file=state.files[index];
  const text=(await request('/fs/file?name='+encodeURIComponent(file.name),{text:true})).data;
  showModal(`<h2>${esc(file.name)}</h2><pre class="code-view">${esc(text)}</pre>
    <div class="modal-actions">
      ${btn('Close','close-modal','ghost')}
      <button id="download-file" class="button">Download</button>
    </div>`);
  $('#download-file').onclick=()=>download(file.name,text,'text/plain');
}

// Validate and send an AT command, then select BLE console polling.
async function sendBle(command) {
  if (!command || command.length>64) { toast('Enter an AT command of at most 64 characters.'); return; }
  await post('/ble/sniff/send',{cmd:command});
  state.activeConsole='ble';
}

const MAX_CONSOLE_CHARS = 12000;
let lastConsolePoll=0,consoleBusy=false;

// Poll the active RF or BLE console at its own interval and bound the displayed output.
async function pollConsole() {
  const kind=state.activeConsole;
  if (!kind || consoleBusy || saving || document.hidden || state.route!=='system' || state.systemTab!==kind) return;
  if (Date.now()-lastConsolePoll<(kind==='rf'?500:300)) return;
  consoleBusy=true;
  try {
    const data=await get('/'+kind+'/sniff/poll',true);
    const output=$('#'+kind+'-output');
    if (!output) return;
    if (kind==='ble' && data.data) output.textContent+=data.data;
    if (kind==='rf' && data.received) {
      output.textContent+='\nCode '+data.code+' ('+data.hex+') · '+data.bits+' bits · '+data.delay+' µs · '+
        'protocol '+data.protocol;
    }
    if (output.textContent.length>MAX_CONSOLE_CHARS) output.textContent=output.textContent.slice(-MAX_CONSOLE_CHARS);
  } catch (error) { toast(error.message); state.activeConsole=null; }
  finally { consoleBusy=false; lastConsolePoll=Date.now(); }
}
