'use strict';

function fermCard(f) {
  const cls=!f.power?'off':f.status==='Heating'?'heating':f.status==='Cooling'?'cooling':'holding';
  const profile=state.profiles[f.profile-1];
  const running=f.running||f.paused;
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
          <div class="reading-number primary">${temp(f.beerTemp)}<small>°${state.unit}</small></div>
          <div class="reading-source">${esc(f.source)}</div></div>
        <div><div class="reading-label">Gravity</div>
          <div class="reading-number">${f.sg>0?f.sg.toFixed(4):'—'}</div>
          <div class="reading-source">${esc(f.gravity)}</div></div>
        <div><div class="reading-label">Ambient</div>
          <div class="reading-number">${temp(f.ambient)}<small>°${state.unit}</small></div>
          <div class="reading-source">${f.ambient==null?'No reading':'Ambient temperature'}</div></div>
      </div>
      <div class="band"><span>Target band</span><strong>${temp(f.floor)} – ${temp(f.ceiling)} °${state.unit}</strong>
        <span>Temp control ${f.tempControl?'on':'off'}</span></div>
      ${profile?`<div class="profile-line"><strong>${esc(profile.name)}</strong>
        <span>${running?(f.paused?'Paused · ':'')+'Step '+(f.step+1)+' of '+profile.steps.length:'Assigned · not running'}</span></div>
        <div class="segments">${profile.steps.map((s,i)=>`<span class="${running?(i<f.step?'done':i===f.step?'current':''):''}"></span>`).join('')}</div>
        <div class="profile-caption"><span>${running?esc(stepNames[profile.steps[f.step]?.type]||'Profile step'):'Start from fermenter settings'}</span>
          <span>${running?'Hour '+f.hour+' in this step':''}</span></div>`:
        '<div class="off-placeholder">Standard temperature control · no profile assigned</div>'}
    </div>
    <div class="card-footer"><div class="tiny-metrics"><span>Est. ABV <b>${f.abv.toFixed(1)}%</b></span>
      <span>Att. <b>${f.attenuation.toFixed(1)}%</b></span></div>
      ${btn('Manage '+icon('arrow'),'edit-ferm','secondary',`data-id="${f.id}"`)}</div>
  </article>`;
}

function openFerm(id) {
  state.editorId=id;
  const f=state.fermenters[id],profile=state.profiles[f.profile-1],running=f.running||f.paused;
  $('#editor-content').innerHTML=`
    <div class="editor-head"><div><div class="slot-label">${esc(f.name)}</div><h2>${esc(f.beer)}</h2></div>
      <button type="button" class="icon-button" data-action="close-editor" aria-label="Close fermenter settings">${icon('close')}</button></div>
    <form data-form="ferm-${id}" id="ferm-form" style="display:contents">
      <div class="editor-scroll">
        <div class="editor-summary"><div><small>Beer temperature</small><strong>${temp(f.beerTemp)} °${state.unit}</strong></div>
          ${badge(f.alarmActive?'Alarm · '+f.status:f.status,f.alarmActive?'warn':'good')}</div>
        <section class="editor-section"><h3>Batch details</h3><div class="fields">
          ${field('Fermenter name','name',f.name,'text','maxlength="31"')}
          ${field('Beer name','beer',f.beer,'text','maxlength="31"')}
          ${field('Yeast','yeast',f.yeast,'text','maxlength="31"')}
          ${select('Fermentation profile','profile',f.profile,[[0,'Standard temperature control'],...state.profiles.map((p,i)=>[i+1,p.name])])}
          ${field('Original gravity','og',f.og.toFixed(4),'number','min="0.990" max="1.200" step="0.0001" required')}
          ${field('Target gravity','tg',f.tg.toFixed(4),'number','min="0.990" max="1.200" step="0.0001" required')}
        </div></section>
        <section class="editor-section"><h3>Temperature control</h3><div class="fields">
          ${field('Floor (°'+state.unit+')','floor',temp(f.floor),'number','step="0.1" required')}
          ${field('Ceiling (°'+state.unit+')','ceiling',temp(f.ceiling),'number','step="0.1" required')}
          ${field('Hysteresis (°'+state.unit+')','hysteresis',delta(f.hysteresis),'number','min="0" step="0.01" required','The target band must be at least twice this value.')}
          ${field('Alarm tolerance (°'+state.unit+')','alarm',delta(f.alarm),'number','min="0" step="0.1" required')}
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
            ${btn(!running?'Start':f.paused?'Resume':'Pause','profile-control','secondary',`data-id="${id}" data-value="${!running?'start':f.paused?'resume':'pause'}"`)}
            ${running?btn('Previous step','profile-control','ghost',`data-id="${id}" data-value="prev"`)+
              btn('Next step','profile-control','ghost',`data-id="${id}" data-value="next"`)+
              btn('Restart','profile-control','ghost',`data-id="${id}" data-value="start"`)+
              btn('Stop profile','profile-control','danger',`data-id="${id}" data-value="stop"`):''}
          </div></section>`:''}
        ${state.global.debug?`<section class="editor-section"><h3>Debug sensor overrides</h3>
          <p class="subtitle">Runtime only; resets on reboot.</p>${btn('Open debug overrides','debug','secondary',`data-id="${id}"`)}</section>`:''}
      </div>
      <div class="editor-actions"><div class="form-error" role="alert"></div><span class="saved-label">No unsaved changes</span>
        <button type="button" class="button ghost" data-action="discard-form">Discard</button>
        <button type="submit" class="button" data-save>Save changes</button></div>
    </form>`;
  mountForms($('#editor'));
  if (!$('#editor').open) $('#editor').showModal();
}
