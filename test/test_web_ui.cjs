'use strict';
// Exercise production helpers without a browser, packages, or controller access.
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');

function ui() {
  const context=vm.createContext({console,TextEncoder,AbortController,TypeError,setTimeout,clearTimeout});
  for (const name of ['html','state','api','forms','app','fermenters','sensors','profiles','plugs','reporting','system','logs']) {
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
    run("profilesView=()=>''; systemView=()=>''; render();");
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
  assert.match(run('dashboardView()'),/Running profiles<\/div>\s*<div[^>]*>1<\/div>/);
});

test('the dashboard menu item counts the fermenters that are powered on, and follows polling',()=>{
  const {run,context}=ui();
  const elements={};
  const count={textContent:''};
  context.document={querySelectorAll:selector=>selector==='.nav-count'?[count]:[],querySelector:selector=>{
    if (selector.startsWith('form')) return null;
    return elements[selector]??={classList:{remove:()=>{}},setAttribute:()=>{},dataset:{},querySelector:()=>null,querySelectorAll:()=>[]};
  }};
  context.location={hash:'#profiles'};
  run(`profilesView=()=>'';
    state.fermenters=[{power:true},{power:false},{power:false},{power:false}];
    render();`);
  assert.match(elements['#navigation'].innerHTML,/<span class="nav-count"[^>]*>1<\/span>/);
  // The profiles view polls fermenters without redrawing, so the number updates on its own.
  run('state.fermenters[1].power=true; updateLiveReadings();');
  assert.equal(count.textContent,2);
});

test('the profile dropdown is locked while a profile runs or is paused, as the firmware requires',()=>{
  const {run,context}=ui();
  const elements={};
  context.document={querySelectorAll:()=>[],querySelector:selector=>
    elements[selector]??={open:true,showModal:()=>{},querySelector:()=>null,querySelectorAll:()=>[]}};
  run(`mountForms=()=>{}; state.global={debug:false}; state.profiles=[{name:'Lager',steps:[{}]}];
    state.fermenters=[{name:'Vessel',beer:'Beer',yeast:'',beerTemp:20,status:'Idle',alarmActive:false,
      og:1.05,tg:1.01,floor:18,ceiling:20,hysteresis:0.5,alarm:3,delay:10,power:true,tempControl:true,
      liveTest:false,brewfather:false,brewers:false,mqtt:false,profile:1,running:false,paused:false,step:0,hour:0}];`);
  const profileSelect=()=>elements['#editor-content'].innerHTML.match(/<select name="profile"[^>]*>/)[0];
  for (const [running,paused,locked] of [[false,false,false],[true,false,true],[false,true,true]]) {
    run(`state.fermenters[0].running=${running}; state.fermenters[0].paused=${paused}; openFerm(0);`);
    assert.equal(profileSelect().includes('disabled'),locked,'running='+running+' paused='+paused);
    assert.equal(elements['#editor-content'].innerHTML.includes('Stop the profile to choose another.'),locked);
  }
});

test('sensor offset fields carry the firmware limits in the display unit',()=>{
  const {run}=ui();
  const sensor="{name:'Tilt',fn:99,fermenter:99,adjust:0,sgAdjust:0,temp:20,sg:1.05,active:true}";
  for (const [unit,limit] of [['C',10],['F',18]]) {
    run(`state.unit='${unit}'; state.fermenters=[];`);
    const html=run(`sensorCard('tilts',${sensor},0)`);
    assert.match(html,new RegExp(`name="adjust"[^>]*min="-${limit}" max="${limit}"`),unit);
    assert.match(html,/name="sgAdjust"[^>]*min="-0.05" max="0.05"/);
  }
});

function saveFixture() {
  const fixture=ui();
  const inputs=Object.entries({name:'Profile',host:'broker',port:'1883',username:'',password:'',baseTopic:'new-topic'})
    .map(([name,value])=>({name,value,type:'text'}));
  const button={disabled:false};
  const form={dataset:{form:'mqtt',dirty:'true',unit:'C'},elements:[],
    querySelectorAll:selector=>selector==='[name]'?inputs:[],querySelector:()=>button};
  fixture.context.form=form;
  fixture.run('request=async()=>({data:{status:"ok"}}); render=()=>{}; saveError=(form,message)=>{form.error=message;};');
  return {...fixture,form};
}

