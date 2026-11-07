'use strict';
// The sensors view for probes, Tilts and iSpindels.

const fnOptions = [
  [UNASSIGNED,'Unassigned'],[FUNCTION_BEER_TEMP,'Beer temperature'],[3,'Ambient temperature'],[4,'Tilt probe'],
  [5,'iSpindel probe'],[6,'Air probe'],[7,'Control temperature']
];

const trackerFunctions = [[FUNCTION_BEER_TEMP,'Beer temperature'],[UNASSIGNED,'Not used']];

// What differs between the three sensor types. The keys match state.sensorTab.
const sensorTypes = {
  probes: {
    tabLabel:'Temperature probes', formKind:'probe', functionLabel:'Function', functions:fnOptions,
    hasGravity:false, clearable:false, onePerFermenter:false,
    emptyName:s=>'Probe '+s.index,
    detail:s=>s.address,
    status:()=>''
  },
  tilts: {
    tabLabel:'Tilts', formKind:'tilt', functionLabel:'Temperature reading', functions:trackerFunctions,
    hasGravity:true, clearable:true, onePerFermenter:true,
    emptyName:(s,i)=>'Empty slot '+i,
    detail:s=>s.isPro?'Tilt Pro':'Tilt',
    status:s=>badge(s.active?'Reading received':'Not detected',s.active?'good':'quiet')
  },
  ispindels: {
    tabLabel:'iSpindels', formKind:'ispindel', functionLabel:'Temperature reading', functions:trackerFunctions,
    hasGravity:true, clearable:true, onePerFermenter:true,
    emptyName:(s,i)=>'Empty slot '+i,
    detail:s=>'ID '+orDash(s.id),
    status:s=>badge(s.fresh?'Fresh':'No recent report',s.fresh?'good':'warn')
  }
};

// Build a labelled sensor value that polling can update independently of its form.
function sensorReading(label, key, value) {
  return `<div><span data-live="${key}">${esc(value)}</span><small>${esc(label)}</small></div>`;
}

// The Sensors page tabs: one per sensor type, then the local pressure sensor.
const sensorTabs = () => [...Object.entries(sensorTypes).map(([key,type])=>[key,type.tabLabel]),['pressure','Pressure']];

// Build the selected sensor tab with current readings, assignments and calibration forms.
function sensorsView() {
  const kind=state.sensorTab;
  if (kind==='pressure') return pressureView();
  const slots=state[kind];
  return pageTitle('GOOD DATA, BETTER CONTROL','Sensors','Assign each sensor and fine-tune its readings.')+
    tabBar('Sensor type','sensor-tab',sensorTabs(),kind)+
    (kind==='tilts'?'<p class="notice info">Eight colours can be detected; up to four Tilts can be configured.</p>':'')+
    sharedFermenterNotices(kind,slots)+
    (slots.length?`<div class="grid">${slots.map((sensor,i)=>sensorCard(kind,sensor,i)).join('')}</div>`:
      '<p class="empty-state">No detected or configured sensors returned by the controller.</p>');
}

// Warn about a fermenter that already has several Tilts or iSpindels. The firmware refuses new ones,
// but an older version or an imported file can have saved them, and only one of them is used.
function sharedFermenterNotices(kind,slots) {
  const type=sensorTypes[kind];
  if (!type.onePerFermenter) return '';
  return state.fermenters.map(f=>{
    const sharing=slots.filter(s=>+s.fermenter===f.id);
    if (sharing.length<2) return '';
    const names=sharing.map(s=>s.name || type.emptyName(s,s.index)).join(', ');
    return `<p class="notice error">${esc(f.name)} has ${sharing.length} ${type.tabLabel}: ${esc(names)}. Only one is used, so unassign the others.</p>`;
  }).join('');
}

