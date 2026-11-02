'use strict';
// Exercise production helpers without a browser, packages, or controller access.
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');

function ui() {
  const context=vm.createContext({console,TextEncoder,AbortController,TypeError,setTimeout,clearTimeout});
  for (const name of ['views','state','api','fermenters','profiles','plugs','panels','reporting','actions']) {
    vm.runInContext(fs.readFileSync(path.join(__dirname,'../web/js/'+name+'.js'),'utf8'),context,{filename:name+'.js'});
  }
  vm.runInContext('toast=()=>{};',context);
  return {run:source=>vm.runInContext(source,context),context};
}

function bootstrapHandlers(context) {
  const source=fs.readFileSync(path.join(__dirname,'../web/js/bootstrap.js'),'utf8');
  // Register real handlers without starting the page's timers or initial GETs.
  vm.runInContext(source.slice(0,source.indexOf("\n$('#menu-toggle')")),context);
}

test('the exact build module list compiles together as one browser script',()=>{
  const root=path.join(__dirname,'..');
  const builder=fs.readFileSync(path.join(root,'web/build.py'),'utf8');
  const names=[...builder.match(/SCRIPTS = \(([\s\S]*?)\)/)[1].matchAll(/"([^"]+\.js)"/g)].map(m=>m[1]);
  assert.ok(names.includes('bootstrap.js'));
  new vm.Script(names.map(name=>fs.readFileSync(path.join(root,'web/js',name),'utf8')).join('\n;\n'));
});

test('rerendering keeps each horizontal selector in place within the same view',()=>{
  const cases=[
    {route:'profiles',previous:'profiles',selector:'.profile-list',position:273,expected:273},
    {route:'system',previous:'system',selector:'.tabs',position:144,expected:144},
    {route:'system',previous:'sensors',selector:'.tabs',position:144,expected:0}
  ];
  for (const {route,previous,selector,position,expected} of cases) {
    const {run,context}=ui();
    let scroller={scrollLeft:position};
    const main={dataset:{route:previous},querySelectorAll:()=>[],
      querySelector:name=>name===selector?scroller:null};
    // Replacing markup creates a fresh strip at its default scroll position.
    Object.defineProperty(main,'innerHTML',{set:()=>{scroller={scrollLeft:0};}});
    const other={classList:{remove:()=>{}},setAttribute:()=>{}};
    context.document={querySelectorAll:()=>[],querySelector:name=>
      name==='#main'?main:name.startsWith('form')?null:other};
    context.location={hash:'#'+route};
    run("profiles=()=>''; system=()=>''; render();");
    assert.equal(scroller.scrollLeft,expected,route+' from '+previous);
    assert.equal(main.dataset.route,route);
  }
});

test('profile loading stops at the same empty sentinel as the firmware',()=>{
  const {run}=ui();
  run(`setProfiles({profiles:[{name:'Test',steps:[
    {stepType:1,startTemp:20,endTemp:20,sgTrigger:0,days:2},
    {stepType:0,startTemp:0,endTemp:0,sgTrigger:1.05,days:0},
    {stepType:1,startTemp:22,endTemp:22,sgTrigger:0,days:1}]}]});`);
  assert.equal(run('state.profiles[0].steps.length'),1);
});

test('all ten profile types expose exactly the fields used by the runner',()=>{
  const {run}=ui();
  // Derived from Profile.cpp::isStepComplete(), stepRamps(), and Free Rise's band.
  const cases=[
    {type:0,name:'Temperature over time',inputs:['start','end','days']},
    {type:1,name:'Time over temperature',inputs:['start','end','days']},
    {type:2,name:'Free rise band',inputs:['start','end','days']},
    {type:3,name:'Specific gravity after minimum time',inputs:['end','trigger','days']},
    {type:4,name:'Attenuation after minimum time',inputs:['end','trigger','days']},
    {type:5,name:'Temperature reached',inputs:['end']},
    {type:6,name:'Gravity reached',inputs:['end','trigger']},
    {type:7,name:'Attenuation reached',inputs:['end','trigger']},
    {type:8,name:'Time and gravity ramp',inputs:['start','end','trigger','days']},
    {type:9,name:'Time and attenuation ramp',inputs:['start','end','trigger','days']}
  ];
  for (const {type,name:description,inputs} of cases) {
    const html=run(`profileStep({type:${type},start:20,end:21,trigger:1.01,days:3},0,1,false)`);
    for (const name of ['start','end','trigger','days']) {
      const input=html.match(new RegExp('<input[^>]+name="'+name+'-0"[^>]*>'));
      assert.equal(!!input && !input[0].includes('disabled'),inputs.includes(name),description+' field '+name);
    }
  }
});

