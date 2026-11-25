const {test}=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const fs=require('node:fs');
function app(){
 const nodes=new Map();
 function node(){return {disabled:false,hidden:false,value:'',textContent:'',children:[],dataset:{},classList:{toggle(){}},append(...x){this.children.push(...x)},replaceChildren(){this.children=[]},setAttribute(){}};}
 const get=id=>{if(!nodes.has(id))nodes.set(id,node());return nodes.get(id)};
 const document={getElementById:get,createElement:node,createTextNode:text=>({textContent:text}),querySelectorAll:q=>q==='[data-device]'?get('devices').children.flatMap(r=>r.children[1]?.children||[]).filter(b=>b.dataset.device):[]};
 const context=vm.createContext({document,window:{isSecureContext:true,addEventListener(){}},navigator:{serial:{}},localStorage:{getItem(){return null},setItem(){}},setTimeout,clearTimeout,console,TextEncoder,TextDecoder});
 for(const file of ['serial.js','app.js'])vm.runInContext(fs.readFileSync(`${__dirname}/../desktop/web/${file}`,'utf8'),context);
 return {run:code=>vm.runInContext(code,context),get};
}
test('connected peer with zero channels can start and auto prepares before capture',async()=>{
 const a=app();
 a.run("run.setup(1); identity={build:'TEST'}; peer={connected:true,secure:true,address:'aa:bb:cc:dd:ee:ff',address_type:'public'}; subscriptions=[];needsFreshLink=false;var commands=[];link={request:async(cmd,args)=>{commands.push(cmd);if(cmd==='discover')return {services:[{uuid:'1812',characteristics:[{uuid:'2a4d',handle:43,notify:true}]}]};if(cmd==='status')return peer;return {status:0};}};render();");
 a.run("deviceList([{address:'aa:bb:cc:dd:ee:ff'}],true)");
 assert.equal(a.run("document.querySelectorAll('[data-device]')[0].disabled"),false);
 assert.equal(a.run("document.querySelectorAll('[data-device]')[0].textContent"),'准备采样');
 assert.equal(a.get('start').disabled,false);
 await a.run("document.getElementById('start').onclick()");
 assert.equal(a.run('run.active'),0);assert.equal(a.run('subscriptions[0]'),43);
 assert.equal(a.run("commands.includes('discover')&&commands.includes('subscribe')"),true);
});
test('opening serial automatically rebuilds stale BLE connection and prepares notifications',async()=>{
 const a=app();
 a.run("var commands=[];SerialLink.prototype.open=async()=>({name:'c3-ble-debugger',protocol:1,build:'TEST'});SerialLink.prototype.request=async(cmd,args)=>{commands.push(cmd);if(cmd==='status'||cmd==='connect')return {connected:true,address:'aa:bb:cc:dd:ee:ff',address_type:'public',secure:true};if(cmd==='bonds')return {bonds:[]};if(cmd==='discover')return {services:[{uuid:'1812',characteristics:[{uuid:'2a4d',handle:43,notify:true}]}]};return {status:0};};");
 await a.run('openSerial({})');
 assert.equal(a.run("commands.includes('disconnect')&&commands.includes('connect')&&commands.includes('discover')&&commands.includes('subscribe')"),true);
 assert.equal(a.run('needsFreshLink'),false);assert.equal(a.run('subscriptions.length'),1);
});
test('BLE preparation failure leaves serial usable and displays local error',async()=>{
 const a=app();
 a.run("identity={build:'TEST'};peer={connected:true,address:'aa:bb:cc:dd:ee:ff'};needsFreshLink=true;link={request:async(cmd)=>{if(cmd==='status')return peer;if(cmd==='connect')throw Error('wake remote');return {};}};");
 await a.run("action('准备采样',prepare)");
 assert.equal(a.run('!!identity'),true);assert.match(a.get('deviceStatus').textContent,/wake remote/);assert.equal(a.run('busy'),false);
});