// Build one sensor's readings and its settings form. Only iSpindels have a battery and a gravity unit.
function sensorCard(kind,s,i) {
  const type=sensorTypes[kind];
  const isSpindel=kind==='ispindels';
  const key=kind+'.'+i;
  const clearButton=type.clearable?btn('Clear slot','clear-sensor','ghost',`data-kind="${kind}" data-id="${i}"`):'';
  const unitNote='The device reports its own gravity unit; it can reset this selection on its next report.';
  return `<article class="card panel">
    <div class="panel-header">
      <div>
        <div class="slot-label">${esc(kind)} · ${i}</div>
        <h2>${esc(s.name || type.emptyName(s,i))}</h2>
        <p>${esc(type.detail(s))}</p>
      </div>
      <span data-sensor-status="${key}">${type.status(s)}</span>
    </div>
    <div class="sensor-reading">
      ${sensorReading('Temperature',key+'.temp',formatTemp(s.temp)+' °'+state.unit)}
      ${type.hasGravity?sensorReading('Specific gravity',key+'.sg',formatGravity(s.sg)):''}
      ${isSpindel?sensorReading('Battery (V)',key+'.battery',toFinite(s.battery).toFixed(2)):''}
    </div>
    ${isSpindel?`<p class="review-caption" data-live="${key}.detail">${esc(spindelDetail(s))}</p>`:''}
    <form data-form="${type.formKind}-${i}">
      <div class="fields">
        ${select(type.functionLabel,'fn',s.fn,type.functions)}
        ${select('Fermenter','fermenter',s.fermenter,fermOptions())}
        ${isSpindel?select('Device gravity unit','unit',s.unit,[[0,'Specific gravity'],[1,'Plato']]):''}
        ${field('Temperature adjustment (°'+state.unit+')','adjust',deltaToUnit(s.adjust),'number','step="0.1" required')}
        ${type.hasGravity?field('Gravity adjustment','sgAdjust',s.sgAdjust,'number','step="0.0001" required'):''}
      </div>
      ${isSpindel&&s.gravityUnit?`<p class="review-caption">${unitNote}</p>`:''}
      ${formActions('Save changes',clearButton)}
    </form>
  </article>`;
}

// Summarize an iSpindel's extra readings and the age of its last report.
function spindelDetail(s) {
  const age=s.minutesSince>=65535?'Never seen':s.minutesSince===0?'Less than a minute ago':s.minutesSince+' min ago';
  return 'Angle '+orDash(s.angle)+'° · RSSI '+orDash(s.rssi)+' dBm · Velocity '+orDash(s.velocity)+
    ' · Corrected gravity '+orDash(s.corrGravity)+' · Runtime '+orDash(s.runTime)+' s'+
    ' · Unit '+orDash(s.gravityUnit)+' · '+age;
}

// Build a probe, Tilt or iSpindel save without rounding unchanged calibration values.
function sensorPayload(form,data,kind,index) {
  const sensors=kind==='probe'?state.probes:kind==='tilt'?state.tilts:state.ispindels;
  const sensor=sensors[index];
  const body={function:+data.fn,fermenter:+data.fermenter,
    tempAdjust:savedNumber(form,data,'adjust',toFinite(sensor.tempAdjust))};
  if (kind==='tilt') body.colour=index;
  else body.index=sensor.index;
  if (kind!=='probe') body.sgAdjust=savedNumber(form,data,'sgAdjust',sensor.sgAdjust);
  if (kind==='ispindel') body.unit=+data.unit;
  return body;
}

// Confirm clearing a Tilt or iSpindel slot, then reload its saved state.
function clearSensor(kind,index) {
  const tilt=kind==='tilts',sensor=tilt?state.tilts[index]:state.ispindels[index];
  confirmAction('Clear sensor slot?','The saved assignment and calibration will be cleared.','Clear',async()=>{
    await post(tilt?'/tilt':'/ispindel/config',tilt?{colour:index,_clear:true}:{index:sensor.index,_clear:true});
    formDrafts.delete((tilt?'tilt-':'ispindel-')+index);
    await fetchData(tilt?'tilts':'ispindels');
    render();
  });
}

// Pressure has one local input and a capture workflow rather than sensor slots.
// Each reading state has a name and a badge style, in the order of
// PressureReadingState in the firmware's PressureCalc.h, whose number the API
// sends as the state.
const pressureStates = [
  {name:'Unavailable', badge:'quiet'},
  {name:'Below range', badge:'warn'},
  {name:'Calibrated', badge:'good'},
  {name:'Estimate', badge:''},
  {name:'ADC limit', badge:'warn'}
];
const describePressureState = status => pressureStates[status]||pressureStates[0];
const formatPressure = psi => psi==null?'—':Number(psi).toFixed(1);

