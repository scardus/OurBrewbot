'use strict';
// Browser state, temperature conversion and tidying of controller replies.

// Device data and unsaved edits live only in this browser tab.
const state = {
  // What is on screen.
  route:'dashboard', sensorTab:'probes', systemTab:'overview', selectedProfile:0, activeConsole:null,
  // Connection and refresh status.
  ready:false, offline:false, refreshPaused:false, logPaused:false, lastUpdated:0, controllerReadAt:0,
  // Display unit, and whether the controller changed it while drafts were open.
  unit:'C', unitChanged:false,
  // The latest controller data, tidied by the set functions below.
  controller:{}, health:{}, debug:{Overrides:[]}, global:{},
  fermenters:[], profiles:[], probes:[], tilts:[], ispindels:[], plugs:[],
  services:[], mqtt:{}, notify:{}, syslog:{}, files:[], pressure:{}, pressureReadAt:0, pressureReadFailed:false,
  pressureReference:'', pressureResult:'', pressureResultError:false,
  // The banners' alerts from /alerts, and the controller boot they came from.
  alerts:[], alertsBoot:''
};

// Unsaved edits. A form's key is its data-form attribute, such as "ferm-0", "profile-2" or "mqtt".
// Drawing a view replaces its forms, so edits are kept here and put back afterwards.
// formDrafts: form key -> the text of each changed form's fields.
const formDrafts = new Map();
// profileDrafts: profile index -> the edited profile, in Celsius. Profiles need this as well as
// formDrafts because adding, moving or deleting a step changes which fields the form has.
const profileDrafts = new Map();
// Inputs show rounded text, but a save must not write that rounding back to the controller.
// formBaselines: form element -> its field text as drawn. A field that still matches was not edited.
const formBaselines = new WeakMap();
// profileFormValues: profile form element -> the exact values behind its rounded fields.
const profileFormValues = new WeakMap();
// Keys of forms whose last save got no clear answer. Saving stays blocked until they are reread.
const unknownForms = new Set();

// Endpoints read at least once, and a counter that stops a slow load drawing over a newer view.
const loaded = new Set();
let routeGeneration = 0;
// True while a save or import runs. Polling and other buttons wait until it finishes.
let saving = false;
// Background polling: whether a poll is running and when each last ran.
let pollBusy = false;
let lastPoll = 0;
let lastLogPoll = 0;
// Log drawer: retained lines and the position to read /logs from next.
let logLines = [];
let logBoot = '';
let logAfter = 0;
let logMore = false;
let toastTimer;

// Numbers shared with the firmware.
const FERMENTER_COUNT = 4;
const MAX_PROFILE_STEPS = 15;
const UNASSIGNED = 99;        // Means no fermenter or no function.
const FUNCTION_BEER_TEMP = 2;
const PLUG_AUX1 = 8;          // Plug functions below this are fermenter heaters and coolers.
const PLUG_AUX2 = 9;
const REPORT_BREWERS = 1;     // Bits in a fermenter's BrewServices value.
const REPORT_BREWFATHER = 2;
const REPORT_MQTT = 8;

// Convert a value to a finite number, using the fallback for invalid input.
const toFinite = (value, fallback=0) => Number.isFinite(Number(value)) ? Number(value) : fallback;

// Convert a Celsius temperature into the selected display unit.
const tempToUnit = value => state.unit === 'F' ? value*1.8+32 : value;

// Format a Celsius temperature in the selected display unit.
const formatTemp = value => value == null ? '—' : tempToUnit(value).toFixed(1);

// Format a gravity reading, using a dash when there is no valid reading.
const formatGravity = value => value > 0 ? (+value).toFixed(4) : '—';

// The firmware's number for a temperature unit letter.
const unitCode = letter => letter === 'F' ? 2 : 1;

// A paused profile still belongs to its fermenter, so it counts as active.
const profileActive = fermenter => fermenter.running || fermenter.paused;

// Convert a Celsius temperature difference into the display unit.
const deltaToUnit = value => state.unit === 'F' ? +(value*1.8).toFixed(2) : value;

