'use strict';
// The profiles view, the profile editor and its drafts.

// The firmware's step types, in step-type number order. The rules come from Profile.cpp:
// usesStart: the step ramps or bands from Start. Without it, the step holds End and ignores Start.
// usesDays:  the step has a time condition, in Days.
// target:    the reading that can complete the step: 'gravity', 'attenuation' or null.
// line:      how the preview draws it: 'solid' timed, 'dashed' condition-dependent, or a free-rise 'band'.
const stepTypes=[
  {name:'Temperature over time',usesStart:true,usesDays:true,target:null,line:'dashed',
    help:'Ramp from Start to End over Days. Advance after Days and when temperature reaches End '+
      'within the fermenter control band.'},
  {name:'Time at temperature',usesStart:true,usesDays:true,target:null,line:'solid',
    help:'Ramp from Start to End over Days. Advance after Days; reaching End is not a completion condition.'},
  {name:'Free rise',usesStart:true,usesDays:true,target:null,line:'band',
    help:'Use Start and End as the lower and upper limits. The controller widens a narrow band '+
      'to twice the fermenter hysteresis. Advance after Days.'},
  {name:'Specific gravity',usesStart:false,usesDays:true,target:'gravity',line:'dashed',
    help:'Hold End. Advance after Minimum days when valid gravity is at or below the target.'},
  {name:'Attenuation',usesStart:false,usesDays:true,target:'attenuation',line:'dashed',
    help:'Hold End. Advance after Minimum days when attenuation is at or above the target percentage.'},
  {name:'Temperature reached',usesStart:false,usesDays:false,target:null,line:'dashed',
    help:'Hold End. Advance when temperature reaches End within the fermenter control band, without a time condition.'},
  {name:'Gravity reached',usesStart:false,usesDays:false,target:'gravity',line:'dashed',
    help:'Hold End. Advance when valid gravity is at or below the target, without a time condition.'},
  {name:'Attenuation reached',usesStart:false,usesDays:false,target:'attenuation',line:'dashed',
    help:'Hold End. Advance when attenuation is at or above the target percentage, without a time condition.'},
  {name:'Time and gravity',usesStart:true,usesDays:true,target:'gravity',line:'dashed',
    help:'Ramp from Start to End over Days. Advance after Days when valid gravity is at or below the target.'},
  {name:'Time and attenuation',usesStart:true,usesDays:true,target:'attenuation',line:'dashed',
    help:'Ramp from Start to End over Days. Advance after Days when attenuation is at or above the target percentage.'}
];

// Build the profile selector and selected draft editor, locking profiles that are in use.
function profilesView() {
  const saved=state.profiles[state.selectedProfile];
  const profile=profileDrafts.get(state.selectedProfile)||saved;
  const locked=!!saved.used;
  const stepCount=profile.steps.length;
  return pageTitle('MAKE A PLAN FOR YOUR BREW','Fermentation profiles','Build a clear path from pitch to pour.')+`
    <div class="profile-layout">
      <div class="profile-list">${state.profiles.map((p,i)=>`
        <button class="profile-choice ${i===state.selectedProfile?'active':''}" data-action="select-profile" data-id="${i}">
          <span class="slot-label">PROFILE ${i+1}</span><strong>${esc(p.name)}</strong>
          <small>${(profileDrafts.get(i)||p).steps.length} steps${profileDrafts.has(i)?' · unsaved':''}</small>
          ${p.used?badge('In use','quiet'):badge('Available','good')}
        </button>`).join('')}</div>
      <div class="card panel">
        ${locked?`<div class="notice info">${icon('info')}<div><strong>Running on ${esc(saved.used)}.</strong>
          Copy it to an available slot to make changes.</div></div>`:''}
        <div class="panel-header">
          <div><div class="slot-label">PROFILE ${state.selectedProfile+1}</div><h2>${esc(profile.name)}</h2></div>
          ${btn('Copy to slot…','copy-profile','secondary')}</div>
        <form data-form="profile-${state.selectedProfile}">
          ${field('Profile name','name',profile.name,'text',`maxlength="31" ${locked?'disabled':''}`)}
          <div class="profile-preview"><div class="plot-heading">TEMPERATURE PLAN · °${state.unit} · step order</div>
            ${profilePlot(profile)}
            <p class="review-caption">Solid: timed step. Dashed: condition-dependent step. Shaded: free-rise band.
              This shows step order, not predicted completion times.</p></div>
          <div class="section-heading"><h3>Profile steps</h3><span class="subtle">${stepCount} of ${MAX_PROFILE_STEPS} slots</span></div>
          <div class="step-list">${profile.steps.map((step,i)=>profileStep(step,i,stepCount,locked)).join('')}</div>
          <div style="margin-top:13px">${btn('+ Add step','step-add','ghost',locked||stepCount>=MAX_PROFILE_STEPS?'disabled':'')}</div>
          ${locked?'<p class="review-caption">Copy this profile to an available slot to edit it.</p>':
            formActions('Save profile')}
        </form>
      </div>
    </div>`;
}

