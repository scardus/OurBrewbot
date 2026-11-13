'use strict';
// The alert banners under the top bar: alarms, sensor problems, restarts, finished profiles and new firmware.

// How each kind of alert from /alerts is shown, most serious first.
// order sorts the banners, style picks the colour and link adds one to the page that can help.
const bannerKinds = {
  alarm:{order:0,style:'alarm'},
  sensor:{order:1,style:'warning',link:['#sensors','Open sensors']},
  restart:{order:2,style:'warning',link:['#system','Open system']},
  profile:{order:3,style:'success'},
  update:{order:4,style:'success'}
};
const MAX_SHOWN_BANNERS = 3;      // more than this are behind a "+N more alerts" row
const ALERTS_POLL_MS = 30000;
const MAX_CLOSED_ALERTS = 100;    // the closed list only grows when the brewer closes banners
const CLOSED_ALERTS_KEY = 'ourbrewbot.closedAlerts';

let lastAlertsPoll = 0;
let alertsBusy = false;
let bannersExpanded = false;
// Alerts closed on this page. Browser storage keeps them across reloads, but it can be
// blocked or missing, so closing still works on this page without it.
const closedThisPage = new Set();

// Read the ids of the banners closed in this browser.
function closedAlerts() {
  let saved=[];
  try { saved=JSON.parse(localStorage.getItem(CLOSED_ALERTS_KEY)) || []; } catch { saved=[]; }
  if (!Array.isArray(saved)) saved=[];
  return [...new Set([...saved,...closedThisPage])];
}

// Keep the ids of closed banners in this browser, ignoring storage that is blocked or full.
function saveClosedAlerts(ids) {
  try { localStorage.setItem(CLOSED_ALERTS_KEY,JSON.stringify(ids.slice(-MAX_CLOSED_ALERTS))); } catch {}
}

// Forget closed banners from earlier boots. One from this boot is kept even while its alert is
// missing from /alerts: an alarm that flips off and back on keeps its id, and must stay closed.
function forgetOldClosedAlerts() {
  if (!state.alertsBoot) return;
  const current=state.alerts.map(alert=>alert.id);
  const closed=closedAlerts();
  const kept=closed.filter(id=>current.includes(id) || id.startsWith(state.alertsBoot+'-'));
  if (kept.length!==closed.length) saveClosedAlerts(kept);
}

// The alerts to show: most serious first, newest first within a kind, without the closed ones.
function shownAlerts() {
  const closed=closedAlerts();
  return state.alerts.filter(alert=>bannerKinds[alert.kind] && !closed.includes(alert.id))
    .sort((a,b)=>bannerKinds[a.kind].order-bannerKinds[b.kind].order || a.age-b.age);
}

// Build the links under an alert's text.
function bannerLinks(alert) {
  const link=bannerKinds[alert.kind].link;
  if (link) return `<a href="${link[0]}">${link[1]}</a>`;
  if (alert.kind!=='update') return '';
  return `<a href="${esc(releaseNotesUrl(state.controller))}" target="_blank" rel="noopener noreferrer">Release notes</a>
    <button type="button" class="banner-link" data-action="open-firmware">Firmware update</button>`;
}

// Build one banner with its text, links and close button.
function bannerHtml(alert) {
  const style=bannerKinds[alert.kind].style;
  return `<div class="banner ${style}">${icon(style==='success'?'shield':'info')}
    <div class="banner-text"><strong>${esc(alert.title)}</strong> ${esc(alert.text)} ${bannerLinks(alert)}</div>
    <button type="button" class="banner-close" data-action="close-alert" data-value="${esc(alert.id)}"
      aria-label="Close this alert">${icon('close')}</button></div>`;
}

// Draw the banners: the first three, and the rest once the "+N more alerts" row is opened.
function renderBanners() {
  const alerts=shownAlerts();
  const shown=bannersExpanded?alerts:alerts.slice(0,MAX_SHOWN_BANNERS);
  const hidden=alerts.length-shown.length;
  let toggle='';
  if (hidden) toggle=`<button type="button" class="banner-more" data-action="toggle-alerts"
    aria-expanded="false">+${hidden} more alert${hidden===1?'':'s'}</button>`;
  else if (bannersExpanded && alerts.length>MAX_SHOWN_BANNERS) toggle=`<button type="button" class="banner-more"
    data-action="toggle-alerts" aria-expanded="true">Show fewer alerts</button>`;
  $('#banners').innerHTML=shown.map(bannerHtml).join('')+toggle;
}

// Hide one alert's banner in this browser until a new occurrence gets a new id.
function closeAlert(id) {
  closedThisPage.add(id);
  saveClosedAlerts([...closedAlerts().filter(saved=>saved!==id),id]);
  renderBanners();
}

// Read the alerts and redraw the banners. After a failed read the last banners stay up;
// the page's connection notice already says the controller is unreachable.
async function pollAlerts() {
  if (alertsBusy || saving || document.hidden || !state.ready) return;
  alertsBusy=true;
  try {
    const reply=await get('/alerts',true);
    state.alerts=Array.isArray(reply.alerts)?reply.alerts:[];
    state.alertsBoot=String(reply.boot||'');
    forgetOldClosedAlerts();
    renderBanners();
  } catch { /* keep the last banners */ }
  finally { alertsBusy=false; lastAlertsPoll=Date.now(); }
}
