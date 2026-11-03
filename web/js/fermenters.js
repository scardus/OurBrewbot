'use strict';
// The dashboard, fermenter cards, settings drawer and debug overrides.

// Build the overview totals and fermenter cards from the latest browser state.
function dashboardView() {
  const active=state.fermenters.filter(f => f.power).length;
  const alarms=state.fermenters.filter(f => f.alarmActive).length;
  const running=state.fermenters.filter(profileActive).length;
  const controlling=state.fermenters.some(f => f.power&&f.tempControl);
  const c=state.controller;
  const uptime=toFinite(c.Uptime)+Math.floor((Date.now()-(state.controllerReadAt||Date.now()))/60000);
  const uptimeText=Math.floor(uptime/1440)+'d '+Math.floor(uptime%1440/60)+'h '+uptime%60+'m';
  const overview=[
    ['Active fermenters',active+' / '+FERMENTER_COUNT,(FERMENTER_COUNT-active)+' powered off'],
    ['Temperature control',alarms?'Check alarms':controlling?'Enabled':'Off',alarms+' active alarms'],
    ['Running profiles',running,'Includes paused profiles'],
    ['Controller uptime',uptimeText,'WiFi signal '+orDash(c.RSSI)+' dBm']
  ].map(([label,value,caption])=>`
    <div class="overview-item">
      <div class="overview-top">${esc(label)}</div>
      <div class="overview-value" style="font-size:21px">${esc(value)}</div>
      <div class="overview-caption">${esc(caption)}</div>
    </div>`).join('');
  const buttons=`
    <span class="refresh-label"><i class="pulse-dot"></i><span data-age></span></span>
    ${btn(icon(state.refreshPaused?'play':'pause'),'pause','ghost','aria-label="Pause or resume refresh"')}
    ${btn(icon('refresh')+'Refresh','refresh','secondary')}`;
  return pageTitle('FERMENTATION, AT A GLANCE','Dashboard','Readings and control for your fermenters.',buttons)+`
    <div class="overview">${overview}</div>
    <div class="section-heading">
      <h2>Fermenters <small>Live overview</small></h2>
      <span class="subtle">Temperatures in °${state.unit}</span>
    </div>
    <div class="grid">${state.fermenters.map(fermCard).join('')}</div>
    <div class="dashboard-note">${icon('shield')}Readings refresh separately from your edits.
      Settings are saved only when you choose Save.</div>`;
}

// Build one dashboard card with live readings, control state and profile progress.
function fermCard(f) {
  const cls=!f.power?'off':f.status==='Heating'?'heating':f.status==='Cooling'?'cooling':'holding';
  const profile=state.profiles[f.profile-1];
  const running=profileActive(f);
  const status=f.alarmActive?'Alarm · '+f.status:!f.power?'Powered off':f.status;
  const kind=f.alarmActive?'warn':!f.power?'quiet':f.status==='Heating'?'warm':f.status==='Cooling'?'':'good';
  return `<article class="card ferm-card ${cls}">
    <div class="card-inner">
      <div class="card-header">
        <div><div class="slot-label">${esc(f.name)}</div><h2 class="batch-name">${esc(f.beer)}</h2>
          <p class="batch-sub">${esc(f.yeast)}</p></div>${badge(status,kind)}
      </div>
      <div class="readings">
        <div><div class="reading-label">Beer temperature</div>
          <div class="reading-number primary">${formatTemp(f.beerTemp)}<small>°${state.unit}</small></div>
          <div class="reading-source">${esc(f.source)}</div></div>
        <div><div class="reading-label">Gravity</div>
          <div class="reading-number">${formatGravity(f.sg)}</div>
          <div class="reading-source">${esc(f.gravity)}</div></div>
        <div><div class="reading-label">Ambient</div>
          <div class="reading-number">${formatTemp(f.ambient)}<small>°${state.unit}</small></div>
          <div class="reading-source">${f.ambient==null?'No reading':'Ambient temperature'}</div></div>
      </div>
      <div class="band"><span>Target band</span><strong>${formatTemp(f.floor)} – ${formatTemp(f.ceiling)} °${state.unit}</strong>
        <span>Temp control ${f.tempControl?'on':'off'}</span></div>
      ${profile?`<div class="profile-line"><strong>${esc(profile.name)}</strong>
        <span>${running?(f.paused?'Paused · ':'')+'Step '+(f.step+1)+' of '+profile.steps.length:
          'Assigned · not running'}</span></div>
        <div class="segments">${profile.steps.map((s,i)=>
          `<span class="${running?(i<f.step?'done':i===f.step?'current':''):''}"></span>`).join('')}</div>
        <div class="profile-caption">
          <span>${running?esc(stepTypes[profile.steps[f.step]?.type]?.name||'Profile step'):
            'Start from fermenter settings'}</span>
          <span>${running?'Hour '+f.hour+' in this step':''}</span>
        </div>`:
        '<div class="off-placeholder">Standard temperature control · no profile assigned</div>'}
    </div>
    <div class="card-footer"><div class="tiny-metrics"><span>Est. ABV <b>${f.abv.toFixed(1)}%</b></span>
      <span>Att. <b>${f.attenuation.toFixed(1)}%</b></span></div>
      ${btn('Manage '+icon('arrow'),'edit-ferm','secondary',`data-id="${f.id}"`)}</div>
  </article>`;
}

