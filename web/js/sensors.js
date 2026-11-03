'use strict';

const fnOptions = [
  [99,'Unassigned'],[2,'Beer temperature'],[3,'Ambient temperature'],[4,'Tilt probe'],
  [5,'iSpindel probe'],[6,'Air probe'],[7,'Control temperature']
];

// Build a labelled sensor value that polling can update independently of its form.
function sensorReading(label, key, value) {
  return `<div><span data-live="${key}">${esc(value)}</span><small>${esc(label)}</small></div>`;
}

// Build the selected sensor tab with current readings, assignments and calibration forms.
function sensorsView() {
  const kind=state.sensorTab;
  const slots=state[kind];
  const tabs=[['probes','Temperature probes'],['tilts','Tilts'],['ispindels','iSpindels']];
  const controls=`<div class="tabs" role="tablist" aria-label="Sensor type">${tabs.map(([key,label])=>`
    <button role="tab" aria-selected="${kind===key}" class="${kind===key?'active':''}"
      data-action="sensor-tab" data-value="${key}">${label}</button>`).join('')}</div>`;
  const cards=slots.map((s,i)=>{
    const name=s.name || (kind==='probes'?'Probe '+s.index:'Empty slot '+i);
    const key=kind+'.'+i;
    const formKind=kind==='probes'?'probe':kind==='tilts'?'tilt':'ispindel';
    const detail=kind==='probes'?s.address:kind==='tilts'?(s.isPro?'Tilt Pro':'Tilt'):'ID '+orDash(s.id);
    const status=kind==='tilts'?badge(s.active?'Reading received':'Not detected',s.active?'good':'quiet'):
      kind==='ispindels'?badge(s.fresh?'Fresh':'No recent report',s.fresh?'good':'warn'):'';
    const sg=s.sg>0?(+s.sg).toFixed(4):'—';
    const fnChoices=kind==='probes'?fnOptions:[[2,'Beer temperature'],[99,'Not used']];
    const unitNote='The device reports its own gravity unit; it can reset this selection on its next report.';
    return `<article class="card panel">
      <div class="panel-header">
        <div><div class="slot-label">${esc(kind)} · ${i}</div><h2>${esc(name)}</h2><p>${esc(detail)}</p></div>
        <span data-sensor-status="${key}">${status}</span>
      </div>
      <div class="sensor-reading">
        ${sensorReading('Temperature',key+'.temp',formatTemp(s.temp)+' °'+state.unit)}
        ${kind==='probes'?'':sensorReading('Specific gravity',key+'.sg',sg)}
        ${kind==='ispindels'?sensorReading('Battery (V)',key+'.battery',toFinite(s.battery).toFixed(2)):''}
      </div>
      ${kind==='ispindels'?`<p class="review-caption" data-live="${key}.detail">${esc(spindelDetail(s))}</p>`:''}
      <form data-form="${formKind}-${i}">
        <div class="fields">
          ${select(kind==='probes'?'Function':'Temperature reading','fn',s.fn,fnChoices)}
          ${select('Fermenter','fermenter',s.fermenter,fermOptions())}
          ${kind==='ispindels'?select('Device gravity unit','unit',s.unit,[[0,'Specific gravity'],[1,'Plato']]):''}
          ${field('Temperature adjustment (°'+state.unit+')','adjust',deltaToUnit(s.adjust),'number','step="0.1" required')}
          ${kind==='probes'?'':field('Gravity adjustment','sgAdjust',s.sgAdjust,'number','step="0.0001" required')}
        </div>
        ${kind==='ispindels'&&s.gravityUnit?`<p class="review-caption">${unitNote}</p>`:''}
        ${formActions('Save changes',kind==='probes'?'':btn('Clear slot','clear-sensor','ghost',`data-kind="${kind}" data-id="${i}"`))}
      </form>
    </article>`;
  }).join('');
  return pageTitle('GOOD DATA, BETTER CONTROL','Sensors','Assign each sensor and fine-tune its readings.')+controls+
    (kind==='tilts'?'<p class="notice info">Eight colours can be detected; up to four Tilts can be configured.</p>':'')+
    (slots.length?`<div class="grid">${cards}</div>`:
      '<p class="empty-state">No detected or configured sensors returned by the controller.</p>');
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