// Build the fields, controls and help text relevant to one profile step type.
function profileStep(step,i,count,locked) {
  const type=stepTypes[step.type];
  const disabled=locked?'disabled':'';
  const startDisabled=locked||!type.usesStart?'disabled':'';
  const targetLabel=type.target==='attenuation'?'Attenuation %':'Gravity target';
  return `<div class="step-row"><span class="step-index">${String(i+1).padStart(2,'0')}</span>
    ${select('Step type','type-'+i,step.type,stepTypes.map(({name},value)=>[value,name]),disabled)}
    ${field('Start °'+state.unit,'start-'+i,formatTemp(step.start),'number',`step="0.1" required ${startDisabled}`)}
    ${field('End °'+state.unit,'end-'+i,formatTemp(step.end),'number',`step="0.1" required ${disabled}`)}
    ${type.target?
      field(targetLabel,'trigger-'+i,step.trigger,'number',`step="any" min="0" required ${disabled}`):''}
    ${!type.target&&type.usesDays?
      field('Days','days-'+i,step.days,'number',`step="any" min="0" required ${disabled}`):''}
    <div class="step-buttons">
      <button type="button" class="icon-button" data-action="step-up" data-id="${i}"
        aria-label="Move step ${i+1} up" ${locked||i===0?'disabled':''}>↑</button>
      <button type="button" class="icon-button" data-action="step-down" data-id="${i}"
        aria-label="Move step ${i+1} down" ${locked||i===count-1?'disabled':''}>↓</button>
      <button type="button" class="icon-button" data-action="step-delete" data-id="${i}"
        aria-label="Delete step ${i+1}" ${disabled}>×</button>
    </div>
    ${type.target&&type.usesDays?
      field('Minimum days','days-'+i,step.days,'number',`min="0" step="any" required ${disabled}`):''}
    <details class="step-help"><summary>Step behaviour</summary><p>${esc(type.help)}</p></details>
  </div>`;
}

// Draw the profile's temperature sequence, including held steps and free-rise bands.
function profilePlot(profile) {
  if (!profile.steps.length) return '<p class="empty-state">Add a step to preview the profile.</p>';
  const segments=profile.steps.map(step=>({
    line:stepTypes[step.type].line,end:step.end,
    start:stepTypes[step.type].usesStart?step.start:step.end
  }));
  const values=segments.flatMap(segment=>[segment.start,segment.end]);
  const low=Math.min(...values)-3,high=Math.max(...values)+3;
  // Map a Celsius temperature into the chart's vertical coordinate.
  const y=value=>95-(value-low)/(high-low)*65;
  const width=420/profile.steps.length;
  return `<svg class="profile-plot" viewBox="0 0 490 120" role="img"
      aria-label="Temperature sequence for ${esc(profile.name)}">
    ${[0,1,2].map(i=>{
      const v=low+(high-low)*i/2;
      return `<line x1="40" x2="467" y1="${y(v)}" y2="${y(v)}" stroke="#2b3749" stroke-dasharray="3 5"/>
        <text x="4" y="${y(v)+3}" fill="#7e8da2" font-size="9">${formatTemp(v)}°</text>`;
    }).join('')}
    ${segments.map((segment,i)=>`
      ${segment.line==='band'?`<rect x="${40+i*width}" y="${y(Math.max(segment.start,segment.end))}"
        width="${width-4}" height="${Math.max(1,Math.abs(y(segment.start)-y(segment.end)))}"
        fill="#ed7182" fill-opacity="0.25" stroke="#ed7182"/>`:
        `<line x1="${40+i*width}" x2="${40+(i+1)*width-4}" y1="${y(segment.start)}" y2="${y(segment.end)}"
          stroke="#ed7182" stroke-width="2.5" ${segment.line==='solid'?'':'stroke-dasharray="5 4"'}/>
        <circle cx="${40+i*width}" cy="${y(segment.start)}" r="3" fill="#ed7182"/>`}
      <text x="${40+i*width+width/2}" y="116" fill="#8d9cb1" font-size="9"
        text-anchor="middle">Step ${i+1}</text>`).join('')}
  </svg>`;
}