test('Fahrenheit profile saves keep unused slots literally zero',()=>{
  const {run}=ui();
  run("state.unit='F'");
  const payload=JSON.parse(run('JSON.stringify(profilePayload(2,{name:"Test",steps:[{type:1,start:20,end:21,trigger:0,days:2}]}))'));
  assert.equal(payload.steps.length,15);
  assert.equal(payload.steps[0].startTemp,68);
  assert.ok(Math.abs(payload.steps[0].endTemp-69.8)<1e-12);
  assert.deepEqual(payload.steps[14],{stepType:0,startTemp:0,endTemp:0,sgTrigger:0,days:0});
});

test('a beer-name edit preserves precise API temperatures and reporting bits',()=>{
  const {run,context}=ui();
  context.form={};
  run(`state.fermenters=[{id:0,raw:{OG:1.054321,TG:1.01001,FloorTemp:18.25,CeilingTemp:19.25,
    Hysteresis:0.125,AlarmTolerance:2.345,BrewServices:4}}];
    data={name:'Vessel',beer:'New name',yeast:'Yeast',og:'1.0543',tg:'1.0100',floor:'18.3',ceiling:'19.3',
      hysteresis:'0.125',alarm:'2.3',delay:'10',power:true,tempControl:true,profile:'0',liveTest:false,
      brewers:false,brewfather:true,mqtt:false};
    formBaselines.set(form,{...data,beer:'Old name'});`);
  let payload=JSON.parse(run('JSON.stringify(fermenterPayload(form,data,0))'));
  assert.equal(payload.BeerName,'New name');
  assert.equal(payload.FloorTemp,18.25);
  assert.equal(payload.CeilingTemp,19.25);
  assert.equal(payload.Hysteresis,0.125);
  assert.equal(payload.AlarmTolerance,2.345);
  assert.equal(payload.OG,1.054321);
  assert.equal(payload.BrewServices,6);
  run("data.floor='18.4'");
  payload=JSON.parse(run('JSON.stringify(fermenterPayload(form,data,0))'));
  assert.equal(payload.FloorTemp,18.4);
});

test('Fahrenheit calibration preserves a received span until it is edited',()=>{
  const {run,context}=ui();
  context.form={};
  run(`state.unit='F'; state.probes=[{index:3,tempAdjust:0.123456789}];
    data={fn:'2',fermenter:'0',adjust:'0.12'}; formBaselines.set(form,{...data});`);
  assert.equal(run("sensorPayload(form,data,'probe',0).tempAdjust"),0.123456789);
  run("data.adjust='0.2'");
  assert.equal(run("sensorPayload(form,data,'probe',0).tempAdjust"),0.2);
});

test('profile-name edits and reverted temperature edits preserve exact values',()=>{
  const {run,context}=ui();
  const fields={name:'Renamed','type-0':'1','start-0':'18.3','end-0':'19.3','days-0':'2'};
  const inputs=Object.entries(fields).map(([name,value])=>({name,value,type:'text'}));
  context.form={querySelectorAll:()=>inputs};
  context.document={querySelector:()=>context.form};
  run(`state.profiles=[{name:'Original',steps:[{type:1,start:18.25,end:19.25,days:2,trigger:0}]}];
    profileFormValues.set(form,clone(state.profiles[0]));
    formBaselines.set(form,{'name':'Original','type-0':'1','start-0':'18.3','end-0':'19.3','days-0':'2'});`);
  assert.equal(run('captureProfile().steps[0].start'),18.25);
  inputs.find(input=>input.name==='start-0').value='18.4';
  run('profileChanged()');
  assert.equal(run('captureProfile().steps[0].start'),18.4);
  inputs.find(input=>input.name==='start-0').value='18.3';
  assert.equal(run('captureProfile().steps[0].start'),18.25);
  const payload=JSON.parse(run('JSON.stringify(profilePayload(0,captureProfile()))'));
  assert.equal(payload.steps[0].startTemp,18.25);
  assert.equal(payload.steps[0].endTemp,19.25);
});

