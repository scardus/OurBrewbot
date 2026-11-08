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

// The sensor card's status badge: its reading state, independently of assignment.
function pressureBadge(p) {
  if (!p.enabled) return badge('Disabled','quiet');
  const described=describePressureState(p.state);
  return badge(described.name,described.badge);
}

// The calibration card's status badge: whether a capture is running or failed.
function pressureCaptureBadge(p) {
  if (p.failed) return badge('Capture failed','warn');
  if (p.active) return badge('Capturing '+p.samples+' / '+p.samplesNeeded);
  const count=(p.draft||[]).length;
  return badge(count?count+' new points':(p.points||[]).length?'Current calibration':'Not calibrated','quiet');
}

// Keep the outcome visible after a capture ends, including after returning to this tab.
function pressureCaptureMessage(p) {
  const count=(p.draft||[]).length;
  if (!p.enabled) return 'Enable the pressure sensor to calibrate it and check its reading.';
  if (p.failed) {
    const reason=p.failure===1?'The sensor input reached its limit. Check the wiring and pressure.':
      p.failure===2?'The sensor reading did not increase. Check the reference pressure and try again.':'The point could not be captured. Try again.';
    return 'Capture at '+p.capturePsi+' PSI failed. '+reason+' Completed new points are kept.';
  }
  if (p.active) return 'Capturing point '+(count+1)+' at '+p.capturePsi+' PSI. Hold the pressure steady.';
  // The lists above already show which points are new, so only say what to do next.
  if (count) {
    const next=count===1?'Set a higher pressure and enter the reference gauge reading.':
      count<5?'Apply the new points, or capture another higher pressure.':'Five points is the most; apply the new points when ready.';
    return p.draft[count-1].psi+' PSI point captured. '+next;
  }
  return (p.points||[]).length?'Current calibration is in use. To recalibrate, capture atmospheric zero first.':'Capture atmospheric zero first.';
}

// Estimate time only while controller updates are fresh. A clock reaching zero is not success.
function pressureCaptureTime(p,now=Date.now()) {
  if (!p.active) return '';
  if (state.refreshPaused) return 'Refresh paused. Capture continues on the controller; resume to see progress.';
  if (!state.pressureReadAt || state.pressureReadFailed || now-state.pressureReadAt>10000)
    return 'Progress not confirmed. Waiting for the controller.';
  const seconds=Math.ceil(Math.max(0,p.remainingMs-(now-state.pressureReadAt))/1000);
  return seconds>0?'About '+seconds+' seconds remaining':'Waiting for the controller to confirm completion.';
}

// Called by the existing UI timer; these nodes are separate from the editing form.
function updatePressureProgress() {
  if (state.route!=='sensors' || state.sensorTab!=='pressure') return;
  const message=$('[data-pressure-message]');
  if (!message) return;
  const p=state.pressure;
  const text=pressureCaptureMessage(p);
  if (message.textContent!==text) message.textContent=text;
  // A failed capture is shown as an error notice, like a failed command.
  message.className=p.failed?'notice error':'review-caption';
  $('[data-pressure-timing]').hidden=!p.active;
  $('[data-pressure-time]').textContent=pressureCaptureTime(p);
  const progress=$('[data-pressure-progress]');
  progress.hidden=!p.active;
  progress.max=p.samplesNeeded||10;
  progress.value=p.active?Math.min(p.samples||0,progress.max-1):0;
  $('[data-pressure-samples]').textContent=p.active?p.samples+' of '+p.samplesNeeded+' samples collected':'';
}

// Use the existing label/value rows so both sets remain readable on a narrow screen.
function pressurePoints(points,empty,capture=null) {
  const rows=(points||[]).map(point=>`<div class="list-row">
    <span>${esc(point.psi)} PSI<small>Reference pressure</small></span>
    <strong>${esc(point.raw)}<small>Sensor reading (ADC)</small></strong>
  </div>`).join('');
  const pending=capture?.active?`<div class="list-row">
    <span>${esc(capture.capturePsi)} PSI<small>Reference pressure</small></span>
    ${badge('Capturing - incomplete')}
  </div>`:'';
  return rows+pending || `<p class="review-caption">${esc(empty)}</p>`;
}

