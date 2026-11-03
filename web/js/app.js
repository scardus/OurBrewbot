'use strict';

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

// Render the selected route while restoring drafts and horizontal scroll positions.
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

// Keep the small fixed action list here; substantial operations have named helpers above.
// Route a data-action button to its UI operation or explicit controller request.
async function action(target) {
  const actionName=target.dataset.action,index=+target.dataset.id,value=target.dataset.value;
  if (saving && !['close-modal','close-editor'].includes(actionName)) return;
  if (actionName==='edit-ferm') openFerm(index);
  else if (actionName==='close-editor') $('#editor').close();
  else if (actionName==='refresh'||actionName==='reconnect') await refreshForms();
  else if (actionName==='pause') { state.paused=!state.paused; render(); }
  else if (actionName==='discard-form') discardForm(target.closest('form'));
  else if (actionName==='select-profile') { state.profile=index; render(); }
  else if (actionName==='copy-profile') chooseProfileCopy();
  else if (actionName==='copy-confirm') copyProfile();
  else if (actionName.startsWith('step-')) editProfileStep(actionName,index);
  else if (actionName==='profile-control') controlProfile(index,value);
  else if (actionName==='sensor-tab'||actionName==='system-tab') {
    state.console=null;
    if (actionName==='sensor-tab') state.sensorTab=value;
    else state.systemTab=value;
    await loadRoute();
  } else if (actionName==='clear-sensor') clearSensor(target.dataset.kind,index);
  else if (actionName==='plug-preset') applyPreset(target.closest('form'));
  else if (actionName==='plug-test'||actionName==='rf-test') {
    testPlug(actionName==='rf-test'?+$('#rf-plug').value:state.plugs[index].index,value);
  } else if (actionName==='test-service'||actionName==='test-mqtt'||actionName==='discover') {
    let path='/mqtt/discover';
    if (actionName==='test-service') path='/brewservices/test';
    else if (actionName==='test-mqtt') path='/mqtt/test';
    const reply=await post(path,actionName==='test-service'?{index}:{});
    toast(reply.msg);
  } else if (actionName==='open-rf') { state.systemTab='rf'; location.hash='system'; await loadRoute(); }
  else if (actionName==='export') await exportConfig();
  else if (actionName==='import') $('#import-file').click();
  else if (actionName==='file-view') await viewFile(index);
  else if (actionName==='console-start') { state.console=value; lastConsolePoll=0; }
  else if (actionName==='console-stop') state.console=null;
  else if (actionName==='rf-clear') $('#rf-output').textContent='';
  else if (actionName==='ble-clear') $('#ble-output').textContent='';
  else if (actionName==='ble-send') await sendBle(value);
  else if (actionName==='update-check') {
    Object.assign(state.controller,await post('/update/check',{}, {timeout:30000}));
    $('#update-result').innerHTML=updateResult(state.controller);
  } else if (actionName==='close-modal') $('#modal').close();
  else if (actionName==='debug') await showDebug(index);
}