test('a failed confirmation read retains the profile draft until a successful refresh',async()=>{
  const {run,form}=saveFixture();
  form.dataset.form='profile-0';
  run(`state.profiles=[{name:'Original',steps:[]}]; profileDrafts.set(0,{name:'Draft',steps:[]});
    formDrafts.set('profile-0',{}); captureProfile=()=>profileDrafts.get(0);
    fetchData=async name=>{if(name==='profiles') throw new Error('Read failed');};`);
  await run('saveForm(form)');
  assert.match(form.error,/Save acknowledged/);
  assert.equal(run("profileDrafts.get(0).name"),'Draft');
  assert.equal(form.dataset.unknown,'true');
});

test('a successful save keeps another draft blocked after a unit change',async()=>{
  const {run,form}=saveFixture();
  run(`formDrafts.set('mqtt',{}); formDrafts.set('probe-0',{adjust:'0.7'});
    fetchData=async()=>{state.unitChanged=true; state.unit='F';};`);
  await run('saveForm(form)');
  assert.equal(run('state.unitChanged'),true);
  assert.equal(run("formDrafts.has('probe-0')"),true);
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
  run("state.offline=true; updateAge()");
  assert.match(connection.innerHTML,/<i[^>]*><\/i> Connection lost/);
});

test('controller polling updates visible values without replacing settings forms',()=>{
  const {run,context}=ui();
  const nodes=[{dataset:{controller:'FreeHeap'},textContent:'Old heap'},
    {dataset:{controller:'Uptime'},textContent:'Old uptime'}];
  const unit={},connection={};
  context.document={querySelectorAll:selector=>selector==='[data-controller]'?nodes:[],
    querySelector:selector=>{
      if (selector==='#unit-label') return unit;
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

test('opening the log drawer follows new entries until the user scrolls back',()=>{
  const {run,context}=ui();
  const elements={};
  const drawer=elements['#log-content']={hidden:true};
  let height=1000,position=100;
  const lines=elements['#log-lines']={clientHeight:200};
  Object.defineProperties(lines,{
    scrollHeight:{get:()=>height},
    scrollTop:{get:()=>position,set:value=>{position=Math.max(0,Math.min(value,height-200));}},
    innerHTML:{set:html=>{height=(html.match(/class="log-line /g)||[]).length*20;}}
  });
  elements['#log-search']={value:''};
  elements['#log-filter']={value:'all'};
  context.document={addEventListener:()=>{},querySelector:selector=>
    elements[selector]||=({addEventListener:()=>{},setAttribute:()=>{}})};
  context.window={addEventListener:()=>{}};
  const source=fs.readFileSync(path.join(__dirname,'../web/js/bootstrap.js'),'utf8');
  // Register the real drawer controls without starting polling or initial GETs.
  vm.runInContext(source.slice(0,source.indexOf('\nsetInterval(')),context);
  run("for(let i=0;i<50;i++) addLog('Line '+i);");

  elements['#log-toggle'].onclick();
  assert.equal(drawer.hidden,false);
  assert.equal(position,800);
  run("addLog('New line'); renderLogs();");
  assert.equal(position,820);

  lines.scrollTop=200;
  run("addLog('Another line'); renderLogs();");
  assert.equal(position,200);
  elements['#log-toggle'].onclick();
  assert.equal(drawer.hidden,true);
  assert.equal(position,200);
  elements['#log-toggle'].onclick();
  assert.equal(position,840);
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
  run("state.ready=true; formDrafts.set('ferm-0',{floor:'18'}); setController({Unit:2});");
  assert.equal(run('state.unitChanged'),true);
  assert.equal(run("formDrafts.get('ferm-0').floor"),'18');
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
  context.form={dataset:{form:'mqtt',dirty:'true',unknown:'true'},elements:[]};
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

// Send one form through the real save flow and report what reached the controller.
async function saveThrough(key,fields,setup='') {
  const {run,context}=ui();
  const inputs=Object.entries(fields).map(([name,value])=>
    typeof value==='boolean'?{name,checked:value,type:'checkbox'}:{name,value,type:'text'});
  const form={dataset:{form:key,dirty:'true',unit:'C'},elements:[],
    querySelectorAll:selector=>selector==='[name]'?inputs:[],querySelector:()=>({disabled:false})};
  context.form=form;
  context.document={querySelectorAll:()=>[],querySelector:selector=>
    selector.startsWith('form')?null:{close:()=>{}}};
  run(`posts=[]; reads=[]; render=()=>{}; saveError=(form,message)=>{form.error=message;};
    fetchData=async name=>{reads.push(name);};
    request=async(path,options)=>{posts.push({path,body:JSON.parse(options.body)}); return {data:{status:'ok',msg:'Saved'}};};`);
  run(setup);
  await run('saveForm(form)');
  return {posts:JSON.parse(run('JSON.stringify(posts)')),reads:JSON.parse(run('JSON.stringify(reads)')),error:form.error};
}

const fermenterFields={name:'Vessel',beer:'Beer',yeast:'Yeast',og:'1.05',tg:'1.01',floor:'18',ceiling:'20',hysteresis:'0.5',
  alarm:'2',delay:'10',power:true,tempControl:false,profile:'1',liveTest:false,brewers:true,brewfather:false,mqtt:true};
const fermenterSetup="state.fermenters=[{id:0,raw:{BrewServices:4}}];";
const profileSetup=`state.profiles=[{name:'Old',used:'',steps:[]}];
  profileDrafts.set(0,{name:'New',steps:[{type:1,start:20,end:21,trigger:0,days:2}]});`;
const plugSetup="state.plugs=[{index:7}];";
const plugFields={role:'hot',fermenter:'1',manufacturer:'Make',model:'Model',onCode:'11',offCode:'22',protocol:'1',bits:'24',delay:'300'};
const mqttFields={enabled:true,host:'broker',port:'1883',username:'user',password:'',baseTopic:'topic',
  haDiscovery:true,allowControl:false,logEnabled:false,clearPassword:false};
const serviceFields={enabled:true,serviceId:'key',deviceName:'device'};
const syslogFields={enabled:true,host:'server',port:'514',facility:'16',level:'6'};
const notifyFields={enabled:true,url:'http://ntfy.sh/topic',alarms:true,sensors:true,restarts:false,
  profile:false,updates:true};

test('log polling keeps the status dot, and shows a failure in amber',async()=>{
  const {run,context}=ui();
  const status={};
  context.document={hidden:false,addEventListener:()=>{},querySelector:()=>status};
  run('state.ready=true; renderLogs=()=>{};');
  context.fetch=async()=>({ok:true,status:200,text:async()=>'',headers:{get:()=>null}});
  await run('pollLogs()');
  assert.equal(status.innerHTML,'<i></i> Controller stream');
  context.fetch=async()=>({ok:false,status:500,text:async()=>'{}'});
  await run('pollLogs()');
  assert.match(status.innerHTML,/^<i style="background:var\(--amber\)"><\/i> Log connection lost/);
});

test('the footer shows the firmware version once the controller has been read',()=>{
  const {run,context}=ui();
  const footer={};
  const other={classList:{remove:()=>{}},setAttribute:()=>{},dataset:{},querySelector:()=>null,querySelectorAll:()=>[]};
  context.document={querySelectorAll:()=>[],querySelector:name=>
    name==='.footer-version'?footer:name.startsWith('form')?null:other};
  context.location={hash:'#dashboard'};
  run("dashboardView=()=>''; state.controller={FirmwareVersion:'1.2.3'}; render();");
  assert.equal(footer.textContent,'1.2.3');
});

test('the route ignores anything after a slash, at start-up as well as on navigation',async()=>{
  const {run,context}=ui();
  context.location={hash:'#system/details'};
  assert.equal(run('currentRoute()'),'system');
  context.location.hash='#unknown';
  assert.equal(run('currentRoute()'),'dashboard');
  context.location.hash='#system/details';
  context.document={querySelector:()=>({}),addEventListener:()=>{}};
  run('fetchData=async()=>{}; loadRoute=async()=>{};');
  const source=fs.readFileSync(path.join(__dirname,'../web/js/bootstrap.js'),'utf8');
  // Run only the start-up block, without registering handlers or timers.
  await vm.runInContext(source.slice(source.indexOf('\n(async()=>{')),context);
  assert.equal(run('state.route'),'system');
});

test('a value finer than its step can be saved, but empty and out-of-range values cannot',()=>{
  const {run,context}=ui();
  const check=validity=>{
    const field={willValidate:true,validity:{valid:false,...validity},reported:false,reportValidity(){this.reported=true;}};
    context.form={elements:[field]};
    return {allowed:run('fieldsAreValid(form)'),reported:field.reported};
  };
  assert.deepEqual(check({stepMismatch:true}),{allowed:true,reported:false});
  assert.deepEqual(check({stepMismatch:true,rangeOverflow:true}),{allowed:false,reported:true});
  assert.deepEqual(check({rangeUnderflow:true}),{allowed:false,reported:true});
  assert.deepEqual(check({valueMissing:true}),{allowed:false,reported:true});
  assert.deepEqual(check({badInput:true}),{allowed:false,reported:true});
  context.form={elements:[{willValidate:false,validity:{valid:false,valueMissing:true}},{willValidate:true,validity:{valid:true}}]};
  assert.equal(run('fieldsAreValid(form)'),true);
});

test('a console says whether it is polling, and its first RF code starts on the first line',async()=>{
  const {run,context}=ui();
  const elements={'#rf-output':{dataset:{},textContent:''}};
  context.document={addEventListener:()=>{},hidden:false,querySelector:selector=>elements[selector]||null};
  bootstrapHandlers(context);
  run("state.route='system'; state.systemTab='rf'; state.plugs=[];");
  assert.match(run('systemPanels.rf()'),/id="rf-output" data-empty="Receiver polling is stopped\."><\/div>/);
  context.target={dataset:{action:'console-start',value:'rf'}};
  await run('handleAction(target)');
  assert.equal(elements['#rf-output'].dataset.empty,'Listening for RF codes…');
  context.fetch=async()=>({ok:true,status:200,headers:{},text:async()=>
    '{"received":true,"code":1,"hex":"1","bits":24,"delay":410,"protocol":1}'});
  await run('pollConsole()');
  assert.match(elements['#rf-output'].textContent,/^Code 1 /);
  context.target={dataset:{action:'console-stop'}};
  await run('handleAction(target)');
  assert.equal(elements['#rf-output'].dataset.empty,'Receiver polling is stopped.');
});

test('every form saves to its endpoint with the expected body and confirmation reads',async()=>{
  const emptySteps=Array.from({length:14},()=>({stepType:0,startTemp:0,endTemp:0,sgTrigger:0,days:0}));
  const cases=[
    {key:'ferm-0',fields:fermenterFields,setup:fermenterSetup,path:'/fermenter',reads:['controller','fermenters'],
      body:{Fermenter:0,FermenterName:'Vessel',BeerName:'Beer',YeastName:'Yeast',OG:1.05,TG:1.01,CeilingTemp:20,FloorTemp:18,
        Hysteresis:0.5,AlarmTolerance:2,CompressorDelay:10,Power:true,TempControl:false,ProfileNo:1,LiveTest:false,BrewServices:13}},
    {key:'profile-0',fields:{name:'New'},setup:profileSetup,path:'/profile',reads:['controller','fermenters','profiles'],
      body:{index:0,name:'New',steps:[{stepType:1,startTemp:20,endTemp:21,sgTrigger:0,days:2},...emptySteps]}},
    {key:'probe-0',fields:{fn:'2',fermenter:'1',adjust:'0.5'},setup:'state.probes=[{index:3,tempAdjust:0}];',path:'/probes',
      reads:['controller','probes'],body:{function:2,fermenter:1,tempAdjust:0.5,index:3}},
    {key:'tilt-2',fields:{fn:'2',fermenter:'1',adjust:'0.5',sgAdjust:'0.001'},setup:'state.tilts=[{},{},{tempAdjust:0,sgAdjust:0}];',
      path:'/tilt',reads:['controller','tilts'],body:{function:2,fermenter:1,tempAdjust:0.5,colour:2,sgAdjust:0.001}},
    {key:'ispindel-0',fields:{fn:'99',fermenter:'0',unit:'1',adjust:'0.5',sgAdjust:'0.001'},setup:'state.ispindels=[{index:4,tempAdjust:0,sgAdjust:0}];',
      path:'/ispindel/config',reads:['controller','ispindels'],body:{function:99,fermenter:0,tempAdjust:0.5,index:4,sgAdjust:0.001,unit:1}},
    {key:'plug-0',fields:plugFields,setup:plugSetup,path:'/smartplug',reads:['smartplugs'],
      body:{index:7,manufacturer:'Make',model:'Model',onCode:11,offCode:22,protocol:1,bits:24,delay:300,function:2,fermenter:1}},
    {key:'service-1',fields:serviceFields,path:'/brewservices',reads:['brewservices'],
      body:{index:1,enabled:true,serviceId:'key',deviceName:'device'}},
    {key:'mqtt',fields:mqttFields,path:'/mqtt',reads:['mqtt'],
      body:{enabled:true,host:'broker',port:1883,username:'user',baseTopic:'topic',haDiscovery:true,allowControl:false,logEnabled:false}},
    {key:'notify',fields:notifyFields,path:'/notify',reads:['notify'],
      body:{enabled:true,url:'http://ntfy.sh/topic',alarms:true,sensors:true,restarts:false,profile:false,updates:true}},
    {key:'global',fields:{unit:'C',resolution:'11',alarm:'60',mdns:true,updates:false,crashes:true},path:'/controller',reads:['controller'],
      body:{Unit:1,Resolution:11,AlarmDwellSec:60,MdnsEnabled:true,UpdateCheck:false,CrashReports:true}},
    {key:'debug-mode',fields:{enabled:true},path:'/debug',reads:['debug'],body:{DebugMode:true}},
    {key:'syslog',fields:syslogFields,path:'/syslog',reads:['syslog'],
      body:{enabled:true,host:'server',port:514,facility:16,minLevel:6}}
  ];
  for (const {key,fields,setup,path:endpoint,reads,body} of cases) {
    const result=await saveThrough(key,fields,setup);
    assert.equal(result.error,'Saving…',key);
    assert.deepEqual(result.posts,[{path:endpoint,body}],key);
    assert.deepEqual(result.reads,reads,key);
  }
});

test('the sensors page warns about Tilts or iSpindels already sharing a fermenter',()=>{
  const {run}=ui();
  run(`state.fermenters=[{id:0,name:'Vessel <1>'},{id:1,name:'Vessel 2'}];
    setSensors('tilts',{tilts:[{colour:0,fermenter:0},{colour:1,fermenter:0},{colour:2,fermenter:1},{colour:3,fermenter:99},{colour:4,fermenter:99}]});
    setSensors('ispindels',{ispindels:[{index:0,name:'A&B',fermenter:1},{index:1,name:'C',fermenter:1},{index:2,name:'D',fermenter:0},{index:3,name:'',fermenter:99}]});
    setSensors('probes',{probes:[{index:0,function:2,fermenter:0,tempAdjust:0},{index:1,function:3,fermenter:0,tempAdjust:0}]});`);
  const notices=kind=>run(`state.sensorTab='${kind}'; sensorsView()`).match(/<p class="notice error">.*?<\/p>/g)||[];
  assert.deepEqual(notices('tilts'),['<p class="notice error">Vessel &lt;1&gt; has 2 Tilts: Red, Green. Only one is used, so unassign the others.</p>']);
  assert.deepEqual(notices('ispindels'),['<p class="notice error">Vessel 2 has 2 iSpindels: A&amp;B, C. Only one is used, so unassign the others.</p>']);
  // A fermenter normally has several probes, each with its own function.
  assert.deepEqual(notices('probes'),[]);
  run("setSensors('tilts',{tilts:[{colour:0,fermenter:0},{colour:1,fermenter:1}]});");
  assert.deepEqual(notices('tilts'),[]);
});

test('a save the firmware refuses shows its reason and keeps the draft',async()=>{
  const message='Tilt Red is already on this fermenter - unassign it first';
  const result=await saveThrough('tilt-1',{fn:'99',fermenter:'0',adjust:'0',sgAdjust:'0'},
    `state.tilts=[{},{tempAdjust:0,sgAdjust:0}];
    request=async()=>{const error=new Error('${message}'); error.server=true; throw error;};`);
  assert.equal(result.error,message);
  // Only the unit check before saving: no confirmation read, so the form and its draft stay as they are.
  assert.deepEqual(result.reads,['controller']);
});

test('text longer than the firmware buffer is refused before anything is sent',async()=>{
  const cases=[
    ['ferm-0','name',fermenterFields,fermenterSetup,31],['ferm-0','beer',fermenterFields,fermenterSetup,31],
    ['ferm-0','yeast',fermenterFields,fermenterSetup,31],['profile-0','name',{name:'New'},profileSetup,31],
    ['plug-0','manufacturer',plugFields,plugSetup,23],['plug-0','model',plugFields,plugSetup,23],
    ['service-0','serviceId',serviceFields,'',47],['service-0','deviceName',serviceFields,'',31],
    ['mqtt','host',mqttFields,'',63],['mqtt','username',mqttFields,'',31],['mqtt','password',mqttFields,'',47],
    ['mqtt','baseTopic',mqttFields,'',31],['syslog','host',syslogFields,'',63]
  ];
  for (const [key,name,fields,setup,limit] of cases) {
    const accepted=await saveThrough(key,{...fields,[name]:'x'.repeat(limit)},setup);
    assert.equal(accepted.posts.length,1,key+' '+name+' at the limit');
    const refused=await saveThrough(key,{...fields,[name]:'x'.repeat(limit+1)},setup);
    assert.match(refused.error,new RegExp('limit of '+limit+' UTF-8 bytes'),key+' '+name);
    assert.equal(refused.posts.length,0,key+' '+name);
  }
  // The limit counts bytes, not characters: two bytes each, so 32 bytes in total.
  const multibyte=await saveThrough('mqtt',{...mqttFields,username:'é'.repeat(16)});
  assert.match(multibyte.error,/limit of 31/);
});

test('the notification URL limit is the firmware field size, and a switched-off form may clear it',async()=>{
  // 127 bytes fit NotifyConfig.url[128]; the limit counts the whole URL, http:// included.
  const longest='http://'+'x'.repeat(120);
  const accepted=await saveThrough('notify',{...notifyFields,url:longest});
  assert.equal(accepted.posts.length,1);
  const refused=await saveThrough('notify',{...notifyFields,url:longest+'x'});
  assert.match(refused.error,/limit of 127 UTF-8 bytes/);
  assert.equal(refused.posts.length,0);
  const cleared=await saveThrough('notify',{...notifyFields,enabled:false,url:''});
  assert.deepEqual(cleared.posts[0].body,{enabled:false,url:'',alarms:true,sensors:true,restarts:false,
    profile:false,updates:true});
});

test('the notifications card shows the saved settings and escapes the URL',()=>{
  const {run}=ui();
  run(`state.services=[]; state.mqtt={};
    state.notify={enabled:true,url:'http://ntfy.sh/a"b<c',alarms:true,sensors:false,restarts:true,profile:false,updates:false};`);
  const html=run('reportingView()');
  assert.match(html,/<h2>Notifications<\/h2>/);
  assert.match(html,/data-form="notify"/);
  assert.match(html,/value="http:\/\/ntfy\.sh\/a&quot;b&lt;c"/);
  assert.match(html,/name="alarms" checked/);
  assert.doesNotMatch(html,/name="sensors" checked/);
  assert.match(html,/name="restarts" checked/);
  assert.match(html,/data-action="test-notify"/);
});

test('invalid saves show their message and send nothing',async()=>{
  const cases=[
    ['ferm-0',{...fermenterFields,floor:'30'},fermenterSetup,/Temperature band/],
    ['ferm-0',{...fermenterFields,alarm:'11'},fermenterSetup,/Alarm tolerance/],
    ['plug-0',{...plugFields,fermenter:'99'},plugSetup,/Choose a fermenter/],
    ['plug-0',{...plugFields,delay:'65000'},plugSetup,/exceed the firmware limit/],
    ['mqtt',{...mqttFields,baseTopic:'bad/#'},'',/Base topic/],
    ['notify',{...notifyFields,url:'https://ntfy.sh/topic'},'',/must start with http:\/\//],
    ['notify',{...notifyFields,url:'ntfy.sh/topic'},'',/must start with http:\/\//],
    ['notify',{...notifyFields,url:'http://ntfy.sh/my topic'},'',/no spaces/],
    ['notify',{...notifyFields,url:'http://'},'',/must start with http:\/\//],
    ['notify',{...notifyFields,url:''},'',/Enter a notification URL/],
    ['global',{unit:'F',resolution:'11',alarm:'60',mdns:true,updates:true,crashes:true},"formDrafts.set('a',{}); formDrafts.set('b',{});",/other drafts/],
    ['profile-0',{name:'New'},profileSetup+"state.profiles[0].used='Vessel';",/running/]
  ];
  for (const [key,fields,setup,message] of cases) {
    const result=await saveThrough(key,fields,setup);
    assert.match(result.error,message);
    assert.equal(result.posts.length,0,key);
  }
});

test('refreshing clears an uncertain save only when that form was reread',async()=>{
  const endpoints={'ferm-0':'fermenters','profile-0':'profiles','probe-0':'probes','tilt-0':'tilts','ispindel-0':'ispindels',
    'plug-0':'smartplugs','service-0':'brewservices',global:'controller',mqtt:'mqtt',notify:'notify',syslog:'syslog',
    'debug-mode':'debug'};
  for (const [key,endpoint] of Object.entries(endpoints)) {
    const {run,context}=ui();
    context.document={querySelectorAll:()=>[]};
    run(`loadRoute=async()=>true; unknownForms.add('${key}');`);
    run("routeData=()=>['nothing-relevant'];");
    await run('refreshForms()');
    assert.equal(run(`unknownForms.has('${key}')`),true,key);
    run(`routeData=()=>['${endpoint}'];`);
    await run('refreshForms()');
    assert.equal(run(`unknownForms.has('${key}')`),false,key);
  }
});

// Each action, and how to tell that the shared handler really carried it out.
const actionEffects={
  'edit-ferm':{called:'openFerm'},'copy-profile':{called:'chooseProfileCopy'},'copy-confirm':{called:'copyProfile'},
  'step-add':{called:'editProfileStep'},'step-up':{called:'editProfileStep'},'step-down':{called:'editProfileStep'},
  'step-delete':{called:'editProfileStep'},'profile-control':{called:'controlProfile'},'clear-sensor':{called:'clearSensor'},
  'plug-preset':{called:'applyPreset'},'plug-test':{called:'testPlug'},'rf-test':{called:'testPlug'},
  'discard-form':{called:'discardForm'},refresh:{called:'refreshForms'},reconnect:{called:'refreshForms'},
  export:{called:'exportConfig'},'file-view':{called:'viewFile'},'ble-send':{called:'sendBle'},debug:{called:'showDebug'},
  'test-service':{posted:'/brewservices/test'},'test-mqtt':{posted:'/mqtt/test'},discover:{posted:'/mqtt/discover'},
  'test-notify':{posted:'/notify/test'},
  'update-check':{posted:'/update/check'},
  pause:{state:'state.refreshPaused',equals:true},'select-profile':{state:'state.selectedProfile',equals:1},
  'sensor-tab':{state:'state.sensorTab',equals:'chosen'},'system-tab':{state:'state.systemTab',equals:'chosen'},
  'open-rf':{state:'state.systemTab',equals:'rf'},'console-start':{state:'state.activeConsole',equals:'chosen'},
  'console-stop':{state:'state.activeConsole',equals:null},
  'close-editor':{closed:'#editor'},'close-modal':{closed:'#modal'},import:{clicked:'#import-file'},
  'rf-clear':{emptied:'#rf-output'},'ble-clear':{emptied:'#ble-output'}
};

test('every action a view can render is handled, and every handler is still used',()=>{
  const root=path.join(__dirname,'../web');
  const sources=[...fs.readdirSync(path.join(root,'js')).map(name=>path.join(root,'js',name)),path.join(root,'pages/admin.html')]
    .map(file=>fs.readFileSync(file,'utf8')).join('\n');
  const rendered=new Set();
  for (const match of sources.matchAll(/data-action="([a-z-]+)"/g)) rendered.add(match[1]);
  for (const match of sources.matchAll(/\b(?:btn|tabBar)\(.*?,'([a-z-]+)'/g)) rendered.add(match[1]);
  assert.deepEqual([...rendered].sort(),Object.keys(actionEffects).sort());
  const handlers=JSON.parse(ui().run('JSON.stringify(Object.keys(actionHandlers))'));
  assert.deepEqual(handlers.sort(),Object.keys(actionEffects).sort());
});

test('the shared action handler carries out every action',async()=>{
  for (const [name,effect] of Object.entries(actionEffects)) {
    const {run,context}=ui();
    const elements={};
    context.location={hash:''};
    context.document={addEventListener:()=>{},querySelectorAll:()=>[],querySelector:selector=>elements[selector]||=({
      value:'0',textContent:'old',closed:false,clicked:false,innerHTML:'',dataset:{},
      close(){this.closed=true;},click(){this.clicked=true;}
    })};
    bootstrapHandlers(context);
    context.target={dataset:{action:name,id:'1',value:'chosen'},closest:()=>({})};
    run(`state.activeConsole='old'; state.plugs=[{index:0},{index:1}]; state.controller={}; calls=[]; posts=[];
      loadRoute=async()=>{}; render=()=>{}; updateResult=()=>'';
      for (const name of ['openFerm','chooseProfileCopy','copyProfile','editProfileStep','controlProfile','clearSensor','applyPreset',
        'testPlug','discardForm','refreshForms','exportConfig','viewFile','sendBle','showDebug']) {
        globalThis[name]=()=>{calls.push(name);};
      }
      request=async path=>{posts.push(path); return {data:{status:'ok',msg:'Done'}};};`);
    await run('handleAction(target)');
    if (effect.called) assert.deepEqual(JSON.parse(run('JSON.stringify(calls)')),[effect.called],name);
    if (effect.posted) assert.deepEqual(JSON.parse(run('JSON.stringify(posts)')),[effect.posted],name);
    if ('state' in effect) assert.equal(run(effect.state),effect.equals,name);
    if (effect.closed) assert.equal(elements[effect.closed].closed,true,name);
    if (effect.clicked) assert.equal(elements[effect.clicked].clicked,true,name);
    if (effect.emptied) assert.equal(elements[effect.emptied].textContent,'',name);
  }
});

test('while saving, only the close actions still respond',async()=>{
  for (const name of Object.keys(actionEffects)) {
    const {run,context}=ui();
    const closed=[];
    context.document={querySelector:selector=>({close:()=>closed.push(selector)})};
    context.target={dataset:{action:name},closest:()=>({})};
    run('saving=true; state.refreshPaused=false; openFerm=()=>{throw new Error("ran while saving");};');
    await run('handleAction(target)');
    assert.equal(run('state.refreshPaused'),false,name);
    assert.equal(closed.length>0,name==='close-modal'||name==='close-editor',name);
  }
});