// Convert an absolute temperature from the display unit to Celsius.
const tempToC = value => state.unit === 'F' ? (value-32)/1.8 : value;

// Convert a temperature difference from the display unit to Celsius.
const deltaToC = value => state.unit === 'F' ? value/1.8 : value;

// Normalize a sensor temperature to Celsius or mark it as unavailable.
const sensorTemp = value => value == null || +value===-127 || tempToC(toFinite(value,-127))<=-100 ?
  null : tempToC(+value);

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
  if (unit && unit!==state.unit) setController({...state.controller,Unit:unitCode(unit)});
  state.fermenters = data.map(f => ({raw:f, id:+f.Fermenter, name:f.FermenterName,
    beer:f.BeerName, yeast:f.YeastName, status:f.StatusStr, alarmActive:!!f.Alarm,
    power:!!f.Power, tempControl:!!f.TempControl, beerTemp:sensorTemp(f.BeerTemp),
    ambient:sensorTemp(f.AmbientTemp), sg:toFinite(f.SG), og:toFinite(f.OG,1.05),
    tg:toFinite(f.TG,1.01), floor:tempToC(+f.FloorTemp), ceiling:tempToC(+f.CeilingTemp),
    hysteresis:deltaToC(+f.Hysteresis), alarm:deltaToC(+f.AlarmTolerance),
    delay:+f.CompressorDelay, profile:+f.ProfileNo, running:!!f.ProfileRunning,
    step:+f.CurrentStep, hour:+f.CurrentHour, paused:!!f.ProfilePaused,
    liveTest:!!f.LiveTest, brewfather:!!(f.BrewServices&REPORT_BREWFATHER), brewers:!!(f.BrewServices&REPORT_BREWERS),
    mqtt:!!(f.BrewServices&REPORT_MQTT), abv:toFinite(f.EstABV), attenuation:toFinite(f.Attenuation),
    pressure:f.Pressure, pressureState:f.PressureState, pressureAssigned:!!f.PressureAssigned,
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
    p.used = state.fermenters.filter(f => profileActive(f) && f.profile===i+1).map(f => f.name).join(', ');
  });
}

// Tilts and iSpindels can only provide a beer temperature, so anything else reads as unused.
const trackerFunction = value => value===FUNCTION_BEER_TEMP ? FUNCTION_BEER_TEMP : UNASSIGNED;

// Normalize probe, Tilt or iSpindel replies for display and editing.
function setSensors(kind, data) {
  if (kind==='probes') state.probes = data.probes.map(p => ({...p, fn:+p.function,
    temp:sensorTemp(p.temperature), adjust:deltaToC(+p.tempAdjust)}));
  if (kind==='tilts') {
    const names=['Red','Green','Black','Purple','Orange','Blue','Yellow','Pink'];
    state.tilts = names.map((name,i) => {
      const p=data.tilts.find(t => +t.colour===i) || {};
      return {...p, index:i, name, fn:trackerFunction(p.function), fermenter:p.fermenter??UNASSIGNED,
        temp:sensorTemp(p.temperature), sg:toFinite(p.sg), adjust:deltaToC(toFinite(p.tempAdjust)),
        sgAdjust:toFinite(p.sgAdjust), active:!!p.active};
    });
  }
  if (kind==='ispindels') state.ispindels = data.ispindels.map(p => ({...p,
    fn:trackerFunction(p.function), temp:sensorTemp(p.temperature), adjust:deltaToC(+p.tempAdjust)}));
}

// Translate a saved plug function number into its editable role.
// Each fermenter has two functions: even numbers are its heater and odd numbers its cooler.
function plugRole(plugFunction) {
  if (plugFunction<PLUG_AUX1) return plugFunction%2 ? 'cold' : 'hot';
  if (plugFunction===PLUG_AUX1) return 'aux1';
  if (plugFunction===PLUG_AUX2) return 'aux2';
  return 'none';
}

// Translate saved plug function numbers into editable roles and fermenter assignments.
function setPlugs(data) {
  state.plugs = data.plugs.map(p => ({...p, role:plugRole(p.function),
    fermenter:p.function<PLUG_AUX1 ? Math.floor(p.function/2) : p.fermenter}));
}