// The note about replacing the current set only applies once new points exist.
const pressureHasNewPoints = p => !!p.active || (p.draft||[]).length>0;

// Laid out like the other sensor tabs: the sensor's card, then its calibration card.
function pressureView() {
  const p=state.pressure;
  return pageTitle('GOOD DATA, BETTER CONTROL','Sensors','Assign each sensor and fine-tune its readings.')+
    tabBar('Sensor type','sensor-tab',sensorTabs(),'pressure')+
    `<p class="notice info">Calibrate with a reference gauge: capture 0 PSI first, then up to four higher pressures,
      holding each steady for about 45 seconds. Apply the points and check the reading before assigning a fermenter. Readings above the highest point are estimates.
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
        ${toggle('Enable pressure sensor','enabled',!!p.enabled,'Disabling it discards new calibration points but keeps the current calibration and assignment.')}
        <div class="fields" style="margin-top:18px">
          ${select('Fermenter','fermenter',p.fermenter??UNASSIGNED,fermOptions())}
        </div>
        <p class="review-caption" data-pressure-assignment>Apply a calibration before assigning a fermenter.</p>
        ${formActions('Save changes')}
      </form>
    </article>
    <article class="card panel">
      <div class="panel-header">
        <div>
          <div class="slot-label">pressure · calibration</div>
          <h2>Calibration</h2>
          <p>Capture two to five reference points.</p>
        </div>
        <span data-pressure-capture>${pressureCaptureBadge(p)}</span>
      </div>
      <h3>Current calibration</h3>
      <div data-pressure-saved>${pressurePoints(p.points,'No calibration applied.')}</div>
      <h3 style="margin-top:24px">New calibration - not applied</h3>
      <div data-pressure-draft>${pressurePoints(p.draft,'No new points captured.',p)}</div>
      <p class="review-caption" data-pressure-replace ${pressureHasNewPoints(p)?'':'hidden'}>Applying new points replaces the whole current calibration. New points are lost if the controller restarts.</p>
      <p class="${p.failed?'notice error':'review-caption'}" style="margin-top:16px" data-pressure-message role="status">${esc(pressureCaptureMessage(p))}</p>
      <progress class="pressure-progress" data-pressure-progress aria-label="Calibration samples collected" hidden></progress>
      <p class="review-caption" data-pressure-timing hidden><span data-pressure-time></span><br><span data-pressure-samples></span></p>
      <form data-pressure-point novalidate>
        <div class="fields" style="margin-top:18px">
          ${field('Reference pressure (PSI)','psi',p.active||p.failed?p.capturePsi:(p.draft||[]).length?state.pressureReference:0,'number','min="0" max="100" step="0.1" required')}
        </div>
        <div class="form-actions">
          <button type="submit" class="button" data-pressure-start>Capture point</button>
          ${btn('Apply new points','pressure-apply')}
          ${btn('Discard new points','pressure-cancel','ghost')}
        </div>
      </form>
      <p data-pressure-result role="status" class="notice ${state.pressureResultError?'error':'info'}" style="margin-top:16px" ${state.pressureResult?'':'hidden'}>${esc(state.pressureResult)}</p>
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
  $('[data-pressure-saved]').innerHTML=pressurePoints(p.points,'No calibration applied.');
  $('[data-pressure-draft]').innerHTML=pressurePoints(p.draft,'No new points captured.',p);
  $('[data-pressure-replace]').hidden=!pressureHasNewPoints(p);
  updatePressureProgress();
  updatePressureControls();
}

// Actions depend on controller state, not the settings form's dirty flag.
function pressureActions(p) {
  const count=(p.draft||[]).length;
  return {
    capture:!!p.enabled && !p.active && count<5,
    apply:!!p.enabled && !p.active && count>=2,
    cancel:!!p.active || count>0 || !!p.failed,
    assign:(p.points||[]).length>=2 && !p.active && count===0
  };
}

// Update availability without redrawing the form or replacing a reference being typed.
function updatePressureControls() {
  if (state.route!=='sensors' || state.sensorTab!=='pressure') return;
  const form=$('form[data-pressure-point]');
  if (!form) return;
  const p=state.pressure,actions=pressureActions(p),count=(p.draft||[]).length;
  const input=form.elements.psi;
  if (form.dataset.points!==String(count)) {
    if (form.dataset.points!==undefined) input.value=count?state.pressureReference:'0';
    form.dataset.points=String(count);
  }
  if (!count) input.value='0';
  if (p.active) input.value=String(p.capturePsi);
  input.readOnly=count===0;
  input.disabled=saving || !p.enabled || !!p.active || count>=5;
  input.min=count?p.draft[count-1].psi:0;
  const psi=Number(input.value);
  const valid=input.value.trim()!=='' && Number.isFinite(psi) && psi<=100 && (count?psi>p.draft[count-1].psi:psi===0);
  const unconfirmed=!state.pressureReadAt || state.pressureReadFailed;
  const start=$('[data-pressure-start]'),apply=$('[data-action="pressure-apply"]');
  start.disabled=saving || unconfirmed || !actions.capture || !valid;
  start.textContent=p.active?'Capturing point…':p.failed?'Retry point':count?'Capture point':'Capture 0 PSI';
  apply.disabled=saving || unconfirmed || !actions.apply;
  // Highlight the next step: Apply once enough points exist, otherwise Capture.
  const applyNext=actions.apply && !p.failed;
  start.className=applyNext?'button secondary':'button';
  apply.className=applyNext?'button':'button secondary';
  const cancel=$('[data-action="pressure-cancel"]');
  cancel.disabled=saving || unconfirmed || !actions.cancel;
  cancel.textContent=p.active?'Cancel capture and discard new points':'Discard new points';
  const settings=$('form[data-form="pressure"]');
  settings.elements.fermenter.disabled=saving || !actions.assign;
  $('[data-save]',settings).disabled=saving || settings.dataset.dirty!=='true' ||
    (!!p.active && settings.elements.enabled.checked);
  $('[data-pressure-assignment]').textContent=actions.assign?'Assignment is optional. Unassigned sensors still show local readings.':
    (p.points||[]).length<2?'Apply a calibration before assigning a fermenter.':'Apply or discard new points before changing the assignment.';
}

// Command feedback remains visible through polling, including an unsuccessful flash save.
function showPressureResult(message,error=false) {
  state.pressureResult=message;
  state.pressureResultError=error;
  const result=$('[data-pressure-result]');
  if (!result) return;
  result.textContent=message;
  result.className='notice '+(error?'error':'info');
  result.hidden=!message;
}

async function capturePressurePoint(form) {
  if (!fieldsAreValid(form)) return;
  const p=state.pressure,count=(p.draft||[]).length,psi=Number(form.elements.psi.value);
  if (!Number.isFinite(psi) || psi>100 || (count?psi<=p.draft[count-1].psi:psi!==0)) {
    showPressureResult(count?'Enter a reference pressure higher than '+p.draft[count-1].psi+' PSI, up to 100 PSI.':'Capture 0 PSI first.',true);
    return;
  }
  await pressureCommand('capture',psi);
}

// Never retry a POST automatically. Re-read even after an uncertain reply or a failed flash save.
async function pressureCommand(action,psi) {
  if (saving) return;
  const actions=pressureActions(state.pressure);
  if (!state.pressureReadAt || state.pressureReadFailed) {
    showPressureResult('Refresh the controller state before trying again.',true); return;
  }
  if (!actions[action]) return;
  saving=true;
  showPressureResult('');
  updatePressureControls();
  try {
    const reply=await post('/pressure',action==='capture'?{action,psi}:{action});
    if (action==='capture') state.pressureReference='';
    else { state.pressureReference=''; showPressureResult(reply.msg); }
  } catch (error) {
    showPressureResult(error.message,true);
  } finally {
    try { await fetchData('pressure'); }
    catch (error) {
      showPressureResult((state.pressureResult?state.pressureResult+' ':'')+'Progress not confirmed. Refresh before trying again. '+error.message,true);
    }
    finally { saving=false; updatePressureReadings(); }
  }
}
