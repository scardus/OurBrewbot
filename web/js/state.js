'use strict';
// Device data and unsaved edits live only in this browser tab.
const state = {
  route:'dashboard', unit:'C', paused:false, scenario:'normal', ready:false,
  sensorTab:'probes', systemTab:'overview', profile:0, editorId:null,
  logPaused:false, lastUpdated:0, controller:{}, health:{}, debug:{Overrides:[]},
  fermenters:[], profiles:[], probes:[], tilts:[], ispindels:[], plugs:[],
  services:[], mqtt:{}, global:{}, syslog:{}, files:[]
};
const drafts = new Map();
const formBaselines = new WeakMap();
const profileFormValues = new WeakMap();
const profileDrafts = new Map();
const unknownForms = new Set();
let toastTimer;
let logLines = [];
const loaded = new Set();
let routeGeneration = 0;
let pollBusy = false;
let lastPoll = 0;
let lastLogPoll = 0;
let logBoot = '';
let logAfter = 0;
let logMore = false;
let saving = false;

const finite = (value, fallback=0) => Number.isFinite(Number(value)) ? Number(value) : fallback;
const sensorTemp = value => value == null || +value===-127 || toC(finite(value,-127))<=-100 ? null : toC(+value);
const shown = value => value == null ? '—' : String(value);

function setController(data) {
  const unit = data.Unit === 2 ? 'F' : 'C';
  if (state.ready && unit !== state.unit && (drafts.size || profileDrafts.size)) {
    state.unitChanged = true;
    toast('The controller temperature unit changed. Discard and review drafts before saving.');
  }
  state.controller = data;
  state.controllerReadAt=Date.now();
  state.unit = unit;
  state.global = {unit, resolution:data.Resolution, alarm:data.AlarmDwellSec,
    mdns:data.MdnsEnabled, updates:data.UpdateCheck, crashes:data.CrashReports,
    debug:!!state.debug.DebugMode};
}

function setFermenters(data) {
  const unit=data[0]?.TempUnit;
  if (unit && unit!==state.unit) setController({...state.controller,Unit:unit==='F'?2:1});
  state.fermenters = data.map(f => ({raw:f, id:+f.Fermenter, name:f.FermenterName,
    beer:f.BeerName, yeast:f.YeastName, status:f.StatusStr, alarmActive:!!f.Alarm,
    power:!!f.Power, tempControl:!!f.TempControl, beerTemp:sensorTemp(f.BeerTemp),
    ambient:sensorTemp(f.AmbientTemp), sg:finite(f.SG), og:finite(f.OG,1.05),
    tg:finite(f.TG,1.01), floor:toC(+f.FloorTemp), ceiling:toC(+f.CeilingTemp),
    hysteresis:deltaC(+f.Hysteresis), alarm:deltaC(+f.AlarmTolerance),
    delay:+f.CompressorDelay, profile:+f.ProfileNo, running:!!f.ProfileRunning,
    step:+f.CurrentStep, hour:+f.CurrentHour, paused:!!f.ProfilePaused,
    liveTest:!!f.LiveTest, brewfather:!!(f.BrewServices&2), brewers:!!(f.BrewServices&1),
    mqtt:!!(f.BrewServices&8), abv:finite(f.EstABV), attenuation:finite(f.Attenuation),
    source:f.BeerTempSource, gravity:f.GravitySource}));
  updateProfileLocks();
}

function setProfiles(data) {
  state.profiles = data.profiles.map(p => ({name:p.name, used:'',
    // The runner stops at the first empty slot, even if later slots contain data.
    steps:p.steps.slice(0,profileStepCount(p.steps))
      .map(s => ({type:+s.stepType, start:toC(+s.startTemp), end:toC(+s.endTemp),
        trigger:+s.sgTrigger, days:+s.days}))}));
  updateProfileLocks();
}

function profileStepCount(steps) {
  const end=steps.findIndex(s => !s.stepType && !s.days && !s.startTemp && !s.endTemp);
  return end<0 ? steps.length : end;
}

function updateProfileLocks() {
  state.profiles.forEach((p,i) => {
    p.used = state.fermenters.filter(f => (f.running || f.paused) && f.profile===i+1).map(f => f.name).join(', ');
  });
}

function setSensors(kind, data) {
  if (kind==='probes') state.probes = data.probes.map(p => ({...p, fn:+p.function,
    temp:sensorTemp(p.temperature), adjust:deltaC(+p.tempAdjust)}));
  if (kind==='tilts') {
    const names=['Red','Green','Black','Purple','Orange','Blue','Yellow','Pink'];
    state.tilts = names.map((name,i) => {
      const p=data.tilts.find(t => +t.colour===i) || {};
      return {...p, index:i, name, fn:p.function===2?2:99, fermenter:p.fermenter??99,
        temp:sensorTemp(p.temperature), sg:finite(p.sg), adjust:deltaC(finite(p.tempAdjust)),
        sgAdjust:finite(p.sgAdjust), active:!!p.active};
    });
  }
  if (kind==='ispindels') state.ispindels = data.ispindels.map(p => ({...p,
    fn:p.function===2?2:99, temp:sensorTemp(p.temperature), adjust:deltaC(+p.tempAdjust)}));
}

function setPlugs(data) {
  state.plugs = data.plugs.map(p => ({...p,
    role:p.function<8 ? (p.function%2?'cold':'hot') : p.function===8?'aux1':p.function===9?'aux2':'none',
    fermenter:p.function<8 ? Math.floor(p.function/2) : p.fermenter}));
}
