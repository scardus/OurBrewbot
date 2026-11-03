'use strict';
// Drawing the current view, live updates and the shared button handler.

const navItems = [
  ['dashboard','Dashboard'],['profiles','Profiles'],['sensors','Sensors'],
  ['plugs','Smart plugs'],['reporting','Reporting'],['system','System & diagnostics']
];

// Close the mobile navigation and update its accessibility state.
function closeNav() {
  $('#sidebar').classList.remove('open');
  $('#nav-scrim').hidden=true;
  $('#menu-toggle').setAttribute('aria-expanded','false');
}

// Read the view name from the page address, falling back to the dashboard.
function currentRoute() {
  const requested=location.hash.slice(1).split('/')[0];
  return navItems.some(item=>item[0]===requested)?requested:'dashboard';
}

// Render the selected route while restoring drafts and horizontal scroll positions.
function render() {
  state.route=currentRoute();
  const routeLabel=navItems.find(item=>item[0]===state.route)[1];
  const main=$('#main');
  const sameView=main.dataset.route===state.route;
  // Replacing the view must not move a horizontal list the user has scrolled.
  const profileScroll=sameView?$('.profile-list',main)?.scrollLeft:0;
  const tabScroll=sameView?$('.tabs',main)?.scrollLeft:0;
  $('#navigation').innerHTML=navItems.map(([key,label])=>`
    <a href="#${key}" class="nav-item ${state.route===key?'active':''}" ${state.route===key?'aria-current="page"':''}>
      ${icon(key)}<span>${label}</span>
      ${key==='dashboard'?`<span class="nav-count">${state.fermenters.length}</span>`:''}</a>`).join('');
  $('#crumb').textContent=routeLabel;
  $('.footer-version').textContent=state.controller.FirmwareVersion||'';
  const renderers={dashboard:dashboardView,profiles:profilesView,sensors:sensorsView,plugs:plugsView,
    reporting:reportingView,system:systemView};
  const offline=state.offline?`<div class="notice error">${icon('info')}<div>
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
  document.title=routeLabel+' · OurBrewbot';
}

// Update live text and status badges without replacing editing forms.
function updateLiveReadings() {
  $$('[data-controller]').forEach(node=>{
    node.textContent=controllerValue(node.dataset.controller);
  });
  $$('[data-plug-status]').forEach(node=>{
    const plug=state.plugs[+node.dataset.plugStatus];
    if (!plug) return;
    node.innerHTML=plugStatusBadge(plug);
  });
  $$('[data-sensor-status]').forEach(node=>{
    const [kind,index]=node.dataset.sensorStatus.split('.'),sensor=state[kind]?.[+index];
    if (!sensor) return;
    node.innerHTML=sensorTypes[kind].status(sensor);
  });
  $$('[data-live]').forEach(node=>{
    const [kind,index,name]=node.dataset.live.split('.');
    const sensor=state[kind]?.[+index];
    if (!sensor) return;
    let reading=orDash(sensor[name]);
    if (name==='temp') reading=formatTemp(sensor.temp)+' °'+state.unit;
    else if (name==='detail') reading=spindelDetail(sensor);
    else if (name==='sg') reading=formatGravity(sensor.sg);
    else if (name==='battery') reading=toFinite(sensor.battery).toFixed(2);
    node.textContent=reading;
  });
  updateAge();
}

// Refresh reading-age labels, temperature units and the connection indicator.
function updateAge() {
  $('#unit-label').textContent='°'+state.unit;
  const age=state.lastUpdated?Math.floor((Date.now()-state.lastUpdated)/1000):null;
  let description='Updated '+age+'s ago';
  if (state.offline) description='Connection lost · last update '+orDash(age)+'s ago';
  else if (state.refreshPaused) description='Refresh paused';
  else if (age==null) description='Waiting for readings';
  else if (age<3) description='Updated just now';
  $$('[data-age]').forEach(el=>{ el.textContent=description; });
  $('#connection').innerHTML=state.offline
    ? '<i style="background:var(--amber)"></i> Connection lost'
    : '<i></i> Controller online';
}

// Post a command and show the controller's reply.
async function postAndShowReply(path,body) {
  const reply=await post(path,body);
  toast(reply.msg);
}

// Switch to another tab of the current view, stopping any running diagnostics console.
function selectTab(tabField,value) {
  state.activeConsole=null;
  state[tabField]=value;
  return loadRoute();
}

// What each data-action button does. Each handler receives the button's data-id as a number,
// its data-value, and the button itself. Substantial operations are named functions in their feature files.
const actionHandlers = {
  // Page controls.
  refresh:()=>refreshForms(),
  reconnect:()=>refreshForms(),
  pause:()=>{ state.refreshPaused=!state.refreshPaused; render(); },
  'close-modal':()=>$('#modal').close(),
  'discard-form':(index,value,button)=>discardForm(button.closest('form')),
  // Fermenters.
  'edit-ferm':index=>openFerm(index),
  'close-editor':()=>$('#editor').close(),
  'profile-control':(index,value)=>controlProfile(index,value),
  debug:index=>showDebug(index),
  // Profiles.
  'select-profile':index=>{ state.selectedProfile=index; render(); },
  'copy-profile':()=>chooseProfileCopy(),
  'copy-confirm':()=>copyProfile(),
  'step-add':index=>editProfileStep('step-add',index),
  'step-up':index=>editProfileStep('step-up',index),
  'step-down':index=>editProfileStep('step-down',index),
  'step-delete':index=>editProfileStep('step-delete',index),
  // Sensors.
  'sensor-tab':(index,value)=>selectTab('sensorTab',value),
  'clear-sensor':(index,value,button)=>clearSensor(button.dataset.kind,index),
  // Smart plugs.
  'plug-preset':(index,value,button)=>applyPreset(button.closest('form')),
  'plug-test':(index,value)=>testPlug(state.plugs[index].index,value),
  // Reporting.
  'test-service':index=>postAndShowReply('/brewservices/test',{index}),
  'test-mqtt':()=>postAndShowReply('/mqtt/test',{}),
  discover:()=>postAndShowReply('/mqtt/discover',{}),
  // System.
  'system-tab':(index,value)=>selectTab('systemTab',value),
  'open-rf':()=>{ state.systemTab='rf'; location.hash='system'; return loadRoute(); },
  export:()=>exportConfig(),
  import:()=>$('#import-file').click(),
  'file-view':index=>viewFile(index),
  'console-start':(index,value)=>{ state.activeConsole=value; lastConsolePoll=0; updateConsoleMessage(); },
  'console-stop':()=>{ state.activeConsole=null; updateConsoleMessage(); },
  'rf-test':(index,value)=>testPlug(+$('#rf-plug').value,value),
  'rf-clear':()=>{ $('#rf-output').textContent=''; },
  'ble-clear':()=>{ $('#ble-output').textContent=''; },
  'ble-send':(index,value)=>sendBle(value),
  'update-check':async()=>{
    Object.assign(state.controller,await post('/update/check',{},{timeout:30000}));
    $('#update-result').innerHTML=updateResult(state.controller);
  }
};

// Run a data-action button's handler. While a save runs, a dialog can still be closed but nothing else can start.
async function handleAction(button) {
  const name=button.dataset.action;
  if (saving && name!=='close-modal' && name!=='close-editor') return;
  await actionHandlers[name](+button.dataset.id,button.dataset.value,button);
}