// Open a fermenter's settings drawer and restore any unsaved browser draft.
function openFerm(id) {
  const f=state.fermenters[id],profile=state.profiles[f.profile-1],running=profileActive(f);
  const profileChoices=[[0,'Standard temperature control'],...state.profiles.map((p,i)=>[i+1,p.name])];
  const gravityLimits='min="0.990" max="1.200" step="0.0001" required';
  const mainLabel=!running?'Start':f.paused?'Resume':'Pause';
  const mainCommand=!running?'start':f.paused?'resume':'pause';
  const controlButton=(label,command,kind)=>btn(label,'profile-control',kind,`data-id="${id}" data-value="${command}"`);
  $('#editor-content').innerHTML=`
    <div class="editor-head"><div><div class="slot-label">${esc(f.name)}</div><h2>${esc(f.beer)}</h2></div>
      <button type="button" class="icon-button" data-action="close-editor" aria-label="Close fermenter settings">
        ${icon('close')}
      </button></div>
    <form data-form="ferm-${id}" id="ferm-form" style="display:contents">
      <div class="editor-scroll">
        <div class="editor-summary">
          <div><small>Beer temperature</small><strong>${formatTemp(f.beerTemp)} °${state.unit}</strong></div>
          ${badge(f.alarmActive?'Alarm · '+f.status:f.status,f.alarmActive?'warn':'good')}</div>
        <section class="editor-section"><h3>Batch details</h3><div class="fields">
          ${field('Fermenter name','name',f.name,'text','maxlength="31"')}
          ${field('Beer name','beer',f.beer,'text','maxlength="31"')}
          ${field('Yeast','yeast',f.yeast,'text','maxlength="31"')}
          ${select('Fermentation profile','profile',f.profile,profileChoices)}
          ${field('Original gravity','og',f.og.toFixed(4),'number',gravityLimits)}
          ${field('Target gravity','tg',f.tg.toFixed(4),'number',gravityLimits)}
        </div></section>
        <section class="editor-section"><h3>Temperature control</h3><div class="fields">
          ${field('Floor (°'+state.unit+')','floor',formatTemp(f.floor),'number','step="0.1" required')}
          ${field('Ceiling (°'+state.unit+')','ceiling',formatTemp(f.ceiling),'number','step="0.1" required')}
          ${field('Hysteresis (°'+state.unit+')','hysteresis',deltaToUnit(f.hysteresis),'number',
            'min="0" step="0.01" required','The target band must be at least twice this value.')}
          ${field('Alarm tolerance (°'+state.unit+')','alarm',deltaToUnit(f.alarm),'number','min="0" step="0.1" required')}
          ${field('Compressor delay (minutes)','delay',f.delay,'number','min="0" max="1440" step="1" required')}
        </div>
          ${toggle('Power','power',f.power,'Turning this off stops heating and cooling.')}
          ${toggle('Temperature control','tempControl',f.tempControl)}
          ${toggle('Profile test mode','liveTest',f.liveTest,'Fast-forwards a profile for testing.')}
        </section>
        <section class="editor-section"><h3>Send readings to</h3>
          ${toggle('Brewfather','brewfather',f.brewfather)}
          ${toggle('Brewer’s Friend','brewers',f.brewers)}
          ${toggle('MQTT / Home Assistant','mqtt',f.mqtt)}
        </section>
        ${profile?`<section class="editor-section"><h3>Profile controls · ${esc(profile.name)}</h3>
          <p class="subtitle">${running?'Step '+(f.step+1)+' · hour '+f.hour:'Assigned, not running'}</p>
          <div class="two-col-actions">
            ${controlButton(mainLabel,mainCommand,'secondary')}
            ${running?controlButton('Previous step','prev','ghost')+
              controlButton('Next step','next','ghost')+
              controlButton('Restart','start','ghost')+
              controlButton('Stop profile','stop','danger'):''}
          </div></section>`:''}
        ${state.global.debug?`<section class="editor-section"><h3>Debug sensor overrides</h3>
          <p class="subtitle">Runtime only; resets on reboot.</p>
          ${btn('Open debug overrides','debug','secondary',`data-id="${id}"`)}</section>`:''}
      </div>
      <div class="editor-actions">
        <div class="form-error" role="alert"></div>
        <span class="saved-label">No unsaved changes</span>
        <button type="button" class="button ghost" data-action="discard-form">Discard</button>
        <button type="submit" class="button" data-save>Save changes</button></div>
    </form>`;
  mountForms($('#editor'));
  if (!$('#editor').open) $('#editor').showModal();
}

