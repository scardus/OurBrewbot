'use strict';
// Event wiring, timers and page start-up. Loaded last, because it runs immediately.

// Track typed form edits in browser drafts without saving to the controller.
document.addEventListener('input',event=>{
  if (event.target.closest('form[data-pressure-point]')) {
    state.pressureReference=event.target.value; updatePressureControls(); return;
  }
  const form=event.target.closest('form[data-form]');
  if (!form) return;
  if (form.dataset.form.startsWith('profile-')) profileChanged();
  updateDirty(form);
  if (form.dataset.form==='pressure') updatePressureControls();
});

// Update changed controls, rebuild profile fields when needed and accept an import file.
document.addEventListener('change',event=>{
  const form=event.target.closest('form[data-form]');
  if (form) {
    if (form.dataset.form.startsWith('profile-')) {
      profileChanged();
      if (event.target.name.startsWith('type-')) { formDrafts.delete(form.dataset.form); render(); }
    } else updateDirty(form);
    if (form.dataset.form==='pressure') updatePressureControls();
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
  if (form.hasAttribute('data-pressure-point')) capturePressurePoint(form).catch(error=>showPressureResult(error.message,true));
  else if (form.dataset.form) saveForm(form);
  else if (form.id==='ble-form') sendBle(form.elements.cmd.value).catch(error=>toast(error.message));
  else if (form.dataset.debug!==undefined) saveDebug(form).catch(error=>toast(error.message));
});

// Delegate action-button clicks to the shared action handler.
document.addEventListener('click',event=>{
  const target=event.target.closest('[data-action]');
  if (target) handleAction(target).catch(error=>toast(error.message));
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
$('#log-pause').onclick=()=>{
  state.logPaused=!state.logPaused;
  $('#log-pause').textContent=state.logPaused?'Resume':'Pause';
};

// Clear only the browser-held log entries.
$('#log-clear').onclick=()=>{logLines=[]; renderLogs();};

// Download retained log entries as a plain-text file.
$('#log-download').onclick=()=>{
  download('ourbrewbot.log',logLines.map(l=>l[0]+' '+l[1]+' '+l[2]).join('\n'),'text/plain');
};

// Ask the browser to warn before leaving a tab with unsaved drafts.
window.addEventListener('beforeunload',event=>{
  if (formDrafts.size || profileDrafts.size) { event.preventDefault(); event.returnValue=''; }
});

// Load the newly selected route and stop the previous diagnostics console.
window.addEventListener('hashchange',()=>{
  state.activeConsole=null;
  state.route=currentRoute();
  if (state.ready) loadRoute();
  window.scrollTo(0,0);
});

// Refresh stale readings when the browser tab becomes visible again.
document.addEventListener('visibilitychange',()=>{
  if (!document.hidden) { state.pressureReadAt=0; lastPoll=0; lastLogPoll=0; lastAlertsPoll=0; refreshVisible(); }
});

// Schedule reading, alert, log and console polling while the tab is visible and ready.
// Logs are read quickly while the controller has more waiting, and more often with the drawer open.
const READINGS_POLL_MS = 10000;
setInterval(()=>{
  if (document.hidden || !state.ready) return;
  updateAge();
  updatePressureProgress();
  updatePressureControls();
  const readingsDelay=state.route==='sensors' && state.sensorTab==='pressure' && state.pressure.active?2000:READINGS_POLL_MS;
  if (Date.now()-lastPoll>=readingsDelay) refreshVisible();
  if (Date.now()-lastAlertsPoll>=ALERTS_POLL_MS) pollAlerts();
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
    state.route=currentRoute();
    await loadRoute();
  } catch (error) {
    $('#main').textContent=error.message+' Reload this page to reconnect.';
  }
})();
