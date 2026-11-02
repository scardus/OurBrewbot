'use strict';

// Show a confirmation dialog and run its callback only after the user accepts.
function confirmAction(heading,description,label,callback) {
  showModal(`<h2>${esc(heading)}</h2><p>${esc(description)}</p><div class="modal-actions">${btn('Cancel','close-modal','ghost')}<button class="button" id="confirm-action">${esc(label)}</button></div>`);
  // Run the accepted action once and show any reported failure.
  $('#confirm-action').onclick=async()=>{
    $('#confirm-action').disabled=true;
    $('#modal').close();
    try { await callback(); } catch (error) { toast(error.message); }
  };
}

// Read current runtime overrides and open their fermenter-specific editor.
async function showDebug(id) {
  await fetchData('debug');
  const f=state.fermenters[id],override=state.debug.Overrides.find(v=>+v.Fermenter===f.id)||{};
  showModal(`<h2>Debug overrides · ${esc(f.name)}</h2><p>Runtime only; resets on reboot.</p><form data-debug="${id}" data-unit="${state.unit}">`+
    toggle('Enable overrides','enabled',override.Enabled)+`<div class="fields">`+
    field('Beer temperature (°'+state.unit+')','beer',override.BeerTemp??temp(f.beerTemp),'number','step="0.1" required')+
    field('Ambient temperature (°'+state.unit+')','ambient',override.AmbientTemp??temp(f.ambient),'number','step="0.1" required')+
    field('Specific gravity','sg',override.SG??f.sg,'number','step="0.0001" required')+
    `</div><button class="button" type="submit">Apply overrides</button></form>`);
}

// Append a log entry while retaining at most two thousand entries in the browser.
function addLog(text,level='info',timestamp='') {
  logLines.push([timestamp,level,text]);
  if (logLines.length>2000) logLines.splice(0,logLines.length-2000);
}

// Apply log search and level filters, following new entries only when already at the end.
function renderLogs() {
  const q=$('#log-search').value.toLowerCase(),level=$('#log-filter').value;
  const node=$('#log-lines'),follow=node.scrollHeight-node.scrollTop-node.clientHeight<30;
  const filtered=logLines.filter(l=>(level==='all'||l[1]===level)&&l[2].toLowerCase().includes(q));
  node.innerHTML=filtered.map(l=>`<div class="log-line ${l[1]}"><time>${esc(l[0])}</time>${esc(l[2])}</div>`).join('') || '<div class="empty-state">No matching messages.</div>';
  $('.log-count').textContent=logLines.length;
  if (follow) node.scrollTop=node.scrollHeight;
}

let logBusy=false;
// Read incremental log entries and recover the cursor after a controller restart or rejection.
async function pollLogs() {
  if (logBusy || state.logPaused || saving || document.hidden || !state.ready) return;
  logBusy=true;
  try {
    const reply=await request('/logs?boot='+encodeURIComponent(logBoot)+'&after='+logAfter,{text:true},true);
    const boot=reply.headers.get('X-Log-Boot')||'';
    if (logBoot && logBoot!==boot) addLog('Controller restarted. Log cursor reset.','warning');
    const lost=finite(reply.headers.get('X-Log-Lost'));
    if (lost) addLog(lost+' controller log lines were missed.','warning');
    logBoot=boot;
    logAfter=finite(reply.headers.get('X-Log-Next'),logAfter);
    logMore=reply.headers.get('X-Log-More')==='1';
    for (const line of reply.data.split('\n')) {
      const match=/^(\d)\s+(\S+)\s+(.*)$/.exec(line);
      if (match) addLog(match[3],+match[1]<=3?'error':+match[1]===4?'warning':'info',match[2]);
    }
    renderLogs();
    $('.log-bar-status').textContent='Controller stream';
  } catch (error) {
    // A rejected cursor will never recover by sending it again.
    if (error.httpStatus===400) { logBoot=''; logAfter=0; logMore=false; }
    $('.log-bar-status').textContent=error.httpStatus===400?'Log cursor reset · reconnecting':'Log connection lost · retrying';
  }
  finally { logBusy=false; lastLogPoll=Date.now(); }
}

let lastConsolePoll=0,consoleBusy=false;
// Poll the active RF or BLE console at its own interval and bound the displayed output.
async function pollConsole() {
  const kind=state.console;
  if (!kind || consoleBusy || saving || document.hidden || state.route!=='system' || state.systemTab!==kind) return;
  if (Date.now()-lastConsolePoll<(kind==='rf'?500:300)) return;
  consoleBusy=true;
  try {
    const data=await get('/'+kind+'/sniff/poll',true);
    const output=$('#'+kind+'-output');
    if (!output) return;
    if (kind==='ble' && data.data) output.textContent+=data.data;
    if (kind==='rf' && data.received) output.textContent+='\nCode '+data.code+' ('+data.hex+') · '+data.bits+' bits · '+data.delay+' µs · protocol '+data.protocol;
    if (output.textContent.length>12000) output.textContent=output.textContent.slice(-12000);
  } catch (error) { toast(error.message); state.console=null; }
  finally { consoleBusy=false; lastConsolePoll=Date.now(); }
}

