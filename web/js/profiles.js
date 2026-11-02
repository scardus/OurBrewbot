'use strict';

// Completion rules from Profile.cpp. These replace the old hover-only tooltips.
const stepHelp=[
  'Ramp from Start to End over Days. Advance after Days and when temperature reaches End within the fermenter control band.',
  'Ramp from Start to End over Days. Advance after Days; reaching End is not a completion condition.',
  'Use Start and End as the lower and upper limits. The controller widens a narrow band to twice the fermenter hysteresis. Advance after Days.',
  'Hold End. Advance after Minimum days when valid gravity is at or below the target.',
  'Hold End. Advance after Minimum days when attenuation is at or above the target percentage.',
  'Hold End. Advance when temperature reaches End within the fermenter control band, without a time condition.',
  'Hold End. Advance when valid gravity is at or below the target, without a time condition.',
  'Hold End. Advance when attenuation is at or above the target percentage, without a time condition.',
  'Ramp from Start to End over Days. Advance after Days when valid gravity is at or below the target.',
  'Ramp from Start to End over Days. Advance after Days when attenuation is at or above the target percentage.'
];

function profiles() {
  const saved=state.profiles[state.profile];
  const profile=profileDrafts.get(state.profile)||saved;
  const locked=!!saved.used;
  return title('MAKE A PLAN FOR YOUR BREW','Fermentation profiles','Build a clear path from pitch to pour.')+`
    <div class="profile-layout">
      <div class="profile-list">${state.profiles.map((p,i)=>`
        <button class="profile-choice ${i===state.profile?'active':''}" data-action="select-profile" data-id="${i}">
          <span class="slot-label">PROFILE ${i+1}</span><strong>${esc(p.name)}</strong>
          <small>${(profileDrafts.get(i)||p).steps.length} steps${profileDrafts.has(i)?' · unsaved':''}</small>
          ${p.used?badge('In use','quiet'):badge('Available','good')}
        </button>`).join('')}</div>
      <div class="card panel">
        ${locked?`<div class="notice info">${icon('info')}<div><strong>Running on ${esc(saved.used)}.</strong>
          Copy it to an available slot to make changes.</div></div>`:''}
        <div class="panel-header"><div><div class="slot-label">PROFILE ${state.profile+1}</div><h2>${esc(profile.name)}</h2></div>
          ${btn('Copy to slot…','copy-profile','secondary')}</div>
        <form data-form="profile-${state.profile}">
          ${field('Profile name','name',profile.name,'text',`maxlength="31" ${locked?'disabled':''}`)}
          <div class="profile-preview"><div class="plot-heading">TEMPERATURE PLAN · °${state.unit} · step order</div>
            ${profilePlot(profile)}
            <p class="review-caption">Solid: timed step. Dashed: condition-dependent step. Shaded: free-rise band.
              This shows step order, not predicted completion times.</p></div>
          <div class="section-heading"><h3>Profile steps</h3><span class="subtle">${profile.steps.length} of 15 slots</span></div>
          <div class="step-list">${profile.steps.map((step,i)=>profileStep(step,i,profile.steps.length,locked)).join('')}</div>
          <div style="margin-top:13px">${btn('+ Add step','step-add','ghost',locked||profile.steps.length>=15?'disabled':'')}</div>
          ${locked?'<p class="review-caption">Copy this profile to an available slot to edit it.</p>':actions('Save profile')}
        </form>
      </div>
    </div>`;
}

function profileStep(step,i,count,locked) {
  const fields=['sed','sed','sed','egd','egd','e','eg','eg','segd','segd'][step.type];
  const condition=fields.includes('g');
  const attenuation=[4,7,9].includes(step.type);
  const disabled=locked?'disabled':'';
  return `<div class="step-row"><span class="step-index">${String(i+1).padStart(2,'0')}</span>
    ${select('Step type','type-'+i,step.type,stepNames.map((name,value)=>[value,name]),disabled)}
    ${field('Start °'+state.unit,'start-'+i,temp(step.start),'number',`step="0.1" required ${locked||!fields.includes('s')?'disabled':''}`)}
    ${field('End °'+state.unit,'end-'+i,temp(step.end),'number',`step="0.1" required ${disabled}`)}
    ${fields.includes('d')||condition?field(condition?(attenuation?'Attenuation %':'Gravity target'):'Days',
      (condition?'trigger-':'days-')+i,condition?step.trigger:step.days,'number',`step="any" min="0" required ${disabled}`):''}
    <div class="step-buttons">
      <button type="button" class="icon-button" data-action="step-up" data-id="${i}" aria-label="Move step ${i+1} up" ${locked||i===0?'disabled':''}>↑</button>
      <button type="button" class="icon-button" data-action="step-down" data-id="${i}" aria-label="Move step ${i+1} down" ${locked||i===count-1?'disabled':''}>↓</button>
      <button type="button" class="icon-button" data-action="step-delete" data-id="${i}" aria-label="Delete step ${i+1}" ${disabled}>×</button>
    </div>
    ${condition&&fields.includes('d')?field('Minimum days','days-'+i,step.days,'number',`min="0" step="any" required ${disabled}`):''}
    <details class="step-help"><summary>Step behaviour</summary><p>${esc(stepHelp[step.type])}</p></details>
  </div>`;
}

function profilePlot(profile) {
  if (!profile.steps.length) return '<p class="empty-state">Add a step to preview the profile.</p>';
  // Profile.cpp::stepRamps() uses Start only for types 0, 1, 8 and 9.
  // Free Rise uses a band; every other type holds End, ignoring stale Start.
  const segments=profile.steps.map(step=>({
    type:step.type,end:step.end,
    start:[0,1,2,8,9].includes(step.type)?step.start:step.end
  }));
  const values=segments.flatMap(segment=>[segment.start,segment.end]);
  const low=Math.min(...values)-3,high=Math.max(...values)+3;
  const y=value=>95-(value-low)/(high-low)*65;
  const width=420/profile.steps.length;
  return `<svg class="profile-plot" viewBox="0 0 490 120" role="img" aria-label="Temperature sequence for ${esc(profile.name)}">
    ${[0,1,2].map(i=>{
      const v=low+(high-low)*i/2;
      return `<line x1="40" x2="467" y1="${y(v)}" y2="${y(v)}" stroke="#2b3749" stroke-dasharray="3 5"/>
        <text x="4" y="${y(v)+3}" fill="#7e8da2" font-size="9">${temp(v)}°</text>`;
    }).join('')}
    ${segments.map((segment,i)=>`
      ${segment.type===2?`<rect x="${40+i*width}" y="${y(Math.max(segment.start,segment.end))}"
        width="${width-4}" height="${Math.max(1,Math.abs(y(segment.start)-y(segment.end)))}"
        fill="#ed7182" fill-opacity="0.25" stroke="#ed7182"/>`:
        `<line x1="${40+i*width}" x2="${40+(i+1)*width-4}" y1="${y(segment.start)}" y2="${y(segment.end)}"
          stroke="#ed7182" stroke-width="2.5" ${[1].includes(segment.type)?'':'stroke-dasharray="5 4"'}/>
        <circle cx="${40+i*width}" cy="${y(segment.start)}" r="3" fill="#ed7182"/>`}
      <text x="${40+i*width+width/2}" y="116" fill="#8d9cb1" font-size="9" text-anchor="middle">Step ${i+1}</text>`).join('')}
  </svg>`;
}
