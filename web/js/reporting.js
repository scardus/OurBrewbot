'use strict';

// Build service-reporting and MQTT configuration forms with their explicit test actions.
function reporting() {
  const m=state.mqtt;
  return title('KEEP YOUR BREW CONNECTED','Reporting',
    'Choose where your readings go. Enable each service per fermenter.')+
    `<div class="grid">${state.services.map(serviceCard).join('')}</div>
    <article class="card panel" style="margin-top:18px">
      <div class="panel-header"><div class="service-heading"><div class="service-logo">${icon('reporting')}</div>
        <div><h2>MQTT & Home Assistant</h2><p>Your controller, connected to your home.</p></div></div>
        ${badge(m.enabled?'Enabled':'Disabled',m.enabled?'good':'quiet')}</div>
      <form data-form="mqtt">
        ${toggle('Enable MQTT','enabled',m.enabled)}
        <div class="fields" style="margin-top:20px">
          ${field('Broker host','host',m.host,'text','maxlength="63"')}
          ${field('Port','port',m.port,'number','min="1" max="65535" required')}
          ${field('Username','username',m.username,'text','maxlength="31"')}
          ${field('New password','password','','password','autocomplete="new-password" maxlength="47"','Leave blank to keep the saved password.')}
          ${field('Base topic','baseTopic',m.baseTopic,'text','maxlength="31" required')}
        </div>
        ${toggle('Home Assistant discovery','haDiscovery',m.haDiscovery,'Advertise the controller and its sensors automatically.')}
        ${toggle('Allow MQTT control','allowControl',m.allowControl,'Allow commands from your broker to change controller settings.')}
        ${toggle('Publish log messages','logEnabled',m.logEnabled)}
        ${toggle('Clear saved password','clearPassword',false,'Only tick this to explicitly remove the saved password.')}
        ${actions('Save MQTT settings',btn('Test connection','test-mqtt','secondary')+btn('Publish discovery','discover','ghost'))}
      </form>
    </article>`;
}

// Build one reporting service's saved settings and connection-test controls.
function serviceCard(s,i) {
  return `<article class="card panel">
    <div class="panel-header"><div class="service-heading"><div class="service-logo">${i?'Bf':'B'}</div>
      <div><h2>${esc(s.name)}</h2><p>Batch tracking & fermentation readings</p></div></div>
      ${badge(s.enabled?'Enabled':'Disabled',s.enabled?'good':'quiet')}</div>
    <form data-form="service-${i}">
      ${toggle('Enable reporting','enabled',s.enabled)}
      <div class="fields" style="margin-top:18px">
        ${field('Service ID / stream key','serviceId',s.serviceId,'password','maxlength="47"')}
        ${field('Device name','deviceName',s.deviceName,'text','maxlength="31"')}
      </div>
      ${actions('Save service',btn('Test connection','test-service','secondary',`data-id="${i}"`))}
    </form>
  </article>`;
}