// The sensor card's status badge: its reading state, or that no fermenter has it.
function pressureBadge(p) {
  if (p.fermenter==null || +p.fermenter===UNASSIGNED) return badge('Not assigned','quiet');
  const described=describePressureState(p.state);
  return badge(described.name,described.badge);
}

// The calibration card's status badge: whether a capture is running or failed.
function pressureCaptureBadge(p) {
  if (p.failed) return badge('Capture failed','warn');
  if (p.active) return badge('Capturing '+p.samples+' / '+p.samplesNeeded);
  return badge('Ready','quiet');
}

// The calibration points as text, such as "0 PSI = 160, 15 PSI = 250".
const pressurePoints = points => (points||[]).map(x=>x.psi+' PSI = '+x.raw).join(', ')||'None';

// Laid out like the other sensor tabs: the sensor's card, then its calibration card.
function pressureView() {
  const p=state.pressure;
  return pageTitle('GOOD DATA, BETTER CONTROL','Sensors','Assign each sensor and fine-tune its readings.')+
    tabBar('Sensor type','sensor-tab',sensorTabs(),'pressure')+
    `<p class="notice info">Calibrate with a reference gauge: capture 0 PSI first, then up to four higher pressures,
      holding each steady for about 45 seconds. Readings above the highest point are estimates.
      This is a monitor only; it doesn't control a pressure relief valve.</p>
    <div class="grid">
    <article class="card panel">
      <div class="panel-header">
        <div>
          <div class="slot-label">pressure · 0</div>
          <h2>Local pressure sensor</h2>
          <p>Pink socket · A0</p>
        </div>
        <span data-pressure-status>${pressureBadge(p)}</span>
      </div>
      <div class="sensor-reading">
        <div><span data-pressure-psi>${formatPressure(p.psi)} PSI</span><small>Pressure</small></div>
        <div><span data-pressure-raw>${orDash(p.raw)}</span><small>ADC reading</small></div>
      </div>
      <form data-form="pressure">
        <div class="fields">
          ${select('Fermenter','fermenter',p.fermenter??UNASSIGNED,fermOptions())}
        </div>
        ${formActions('Save changes')}
      </form>
    </article>
    <article class="card panel">
      <div class="panel-header">
        <div>
          <div class="slot-label">pressure · calibration</div>
          <h2>Calibration</h2>
          <p data-pressure-saved>Saved: ${esc(pressurePoints(p.points))}</p>
        </div>
        <span data-pressure-capture>${pressureCaptureBadge(p)}</span>
      </div>
      <div class="sensor-reading">
        <div><span data-pressure-count>${(p.draft||[]).length}</span><small>New points</small></div>
      </div>
      <p class="review-caption" data-pressure-draft>New: ${esc(pressurePoints(p.draft))}</p>
      <form data-form="pressurepoint">
        <div class="fields">
          ${field('Reference pressure (PSI)','psi','','number','min="0" max="100" step="0.1" required')}
        </div>
        ${formActions('Capture point')}
      </form>
      <div class="two-col-actions" style="margin-top:12px">
        ${btn('Apply new points','pressure-apply','small secondary')}
        ${btn('Discard new points','pressure-cancel','small ghost')}
      </div>
    </article>
  </div>`;
}

// Polling updates these readings without replacing the reference-pressure form.
function updatePressureReadings() {
  if (state.route!=='sensors' || state.sensorTab!=='pressure') return;
  const p=state.pressure;
  const reading=$('[data-pressure-psi]');
  if (!reading) return;
  reading.textContent=formatPressure(p.psi)+' PSI';
  $('[data-pressure-raw]').textContent=orDash(p.raw);
  $('[data-pressure-status]').innerHTML=pressureBadge(p);
  $('[data-pressure-capture]').innerHTML=pressureCaptureBadge(p);
  $('[data-pressure-saved]').textContent='Saved: '+pressurePoints(p.points);
  $('[data-pressure-count]').textContent=(p.draft||[]).length;
  $('[data-pressure-draft]').textContent='New: '+pressurePoints(p.draft);
}

// Actions are never retried automatically; always reread progress afterwards.
async function pressureCommand(action) {
  if (saving) return;
  saving=true;
  try { await postAndShowReply('/pressure',{action}); }
  finally {
    try { await fetchData('pressure'); updatePressureReadings(); }
    finally { saving=false; }
  }
}
