'use strict';

// Device data and unsaved edits live only in this browser tab.
const state = {
  route:'dashboard', unit:'C', refreshPaused:false, offline:false, ready:false,
  sensorTab:'probes', systemTab:'overview', selectedProfile:0,
  logPaused:false, lastUpdated:0, controller:{}, health:{}, debug:{Overrides:[]},
  fermenters:[], profiles:[], probes:[], tilts:[], ispindels:[], plugs:[],
  services:[], mqtt:{}, global:{}, syslog:{}, files:[]
};

const formDrafts = new Map();

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

// Convert a value to a finite number, using the fallback for invalid input.
const toFinite = (value, fallback=0) => Number.isFinite(Number(value)) ? Number(value) : fallback;

// Format a Celsius temperature in the selected display unit.
const formatTemp = value => value == null ? '—' : (state.unit === 'F' ? value*1.8+32 : value).toFixed(1);

// Convert a Celsius temperature difference into the display unit.
const deltaToUnit = value => state.unit === 'F' ? +(value*1.8).toFixed(2) : value;

// Convert an absolute temperature from the display unit to Celsius.
const tempToC = value => state.unit === 'F' ? (value-32)/1.8 : value;

// Convert a temperature difference from the display unit to Celsius.
const deltaToC = value => state.unit === 'F' ? value/1.8 : value;

// Normalize a sensor temperature to Celsius or mark it as unavailable.
const sensorTemp = value => value == null || +value===-127 || tempToC(toFinite(value,-127))<=-100 ? null : tempToC(+value);

// Format a received value for display, using a dash when it is missing.
const orDash = value => value == null ? '—' : String(value);

// Store controller settings and block existing drafts after a unit change.
function setController(data) {
  const unit = data.Unit === 2 ? 'F' : 'C';
  if (state.ready && unit !== state.unit && (formDrafts.size || profileDrafts.size)) {
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

// Normalize fermenter replies into browser state and refresh profile edit locks.
function setFermenters(data) {
  const unit=data[0]?.TempUnit;
  if (unit && unit!==state.unit) setController({...state.controller,Unit:unit==='F'?2:1});
  state.fermenters = data.map(f => ({raw:f, id:+f.Fermenter, name:f.FermenterName,
    beer:f.BeerName, yeast:f.YeastName, status:f.StatusStr, alarmActive:!!f.Alarm,
    power:!!f.Power, tempControl:!!f.TempControl, beerTemp:sensorTemp(f.BeerTemp),
    ambient:sensorTemp(f.AmbientTemp), sg:toFinite(f.SG), og:toFinite(f.OG,1.05),
    tg:toFinite(f.TG,1.01), floor:tempToC(+f.FloorTemp), ceiling:tempToC(+f.CeilingTemp),
    hysteresis:deltaToC(+f.Hysteresis), alarm:deltaToC(+f.AlarmTolerance),
    delay:+f.CompressorDelay, profile:+f.ProfileNo, running:!!f.ProfileRunning,
    step:+f.CurrentStep, hour:+f.CurrentHour, paused:!!f.ProfilePaused,
    liveTest:!!f.LiveTest, brewfather:!!(f.BrewServices&2), brewers:!!(f.BrewServices&1),
    mqtt:!!(f.BrewServices&8), abv:toFinite(f.EstABV), attenuation:toFinite(f.Attenuation),
    source:f.BeerTempSource, gravity:f.GravitySource}));
  updateProfileLocks();
}

// Normalize saved profiles, stopping each one at its first empty step.
function setProfiles(data) {
  state.profiles = data.profiles.map(p => ({name:p.name, used:'',
    // The runner stops at the first empty slot, even if later slots contain data.
    steps:p.steps.slice(0,profileStepCount(p.steps))
      .map(s => ({type:+s.stepType, start:tempToC(+s.startTemp), end:tempToC(+s.endTemp),
        trigger:+s.sgTrigger, days:+s.days}))}));
  updateProfileLocks();
}

// Find how many steps precede the firmware runner's first empty slot.
function profileStepCount(steps) {
  const end=steps.findIndex(s => !s.stepType && !s.days && !s.startTemp && !s.endTemp);
  return end<0 ? steps.length : end;
}

// Identify profiles used by running or paused fermenters so they cannot be edited.
function updateProfileLocks() {
  state.profiles.forEach((p,i) => {
    p.used = state.fermenters.filter(f => (f.running || f.paused) && f.profile===i+1).map(f => f.name).join(', ');
  });
}

// Normalize probe, Tilt or iSpindel replies for display and editing.
function setSensors(kind, data) {
  if (kind==='probes') state.probes = data.probes.map(p => ({...p, fn:+p.function,
    temp:sensorTemp(p.temperature), adjust:deltaToC(+p.tempAdjust)}));
  if (kind==='tilts') {
    const names=['Red','Green','Black','Purple','Orange','Blue','Yellow','Pink'];
    state.tilts = names.map((name,i) => {
      const p=data.tilts.find(t => +t.colour===i) || {};
      return {...p, index:i, name, fn:p.function===2?2:99, fermenter:p.fermenter??99,
        temp:sensorTemp(p.temperature), sg:toFinite(p.sg), adjust:deltaToC(toFinite(p.tempAdjust)),
        sgAdjust:toFinite(p.sgAdjust), active:!!p.active};
    });
  }
  if (kind==='ispindels') state.ispindels = data.ispindels.map(p => ({...p,
    fn:p.function===2?2:99, temp:sensorTemp(p.temperature), adjust:deltaToC(+p.tempAdjust)}));
}

// Translate saved plug function numbers into editable roles and fermenter assignments.
function setPlugs(data) {
  state.plugs = data.plugs.map(p => ({...p,
    role:p.function<8 ? (p.function%2?'cold':'hot') : p.function===8?'aux1':p.function===9?'aux2':'none',
    fermenter:p.function<8 ? Math.floor(p.function/2) : p.fermenter}));
}
