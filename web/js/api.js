'use strict';
// The request queue, loading endpoint data and background polling.

// One in-flight request per tab. Explicit actions precede queued background reads.
const requests = [];
let requestRunning = false;

// Queue a controller request, prioritizing explicit actions over background reads.
function request(path, options={}, background=false) {
  return new Promise((resolve,reject) => {
    requests.push({path,options,background,resolve,reject});
    requests.sort((a,b) => Number(a.background)-Number(b.background));
    pumpRequests();
  });
}

// Run one queued request with a timeout and report uncertain POST outcomes safely.
async function pumpRequests() {
  if (requestRunning || !requests.length) return;
  requestRunning=true;
  const job=requests.shift();
  const abort=new AbortController();
  const timeout=setTimeout(() => abort.abort(), job.options.timeout || 8000);
  try {
    const response=await fetch(job.path, {...job.options, signal:abort.signal, cache:'no-store'});
    const text=await response.text();
    let data=text;
    if (!job.options.text) {
      try { data=JSON.parse(text); }
      catch { throw new Error('The controller returned an unreadable response.'); }
    }
    if (!response.ok || (data && data.status==='error')) {
      const error=new Error(data.msg || 'Request failed (HTTP '+response.status+').');
      error.server=true;
      error.httpStatus=response.status;
      throw error;
    }
    job.resolve({data, headers:response.headers});
  } catch (cause) {
    if (!cause.server && (cause.name==='AbortError' || cause instanceof TypeError)) {
      state.offline=true;
      const error=new Error(job.options.method==='POST'
        ? 'Result unknown: the request may have reached the controller. Read its state before saving again.'
        : 'Controller unreachable. Your last readings and drafts are retained.');
      error.unknown=job.options.method==='POST';
      job.reject(error);
    } else {
      // A truncated or invalid POST reply cannot tell us whether it took effect.
      if (job.options.method==='POST' && !cause.server) {
        cause.unknown=true;
        cause.message='Result unknown: '+cause.message+' Read current state before saving again.';
      }
      job.reject(cause);
    }
  } finally {
    clearTimeout(timeout);
    requestRunning=false;
    pumpRequests();
  }
}

// Read endpoint data through the shared serialized request queue.
const get = async (path,background=false) => (await request(path,{},background)).data;
// Send a JSON action through the queue without automatically retrying it.
const post = async (path,data,options={}) => (await request(path,{method:'POST',
  headers:{'Content-Type':'application/json'},body:JSON.stringify(data),...options})).data;

// Read an endpoint and update its browser state using the controller's units.
async function fetchData(name, background=false) {
  // These legacy replies contain temperatures but no unit field. Read the
  // existing controller endpoint first rather than extending their contracts.
  if (['probes','tilts','ispindels','profiles'].includes(name)) await fetchData('controller',background);
  let value;
  try { value=await get('/'+name,background); }
  catch (error) {
    if (name==='pressure') state.pressureReadFailed=true;
    throw error;
  }
  if (name==='controller') setController(value);
  else if (name==='fermenters') setFermenters(value);
  else if (name==='profiles') setProfiles(value);
  else if (['probes','tilts','ispindels'].includes(name)) setSensors(name,value);
  else if (name==='smartplugs') setPlugs(value);
  else if (name==='brewservices') state.services=value.services;
  else if (name==='fs/files') state.files=value.files;
  else if (name==='debug') {
    if (value.TempUnit && value.TempUnit!==state.unit) {
      setController({...state.controller,Unit:unitCode(value.TempUnit)});
    }
    state.debug=value;
    state.global.debug=!!value.DebugMode;
  }
  else state[name]=value;
  if (name==='pressure') {
    state.pressureReadAt=Date.now();
    state.pressureReadFailed=false;
  }
  loaded.add(name);
  return value;
}

// List the endpoint data needed by the current view or selected tab.
function routeData() {
  if (state.route==='dashboard' || state.route==='profiles') return ['fermenters','profiles'];
  if (state.route==='sensors') return [state.sensorTab];
  if (state.route==='plugs') return ['smartplugs'];
  if (state.route==='reporting') return ['brewservices','mqtt','notify'];
  if (state.route==='system') {
    if (state.systemTab==='settings') return ['controller','debug','syslog'];
    if (state.systemTab==='files') return ['fs/files'];
    if (state.systemTab==='rf') return ['smartplugs'];
    return ['controller','health'];
  }
  return [];
}

// Load missing or refreshed view data, ignoring results after navigation changes.
async function loadRoute(force=false) {
  const generation=++routeGeneration;
  let received=false;
  try {
    for (const name of routeData()) if (force || name==='pressure' || !loaded.has(name)) {
      await fetchData(name);
      received=true;
    }
    if (generation!==routeGeneration) return;
    if (received) { state.offline=false; state.lastUpdated=Date.now(); }
    render();
    updateLiveReadings();
    return true;
  } catch (error) {
    if (generation!==routeGeneration) return;
    toast(error.message);
    if (state.ready) updateAge();
    return false;
  }
}

// Poll the visible view without replacing editing forms or overlapping refreshes.
async function refreshVisible() {
  if (pollBusy || saving || state.refreshPaused || document.hidden || !state.ready) return;
  pollBusy=true;
  try {
    let names=[];
    if (['dashboard','profiles'].includes(state.route)) names=['fermenters'];
    else if (state.route==='sensors') names=[state.sensorTab];
    else if (state.route==='plugs') names=['smartplugs'];
    else if (state.route==='system') names=['controller'];
    for (const name of names) await fetchData(name,true);
    if (names.length) { state.offline=false; state.lastUpdated=Date.now(); }
    // Never replace an editing form during a poll. Live text has separate nodes.
    if (state.route==='dashboard' && !$('#sidebar').classList.contains('open') && !$('#modal').open) render();
    else updateLiveReadings();
  } catch (error) { updateAge(); }
  finally { pollBusy=false; lastPoll=Date.now(); }
}
