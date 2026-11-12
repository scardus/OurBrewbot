'use strict';
// The reporting view for Brewer's Friend, Brewfather, MQTT and push notifications.

// Build service-reporting, MQTT and notification forms with their explicit test actions.
function reportingView() {
  const m=state.mqtt;
  const mqttButtons=btn('Test connection','test-mqtt','secondary')+btn('Publish discovery','discover','ghost');
  return pageTitle('KEEP YOUR BREW CONNECTED','Reporting',
    'Choose where your readings and alerts go. Enable each brew service per fermenter.')+
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
          ${field('New password','password','','password','autocomplete="new-password" maxlength="47"',
            'Leave blank to keep the saved password.')}
          ${field('Base topic','baseTopic',m.baseTopic,'text','maxlength="31" required')}
        </div>
        ${toggle('Home Assistant discovery','haDiscovery',m.haDiscovery,
          'Advertise the controller and its sensors automatically.')}
        ${toggle('Allow MQTT control','allowControl',m.allowControl,
          'Allow commands from your broker to change controller settings.')}
        ${toggle('Publish log messages','logEnabled',m.logEnabled)}
        ${toggle('Clear saved password','clearPassword',false,'Only tick this to explicitly remove the saved password.')}
        ${formActions('Save MQTT settings',mqttButtons)}
      </form>
    </article>
    ${notifyCard(state.notify)}`;
}

// Build the push-notification settings and their test action.
function notifyCard(n) {
  return `<article class="card panel" style="margin-top:18px">
      <div class="panel-header"><div class="service-heading"><div class="service-logo">${icon('info')}</div>
        <div><h2>Notifications</h2><p>Alerts sent straight to your phone.</p></div></div>
        ${badge(n.enabled?'Enabled':'Disabled',n.enabled?'good':'quiet')}</div>
      <form data-form="notify">
        ${toggle('Enable notifications','enabled',n.enabled)}
        <div class="fields" style="margin-top:20px;grid-template-columns:1fr">
          ${field('Notification URL','url',n.url,'text','maxlength="127" placeholder="http://ntfy.sh/my-topic"',
            'An ntfy topic or other address that accepts a POST. Must start with http:// - HTTPS is not supported.')}
        </div>
        <h3 style="margin-top:20px">Send a notification for</h3>
        ${toggle('Temperature alarms','alarms',n.alarms,'When an alarm starts and when it ends.')}
        ${toggle('Sensor problems','sensors',n.sensors,'A probe, Tilt or iSpindel stops or resumes reporting.')}
        ${toggle('Unexpected restarts','restarts',n.restarts,'After a crash or power cut.')}
        ${toggle('Profile progress','profile',n.profile,'A step completes or a profile finishes.')}
        ${toggle('Firmware updates','updates',n.updates,'Once when a new release is available.')}
        ${formActions('Save notification settings',btn('Send test notification','test-notify','secondary'))}
      </form>
    </article>`;
}

// Validate the notification URL the way TextSafe.cpp::isValidNotifyUrl() does.
function notifyPayload(data) {
  if (data.url && !/^http:\/\/[\x21-\x7e]+$/.test(data.url)) {
    throw new Error('Notification URL must start with http:// and have no spaces. HTTPS is not supported.');
  }
  if (data.enabled && !data.url) throw new Error('Enter a notification URL before enabling notifications.');
  return {enabled:data.enabled,url:data.url,alarms:data.alarms,sensors:data.sensors,
    restarts:data.restarts,profile:data.profile,updates:data.updates};
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
      ${formActions('Save service',btn('Test connection','test-service','secondary',`data-id="${i}"`))}
    </form>
  </article>`;
}

// Validate the MQTT topic and preserve the saved password unless explicitly changed.
function mqttPayload(data) {
  // TextSafe.cpp::isValidBaseTopic() rejects wildcards, controls and a leading $.
  if (!data.baseTopic || /^[\$]|[+#\x00-\x1f\x7f]/.test(data.baseTopic)) {
    throw new Error('Base topic must be 1–31 bytes, without wildcards or control characters, '+
      'and must not start with $.');
  }
  const body={enabled:data.enabled,host:data.host,port:+data.port,username:data.username,
    baseTopic:data.baseTopic,haDiscovery:data.haDiscovery,allowControl:data.allowControl,logEnabled:data.logEnabled};
  if (data.clearPassword) body.password='';
  else if (data.password) body.password=data.password;
  return body;
}