test('held profile steps ignore stale Start in the drawn line and axis',()=>{
  const {run}=ui();
  for (const type of [3,4,5,6,7]) {
    const html=run(`profilePlot({name:'Hold',steps:[{type:${type},start:-100,end:20}]})`);
    const line=html.match(/<line[^>]+y1="([^"]+)" y2="([^"]+)"\s+stroke="#ed7182"/);
    assert.ok(line,'held line exists for '+type);
    assert.equal(line[1],line[2]);
    assert.ok(html.includes('17.0°')&&html.includes('23.0°'),'axis excludes ignored Start');
  }
  const band=run("profilePlot({name:'Free rise',steps:[{type:2,start:18,end:22}]})");
  assert.match(band,/<rect[^>]+[\s\S]*fill-opacity="0.25"/);
});

test('dashboard includes paused profiles in its running count',()=>{
  const {run}=ui();
  run(`state.fermenters=[{name:'Vessel',profile:1,running:false,paused:true}]; fermCard=()=>'';`);
  assert.match(run('dashboard()'),/Running profiles<\/div><div[^>]*>1<\/div>/);
});

function saveFixture() {
  const fixture=ui();
  const inputs=Object.entries({name:'Profile',host:'broker',port:'1883',username:'',password:'',baseTopic:'new-topic'})
    .map(([name,value])=>({name,value,type:'text'}));
  const button={disabled:false};
  const form={dataset:{form:'mqtt',dirty:'true',unit:'C'},reportValidity:()=>true,
    querySelectorAll:selector=>selector==='[name]'?inputs:[],querySelector:()=>button};
  fixture.context.form=form;
  fixture.run('request=async()=>({data:{status:"ok"}}); render=()=>{}; saveError=(form,message)=>{form.error=message;};');
  return {...fixture,form};
}

test('a failed confirmation read retains the profile draft until a successful refresh',async()=>{
  const {run,form}=saveFixture();
  form.dataset.form='profile-0';
  run(`state.profiles=[{name:'Original',steps:[]}]; profileDrafts.set(0,{name:'Draft',steps:[]});
    drafts.set('profile-0',{}); captureProfile=()=>profileDrafts.get(0);
    fetchData=async name=>{if(name==='profiles') throw new Error('Read failed');};`);
  await run('saveForm(form)');
  assert.match(form.error,/Save acknowledged/);
  assert.equal(run("profileDrafts.get(0).name"),'Draft');
  assert.equal(form.dataset.unknown,'true');
});

test('a successful save keeps another draft blocked after a unit change',async()=>{
  const {run,form}=saveFixture();
  run(`drafts.set('mqtt',{}); drafts.set('probe-0',{adjust:'0.7'});
    fetchData=async()=>{state.unitChanged=true; state.unit='F';};`);
  await run('saveForm(form)');
  assert.equal(run('state.unitChanged'),true);
  assert.equal(run("drafts.has('probe-0')"),true);
  form.dataset.form='probe-0';
  form.dataset.dirty='true';
  await run('saveForm(form)');
  assert.match(form.error,/Controller units changed/);
});

test('MQTT keeps a blank password and sends an empty value only for explicit clearing',()=>{
  const {run}=ui();
  run("data={baseTopic:'test',password:''};");
  assert.equal(run("'password' in mqttPayload(data)"),false);
  run('data.clearPassword=true;');
  assert.equal(run('mqttPayload(data).password'),'');
});

test('connection age updates preserve the visible status dot',()=>{
  const {run,context}=ui();
  const connection={};
  context.document={querySelectorAll:()=>[],querySelector:()=>connection};
  run('updateAge()');
  assert.match(connection.innerHTML,/<i><\/i> Controller online/);
  run("state.scenario='offline'; updateAge()");
  assert.match(connection.innerHTML,/<i[^>]*><\/i> Connection lost/);
});