// Validate fermenter settings and build their existing JSON save contract.
function fermenterPayload(form,data,index) {
  const fermenter=state.fermenters[index],raw=fermenter.raw;
  const body={Fermenter:fermenter.id,FermenterName:data.name,BeerName:data.beer,YeastName:data.yeast,
    OG:savedNumber(form,data,'og',raw.OG),TG:savedNumber(form,data,'tg',raw.TG),
    CeilingTemp:savedNumber(form,data,'ceiling',raw.CeilingTemp),
    FloorTemp:savedNumber(form,data,'floor',raw.FloorTemp),
    Hysteresis:savedNumber(form,data,'hysteresis',raw.Hysteresis),
    AlarmTolerance:savedNumber(form,data,'alarm',raw.AlarmTolerance),
    CompressorDelay:+data.delay,Power:data.power,TempControl:data.tempControl,
    ProfileNo:+data.profile,LiveTest:data.liveTest};
  // Config.h defines HTTP reporting at bits 0/1 and MQTT at bit 3.
  // Preserve any bits outside the three checkboxes instead of erasing them.
  const reportingBits=REPORT_BREWERS|REPORT_BREWFATHER|REPORT_MQTT;
  body.BrewServices=(raw.BrewServices&~reportingBits)|
    (data.brewers?REPORT_BREWERS:0)|(data.brewfather?REPORT_BREWFATHER:0)|(data.mqtt?REPORT_MQTT:0);
  // Match Config.h::checkTempBand() and the alarm bounds in WebAPI.cpp.
  const ceiling=tempToC(body.CeilingTemp),floor=tempToC(body.FloorTemp),hysteresis=deltaToC(body.Hysteresis);
  if (deltaToC(body.AlarmTolerance)<0 || deltaToC(body.AlarmTolerance)>10) {
    throw new Error('Alarm tolerance must be 0–10 °C (0–18 °F).');
  }
  if (floor < -20 || ceiling>50 || floor>=ceiling || hysteresis<0 || hysteresis>10 || ceiling-floor+1e-6<2*hysteresis) {
    throw new Error('Temperature band must be within −20…50 °C, floor below ceiling, '+
      'and at least twice hysteresis (0…10 °C).');
  }
  return body;
}

// Confirm and apply a profile command using the fermenter's saved assignment.
function controlProfile(index,command) {
  const fermenter=state.fermenters[index];
  const description=command+' for '+fermenter.name+' using the saved profile assignment.';
  confirmAction('Change profile state?',description,'Apply',async()=>{
    const body={Fermenter:fermenter.id,action:command};
    if (command==='start') body.ProfileIndex=fermenter.profile;
    await post('/fermenter/profile',body);
    await fetchData('fermenters');
    render();
    openFerm(index);
  });
}

// Read current runtime overrides and open their fermenter-specific editor.
async function showDebug(id) {
  await fetchData('debug');
  const f=state.fermenters[id],override=state.debug.Overrides.find(v=>+v.Fermenter===f.id)||{};
  const unit=' (°'+state.unit+')';
  showModal(`<h2>Debug overrides · ${esc(f.name)}</h2><p>Runtime only; resets on reboot.</p>
    <form data-debug="${id}" data-unit="${state.unit}" novalidate>
      ${toggle('Enable overrides','enabled',override.Enabled)}
      <div class="fields">
        ${field('Beer temperature'+unit,'beer',override.BeerTemp??formatTemp(f.beerTemp),'number','step="0.1" required')}
        ${field('Ambient temperature'+unit,'ambient',override.AmbientTemp??formatTemp(f.ambient),'number',
          'step="0.1" required')}
        ${field('Specific gravity','sg',override.SG??f.sg,'number','step="0.0001" required')}
      </div>
      <button class="button" type="submit">Apply overrides</button>
    </form>`);
}

// Validate and apply runtime sensor overrides without retrying an uncertain result.
async function saveDebug(form) {
  const button=$('button[type="submit"]',form);
  if (button.disabled || form.dataset.unknown || !fieldsAreValid(form)) return;
  button.disabled=true;
  try {
    await fetchData('controller');
    if (form.dataset.unit!==state.unit) {
      throw new Error('Controller units changed. Close and reopen debug overrides before applying.');
    }
    const id=+form.dataset.debug,d=formData(form);
    if (![d.beer,d.ambient,d.sg].every(v=>v!=='' && Number.isFinite(+v))) {
      throw new Error('Enter finite numbers for debug readings.');
    }
    await post('/debug',{Fermenter:state.fermenters[id].id,Enabled:d.enabled,
      BeerTemp:+d.beer,AmbientTemp:+d.ambient,SG:+d.sg});
    $('#modal').close();
    await fetchData('debug');
  } catch (error) {
    if (error.unknown) form.dataset.unknown='true';
    toast(error.message+(error.unknown?' Close and reopen overrides to read their state.':''));
  } finally { button.disabled=!!form.dataset.unknown; }
}