// Track typed form edits in browser drafts without saving to the controller.
document.addEventListener('input',event=>{
  const form=event.target.closest('form[data-form]');
  if (!form) return;
  if (form.dataset.form.startsWith('profile-')) profileChanged();
  updateDirty(form);
});
// Update changed controls, rebuild profile fields when needed and accept an import file.
document.addEventListener('change',event=>{
  const form=event.target.closest('form[data-form]');
  if (form) {
    if (form.dataset.form.startsWith('profile-')) {
      profileChanged();
      if (event.target.name.startsWith('type-')) { drafts.delete(form.dataset.form); render(); }
    } else updateDirty(form);
  }
  if (event.target.id==='import-file' && event.target.files[0]) {
    const file=event.target.files[0];
    event.target.value='';
    importConfig(file).catch(error=>toast(error.message));
  }
});
// Send explicit form submissions to the appropriate validated save or command handler.
document.addEventListener('submit',event=>{
  event.preventDefault();
  const form=event.target;
  if (form.dataset.form) saveForm(form).catch(error=>saveError(form,error.message));
  else if (form.id==='ble-form') sendBle(form.elements.cmd.value).catch(error=>toast(error.message));
  else if (form.dataset.debug!==undefined) saveDebug(form).catch(error=>toast(error.message));
});

// Validate and apply runtime sensor overrides without retrying an uncertain result.
async function saveDebug(form) {
  const button=$('button[type="submit"]',form);
  if (button.disabled || form.dataset.unknown) return;
  button.disabled=true;
  try {
    await fetchData('controller');
    if (form.dataset.unit!==state.unit) throw new Error('Controller units changed. Close and reopen debug overrides before applying.');
    const id=+form.dataset.debug,d=formData(form);
    if (![d.beer,d.ambient,d.sg].every(v=>v!=='' && Number.isFinite(+v))) throw new Error('Enter finite numbers for debug readings.');
    await post('/debug',{Fermenter:state.fermenters[id].id,Enabled:d.enabled,BeerTemp:+d.beer,AmbientTemp:+d.ambient,SG:+d.sg});
    $('#modal').close();
    await fetchData('debug');
  } catch (error) {
    if (error.unknown) form.dataset.unknown='true';
    toast(error.message+(error.unknown?' Close and reopen overrides to read their state.':''));
  } finally { button.disabled=!!form.dataset.unknown; }
}
// Delegate action-button clicks to the shared action handler.
document.addEventListener('click',event=>{
  const target=event.target.closest('[data-action]');
  if (target) action(target).catch(error=>toast(error.message));
});

// Toggle mobile navigation and keep its accessibility state in sync.
$('#menu-toggle').onclick=()=>{
  const open=$('#sidebar').classList.toggle('open');
  $('#nav-scrim').hidden=!open;
  $('#menu-toggle').setAttribute('aria-expanded',String(open));
};
$('#nav-scrim').onclick=closeNav;
// Move keyboard focus directly to the page content.
$('.skip').onclick=event=>{event.preventDefault(); $('#main').focus();};
// Open or close the log drawer, following the newest entries whenever it opens.
$('#log-toggle').onclick=()=>{
  const node=$('#log-content'); node.hidden=!node.hidden;
  $('#log-toggle').setAttribute('aria-expanded',String(!node.hidden)); renderLogs();
  if (!node.hidden) {
    const log=$('#log-lines');
    log.scrollTop=log.scrollHeight;
  }
};
$('#log-search').oninput=renderLogs;
$('#log-filter').onchange=renderLogs;
// Pause or resume log polling and update the button label.
$('#log-pause').onclick=()=>{state.logPaused=!state.logPaused; $('#log-pause').textContent=state.logPaused?'Resume':'Pause';};
// Clear only the browser-held log entries.
$('#log-clear').onclick=()=>{logLines=[]; renderLogs();};
// Download retained log entries as a plain-text file.
$('#log-download').onclick=()=>download('ourbrewbot.log',logLines.map(l=>l[0]+' '+l[1]+' '+l[2]).join('\n'),'text/plain');
// Clear the active editor selection when the drawer is dismissed.
$('#editor').addEventListener('cancel',()=>{state.editorId=null;});
// Ask the browser to warn before leaving a tab with unsaved drafts.
window.addEventListener('beforeunload',event=>{
  if (drafts.size || profileDrafts.size) { event.preventDefault(); event.returnValue=''; }
});
// Load the newly selected route and stop the previous diagnostics console.
window.addEventListener('hashchange',()=>{
  state.console=null;
  const requested=location.hash.slice(1).split('/')[0];
  state.route=navItems.some(n=>n[0]===requested)?requested:'dashboard';
  if (state.ready) loadRoute();
  window.scrollTo(0,0);
});
// Refresh stale readings when the browser tab becomes visible again.
document.addEventListener('visibilitychange',()=>{
  if (!document.hidden) { lastPoll=0; lastLogPoll=0; refreshVisible(); }
});
// Schedule reading, log and console polling while the tab is visible and ready.
setInterval(()=>{
  if (document.hidden || !state.ready) return;
  updateAge();
  if (Date.now()-lastPoll>=10000) refreshVisible();
  const delay=logMore?250:$('#log-content').hidden?3000:1000;
  if (Date.now()-lastLogPoll>=delay) pollLogs();
  pollConsole();
},250);

// Load initial controller data before displaying the requested view.
(async()=>{
  try {
    await fetchData('controller');
    await fetchData('fermenters');
    await fetchData('profiles');
    state.ready=true;
    state.lastUpdated=Date.now();
    state.route=navItems.some(n=>n[0]===location.hash.slice(1))?location.hash.slice(1):'dashboard';
    await loadRoute();
  } catch (error) {
    $('#main').textContent=error.message+' Reload this page to reconnect.';
  }
})();