test('controller polling updates visible values without replacing settings forms',()=>{
  const {run,context}=ui();
  const nodes=[{dataset:{controller:'FreeHeap'},textContent:'Old heap'},
    {dataset:{controller:'Uptime'},textContent:'Old uptime'}];
  const unit={},connection={};
  context.document={querySelectorAll:selector=>selector==='[data-controller]'?nodes:[],
    querySelector:selector=>{
      if (selector==='#unit-toggle') return unit;
      if (selector==='#connection') return connection;
      throw new Error('Unexpected form or page access: '+selector);
    }};
  run('state.controller={FreeHeap:18000,Uptime:12}; updateLiveReadings();');
  assert.equal(nodes[0].textContent,'18000 bytes');
  assert.equal(nodes[1].textContent,'12 min');
  run('state.controller={FreeHeap:19000,Uptime:13}; updateLiveReadings();');
  assert.equal(nodes[0].textContent,'19000 bytes');
  assert.equal(nodes[1].textContent,'13 min');
});

test('a rejected log cursor resets before the next request',async()=>{
  const {run,context}=ui();
  const status={};
  const paths=[];
  context.document={hidden:false,addEventListener:()=>{},querySelector:()=>status};
  bootstrapHandlers(context);
  run("state.ready=true; logBoot='old'; logAfter=999; renderLogs=()=>{};");
  context.fetch=async url=>{
    paths.push(url);
    if (paths.length===1) return {ok:false,status:400,text:async()=>'Invalid cursor'};
    return {ok:true,status:200,text:async()=>'',headers:{get:name=>({'X-Log-Boot':'new','X-Log-Next':'0'}[name]||'0')}};
  };
  await run('pollLogs()');
  assert.equal(run('logAfter'),0);
  await run('pollLogs()');
  assert.deepEqual(paths,['/logs?boot=old&after=999','/logs?boot=&after=0']);
});

test('the import handler clears selection so the same file can be chosen again',()=>{
  const {run,context}=ui();
  const handlers={};
  context.document={addEventListener:(name,callback)=>{handlers[name]=callback;}};
  bootstrapHandlers(context);
  run('selected=[]; importConfig=async file=>{selected.push(file.name);};');
  const input={id:'import-file',files:[{name:'same.json'}],value:'same.json',closest:()=>null};
  handlers.change({target:input});
  assert.equal(input.value,'');
  input.value='same.json';
  handlers.change({target:input});
  assert.deepEqual(JSON.parse(run('JSON.stringify(selected)')),['same.json','same.json']);
  assert.equal(input.value,'');
});

test('standalone RF tests wait for a poll without losing or duplicating the click',async()=>{
  const elements={};
  const paths=[];
  let poll,releasePoll,markStarted;
  const started=new Promise(resolve=>{markStarted=resolve;});
  const context=vm.createContext({AbortController,setTimeout,clearTimeout,confirm:()=>true,
    Option:function(label,value){this.value=value;},
    document:{hidden:false,getElementById:id=>elements[id]||=({value:'',replaceChildren:()=>{},add:()=>{}})},
    setInterval:callback=>{poll=callback;},
    fetch:async(path,options)=>{
      paths.push(path);
      if (path==='/rf/sniff/poll') {
        await new Promise(resolve=>{releasePoll=resolve;markStarted();});
      }
      if (path==='/smartplug/test') assert.deepEqual(JSON.parse(options.body),{index:0,action:'on'});
      return {ok:true,json:async()=>path==='/smartplugs'?{plugs:[]}:{received:false,msg:'Sent'}};
    }});
  const html=fs.readFileSync(path.join(__dirname,'../web/pages/rf-sniff.html'),'utf8');
  vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1],context);
  await vm.runInContext('requestQueue',context);
  elements.plug.value='0';
  const polling=poll();
  await started;
  const testing=vm.runInContext("test('on')",context);
  assert.deepEqual(paths,['/smartplugs','/rf/sniff/poll']);
  assert.equal(elements.on.disabled,true);
  releasePoll();
  await Promise.all([polling,testing]);
  assert.deepEqual(paths,['/smartplugs','/rf/sniff/poll','/smartplug/test']);
  assert.equal(elements.on.disabled,false);
});