// Serialize the editable parts of a profile for change comparisons.
function profileValues(p) { return JSON.stringify({name:p.name,steps:p.steps}); }

// Read the profile draft while preserving untouched temperature precision.
function captureProfile() {
  const index=state.selectedProfile;
  const form=$(`form[data-form="profile-${index}"]`);
  const profile=clone(profileFormValues.get(form)||profileDrafts.get(index)||state.profiles[index]);
  if (!form) return profile;
  const data=formData(form),baseline=formBaselines.get(form)||{};
  profile.name=data.name;
  profile.steps.forEach((step,n)=>{
    step.type=+data['type-'+n];
    if (data['start-'+n]!==baseline['start-'+n]) step.start=tempToC(+data['start-'+n]);
    if (data['end-'+n]!==baseline['end-'+n]) step.end=tempToC(+data['end-'+n]);
    if (data['days-'+n]!==undefined) step.days=+data['days-'+n];
    if (data['trigger-'+n]!==undefined) step.trigger=+data['trigger-'+n];
  });
  return profile;
}

// Keep a profile draft only while it differs from the saved profile.
function profileChanged() {
  const profile=captureProfile();
  if (profileValues(profile)===profileValues(state.profiles[state.selectedProfile])) profileDrafts.delete(state.selectedProfile);
  else profileDrafts.set(state.selectedProfile,profile);
}

// Update the visible profile form's unsaved-change state after rendering.
function markProfileDraft() {
  const form=$('form[data-form^="profile-"]');
  if (form) updateDirty(form);
}

// Build the profile JSON the controller expects: temperatures in its unit, and every step slot filled.
function profilePayload(index, profile) {
  const steps=profile.steps.map(step=>({stepType:step.type,startTemp:tempToUnit(step.start),
    endTemp:tempToUnit(step.end),sgTrigger:step.trigger,days:step.days}));
  while (steps.length<MAX_PROFILE_STEPS) steps.push({stepType:0,startTemp:0,endTemp:0,sgTrigger:0,days:0});
  return {index,name:profile.name,steps};
}

// Open the destination selector for copying a profile into another draft slot.
function chooseProfileCopy() {
  const choices=[[-1,'Choose an editable slot'],
    ...state.profiles.map((profile,index)=>[index,profile.name+(profile.used?' (in use)':'')])];
  showModal('<h2>Copy profile</h2>'+select('Destination slot','copy-slot',-1,choices)+
    `<div class="modal-actions">${btn('Cancel','close-modal','ghost')}${btn('Copy as draft','copy-confirm','')}</div>`);
}

// Copy the selected profile draft into a different editable slot without saving it.
function copyProfile() {
  const destination=+$('[name="copy-slot"]').value;
  if (destination<0 || destination===state.selectedProfile || state.profiles[destination].used) {
    toast('Choose a different editable slot.'); return;
  }
  if (profileDrafts.has(destination) || formDrafts.has('profile-'+destination)) {
    toast('Save or discard the destination draft first.'); return;
  }
  const profile=clone(profileDrafts.get(state.selectedProfile)||state.profiles[state.selectedProfile]);
  profile.used='';
  profileDrafts.set(destination,profile);
  state.selectedProfile=destination;
  $('#modal').close();
  render();
}

// Add, remove or reorder a step in an unlocked profile draft.
function editProfileStep(actionName,index) {
  if (state.profiles[state.selectedProfile].used) return;
  const profile=captureProfile(),steps=profile.steps;
  if (actionName==='step-add' && steps.length<MAX_PROFILE_STEPS) steps.push({type:1,start:19,end:19,days:1,trigger:0});
  if (actionName==='step-delete') steps.splice(index,1);
  if (actionName==='step-up'&&index>0) [steps[index-1],steps[index]]=[steps[index],steps[index-1]];
  if (actionName==='step-down'&&index<steps.length-1) [steps[index],steps[index+1]]=[steps[index+1],steps[index]];
  profileDrafts.set(state.selectedProfile,profile);
  formDrafts.delete('profile-'+state.selectedProfile);
  render();
}