test('unit changes retain drafts and block their conversion without review',()=>{
  const {run}=ui();
  run("state.ready=true; drafts.set('ferm-0',{floor:'18'}); setController({Unit:2});");
  assert.equal(run('state.unitChanged'),true);
  assert.equal(run("drafts.get('ferm-0').floor"),'18');
});

test('paused profiles remain locked for editing',()=>{
  const {run}=ui();
  run("state.profiles=[{name:'Test'}]; state.fermenters=[{name:'Vessel',profile:1,running:false,paused:true}]; updateProfileLocks();");
  assert.equal(run('state.profiles[0].used'),'Vessel');
});

test('requests are serialized and explicit actions precede queued polls',async()=>{
  const {run,context}=ui();
  const paths=[];
  let release;
  context.fetch=async url=>{
    paths.push(url);
    if (url==='/first') await new Promise(resolve=>{release=resolve;});
    return {ok:true,headers:{},text:async()=>'{}'};
  };
  const first=run("get('/first')");
  const poll=run("get('/poll',true)");
  const save=run("post('/save',{})");
  assert.deepEqual(paths,['/first']);
  release();
  await Promise.all([first,poll,save]);
  assert.deepEqual(paths,['/first','/save','/poll']);
});

test('HTTP 200 application errors are failures and malformed POST replies are uncertain',async()=>{
  const {run,context}=ui();
  context.fetch=async()=>({ok:true,status:200,headers:{},text:async()=>'{"status":"error","msg":"Refused"}'});
  await assert.rejects(run("post('/mqtt/test',{})"),error=>error.server&&error.message==='Refused');
  context.fetch=async()=>({ok:true,status:200,headers:{},text:async()=>'{partial'});
  await assert.rejects(run("post('/mqtt',{})"),error=>error.unknown&&error.message.startsWith('Result unknown:'));
});

test('a lost POST reply is not retried',async()=>{
  const {run,context}=ui();
  let calls=0;
  context.fetch=async()=>{calls++;throw new TypeError('Disconnected');};
  await assert.rejects(run("post('/profile',{})"),error=>error.unknown);
  assert.equal(calls,1);
});

test('uncertain saves require a fresh read before another explicit submission',async()=>{
  const {run,context}=ui();
  context.form={dataset:{form:'mqtt',dirty:'true',unknown:'true'},reportValidity:()=>true};
  run("saveError=(form,message)=>{form.error=message;};");
  await run('saveForm(form)');
  assert.match(context.form.error,/Refresh/);
  assert.equal(run('requests.length'),0);
});

test('failed imports request rollback and do not reboot',async()=>{
  const {run,context}=ui();
  context.file={name:'test.json',text:async()=>'{"configs":{"global":{"unknownField":42},"mqtt":{"enabled":false}}}'};
  run("confirmAction=(a,b,c,callback)=>{pending=callback();}; calls=[]; request=async(path,options)=>{calls.push([path,options]); if(path.includes('jsonMqtt')) throw new Error('Write failed'); return {data:{}};};");
  await run('importConfig(file)');
  await run('pending');
  const calls=JSON.parse(run('JSON.stringify(calls)'));
  assert.equal(calls.length,3);
  assert.equal(calls[0][1].body,'{"unknownField":42}');
  assert.equal(calls[2][0],'/fs/import/abort');
  assert.equal(run('saving'),false);
});

test('a lost reboot reply does not roll back successfully imported files',async()=>{
  const {run,context}=ui();
  context.file={name:'test.json',text:async()=>'{"configs":{"global":{"unknownField":42}}}'};
  run("confirmAction=(a,b,c,callback)=>{pending=callback();}; calls=[]; request=async(path)=>{calls.push(path); if(path==='/reboot') throw new Error('Connection closed'); return {data:{}};};");
  await run('importConfig(file)');
  await run('pending');
  assert.deepEqual(JSON.parse(run('JSON.stringify(calls)')),['/fs/save?name=%2FjsonGlobal.txt','/reboot']);
  assert.equal(run('saving'),false);
});
