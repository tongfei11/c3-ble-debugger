'use strict';
const $ = id => document.getElementById(id), storeKey = 'c3-ble-debugger-run-v1';
const labels = {pending:'待测试',recording:'采样中',captured:'收到报文',empty:'未收到报文',invalid:'异常'};
let run, link = null, identity = null, peer = {}, busy = false, selected = 0, channels = [], subscriptions = [], channelResults = [], traceLimit = 20000;
try { run = new TestRun(JSON.parse(localStorage.getItem(storeKey)) || undefined); }
catch (_) { run = new TestRun(); }
for (const key of run.data.keys) if (key.status === 'recording') {
  key.status = 'invalid'; const a = key.attempts.at(-1); a.status = 'invalid'; a.reason = '页面关闭或刷新中断采样'; a.ended = new Date().toISOString();
}
function notice(text,error=false) { $('notice').textContent = text; $('notice').classList.toggle('error',error); $('deviceStatus').textContent=text; $('deviceStatus').classList.toggle('error',error); }
function save() {
  try { localStorage.setItem(storeKey,JSON.stringify(run.data)); $('saveState').textContent = '已保存到当前浏览器'; }
  catch (_) { $('saveState').textContent = '本地保存失败，请及时导出 JSON'; }
}
function trace(direction,message) {
  if (run.data.trace.length >= traceLimit) { run.data.trace.shift(); run.data.traceTruncated = true; }
  run.data.trace.push({time:new Date().toISOString(),direction,message});
}
let eventRenderTimer, needsFreshLink = false, lastPort = null;
let deviceNames = {};
try { deviceNames = JSON.parse(localStorage.getItem('c3-device-names-v1')) || {}; } catch (_) {}
function deviceName(d) { return deviceNames[d.address?.toLowerCase()] || d.name || d.address; }
function event(message) {
  if (message.event === 'notification') {
    run.notify(message);
    if (!eventRenderTimer) eventRenderTimer = setTimeout(() => { eventRenderTimer = null; save(); renderCapture(); renderKeys(); }, 100);
  }
  if (message.event === 'connected') peer = {...peer,connected:true,address:message.address};
  if (message.event === 'auth') peer.secure = message.ok;
  if (message.event === 'disconnected') { needsFreshLink = false; peer = {}; subscriptions = []; invalidate('蓝牙断开，当前采样已标记异常'); }
  if (message.event === 'dropped') invalidate('通知队列丢包，当前采样已标记异常，请重测');
  renderConnection(); controls();
}
function invalidate(reason) { run.stop('invalid',reason); save(); render(); notice(reason,true); }
function renderConnection() {
  $('connection').textContent = identity ? `C3 已连接 · 固件 ${identity.build}` : '串口未连接';
  $('peer').textContent = peer.connected ? `${deviceName(peer)} · ${peer.secure?'已加密':'未加密'} · ${subscriptions.length} 路通知` : '蓝牙未连接';
}
function controls() {
  const serial = !!identity, connected = !!peer.connected, active = run.active !== null, key = run.data.keys[selected];
  for (const id of ['scan','disconnect','prepare','applyChannels']) $(id).disabled = busy || !serial || active;
  $('scan').disabled ||= connected;
  for (const id of ['disconnect','prepare','applyChannels']) $(id).disabled ||= !connected;
  $('serialConnect').disabled = busy || !!link || !navigator.serial;
  $('serialReset').disabled = busy || active || !lastPort;
  $('disconnect').hidden = !connected;
  $('setup').disabled = busy || active;
  $('count').disabled = busy || active;
  $('keyName').disabled = busy || active || !key;
  $('start').disabled = busy || active || !key || !serial || !connected;
  $('start').textContent = key?.attempts.length ? '重新采样' : '开始采样';
  $('stop').disabled = !active;
  $('clearKey').disabled = busy || active || !key?.attempts.length;
  $('newRun').disabled = busy || active;
  document.querySelectorAll('[data-device]').forEach(b=>{
    b.disabled=busy||active||!serial;
    b.textContent=connected&&peer.address===b.dataset.device?(subscriptions.length?'重新准备采样':'准备采样'):(b.dataset.connectLabel||'连接设备');
  });
  document.querySelectorAll('#channels input').forEach(b=>b.disabled=busy||active);
  document.querySelectorAll('#keyList button').forEach(b=>b.disabled=busy||active);
}
function renderKeys() {
  const list = $('keyList'); list.replaceChildren();
  run.data.keys.forEach((key,index)=>{
    const button = document.createElement('button'); button.className = selected===index?'selected':'';
    const name = document.createElement('span'); name.textContent = `${String(index+1).padStart(2,'0')}  ${key.name}`;
    const status = document.createElement('small'); status.textContent = labels[key.status]; button.append(name,status);
    button.onclick=()=>{ selected=index; render(); }; list.append(button);
  });
  const completed = run.data.keys.filter(k=>['captured','empty','invalid'].includes(k.status)).length;
  $('progress').textContent = `${completed} / ${run.data.keys.length} 已采样`;
  controls();
}
function renderCapture() {
  const key = run.data.keys[selected], a = key?.attempts.at(-1), rows = a?.notifications || [];
  $('sampleCount').textContent = `${rows.length} 条报文${run.active !== null?' · 正在记录':''}`;
  $('samples').textContent = rows.length ? rows.slice(-100).map(n=>`${String(n.handle).padEnd(5)} ${n.hex.padEnd(44)} ${n.ms ?? ''} ms`).join('\n') : '暂无报文。点击开始测试后操作实体按键。';
  $('samples').scrollTop = $('samples').scrollHeight;
  $('instruction').textContent = run.active !== null ? `正在采集「${key.name}」：请操作按键、依次切换各档位或转动滚轮，完成后点击停止并保存。` : a?.reason || (key ? `当前：${key.name} · ${labels[key.status]}。停止后自动保存并切换下一键，可随时返回重采。` : '先生成按键测试清单。');
  if (document.activeElement !== $('keyName')) $('keyName').value = key?.name || '';
  controls();
}
function keyValues(key) { return [...new Set((key.attempts.at(-1)?.notifications || []).map(n=>n.hex))].join(' · '); }
function renderResults() {
  const body = $('resultRows'); body.replaceChildren();
  run.data.keys.forEach(key=>{
    const row = document.createElement('tr');
    const values = [key.name,keyValues(key) || '—'];
    values.forEach(v=>{const cell=document.createElement('td');cell.textContent=v;row.append(cell);});body.append(row);
  });
}
function render() { renderConnection(); renderKeys(); renderCapture(); renderResults(); }
async function action(title, task) {
  if (busy) return;
  busy = true; controls(); notice(`${title}…`);
  try { await task(); notice(`${title}完成`); }
  catch(e) { notice(e.message,true); }
  finally { busy = false; save(); render(); }
}
async function status() {
  peer = await link.request('status');
  if (peer.dropped > (run.data.lastDropped || 0)) { run.data.lastDropped = peer.dropped; if (run.active !== null) invalidate('设备报告丢包，请重测'); }
  return peer;
}
function deviceList(devices, bonded) {
  const target = $(bonded ? 'devices' : 'scanResults');
  target.replaceChildren();
  if (!devices.length) { target.textContent = bonded?'没有已配对设备，请扫描新设备。':'未扫描到设备，请确认目标已唤醒或进入配对模式。'; return; }
  for (const d of devices) {
    const row = document.createElement('div'); row.className='device-row';
    const info=document.createElement('div'), name=document.createElement('strong'), detail=document.createElement('small');
    name.textContent=deviceName(d);
    detail.textContent=`${d.address} · ${d.address_type||'地址类型待选'}${d.rssi!==undefined?` · ${d.rssi} dBm`:''}`;info.append(name,detail);
    const options=document.createElement('div');options.className='toolbar';
    let type;
    if (!d.address_type) {type=document.createElement('select');type.setAttribute('aria-label','设备地址类型');for(const t of ['public','random']){const o=document.createElement('option');o.value=t;o.textContent=t;type.append(o);}options.append(type);}
    const button=document.createElement('button');button.dataset.device=d.address;button.dataset.connectLabel=bonded?'连接设备':'连接并配对';button.textContent=button.dataset.connectLabel;
    button.onclick=()=>action('连接设备',async()=>{
      if(peer.connected && peer.address===d.address){await prepare();return;}
      if(peer.connected){await link.request('disconnect');peer={};}
      channels=[];subscriptions=[];channelResults=[];renderChannels();
      peer=await link.request('connect',{address:d.address,address_type:d.address_type||type.value,secure:true});
      needsFreshLink=false;
      run.data.device={...d,name:deviceName(d),address_type:d.address_type||type.value};run.data.firmware=identity;
      deviceList((await link.request('bonds')).bonds||[],true);
      await prepare();
    });
    if (bonded) {
      const rename=document.createElement('button');rename.textContent='重命名';rename.setAttribute('aria-label',`重命名 ${deviceName(d)}`);
      rename.onclick=()=>{
        const value=window.prompt('设备备注名称（留空恢复原名，仅保存在当前浏览器）',deviceNames[d.address.toLowerCase()]||'');
        if(value===null)return;
        const text=value.trim();if(text.length>80){notice('设备名称最多 80 个字符',true);return;}
        const next={...deviceNames};if(text)next[d.address.toLowerCase()]=text;else delete next[d.address.toLowerCase()];
        try{localStorage.setItem('c3-device-names-v1',JSON.stringify(next));deviceNames=next;deviceList(devices,true);renderConnection();notice('设备名称已保存，下次仍会显示。');}
        catch(_){notice('设备名称保存失败，请检查浏览器存储空间。',true);}
      };options.append(rename);
    }
    options.append(button);row.append(info,options);target.append(row);
  }controls();
}
function renderChannels() {
  $('channels').replaceChildren();
  for (const ch of channels) {
    const label=document.createElement('label'), input=document.createElement('input'); input.type='checkbox';input.value=ch.handle;input.checked=ch.selected;
    input.onchange=()=>{ch.selected=input.checked;};
    const result=channelResults.find(r=>r.handle===ch.handle);
    label.append(input,document.createTextNode(`句柄 ${ch.handle} · ${ch.uuid} · ${ch.mode}${result ? ` · ${result.ok?'订阅成功':result.error}`:''}`));$('channels').append(label);
  }
  $('channelSummary').textContent=`（已订阅 ${subscriptions.length} 路）`;
}
async function applyChannels() {
  for (const handle of [...subscriptions]) { await link.request('unsubscribe',{handle});subscriptions=subscriptions.filter(h=>h!==handle); }
  channelResults=[];
  for (const ch of channels.filter(c=>c.selected)) {
    try {await link.request('subscribe',{handle:ch.handle,mode:ch.mode}); subscriptions.push(ch.handle);channelResults.push({handle:ch.handle,ok:true});}
    catch(e) {channelResults.push({handle:ch.handle,ok:false,error:e.message});await status();if(!peer.connected)break;}
  }
  run.data.channels=channels; run.data.channelResults=channelResults;renderChannels();
  if (!subscriptions.length) throw Error('没有成功订阅的通知通道，请展开通道列表检查并重试');
  const failures=channelResults.filter(c=>!c.ok).length;
  if (failures) throw Error(`${subscriptions.length} 路订阅成功，${failures} 路失败。请展开列表检查；可使用已成功的通道测试。`);
}
async function prepare() {
  if(needsFreshLink) {
    const address=peer.address, addressType=peer.address_type||run.data.device?.address_type||'public';
    notice('正在恢复设备连接…');
    await link.request('disconnect');peer={};subscriptions=[];channels=[];channelResults=[];renderChannels();renderConnection();
    peer=await link.request('connect',{address,address_type:addressType,secure:true});
    needsFreshLink=false;
  }
  if(!peer.connected)throw Error('设备连接已断开，请唤醒设备后点击连接');
  notice('正在自动枚举服务…');
  const db=await link.request('discover');run.data.services=db.services;
  const all=(db.services||[]).flatMap(s=>(s.characteristics||[]).filter(c=>c.notify||c.indicate).map(c=>({...c,service:s.uuid,mode:c.notify?'notify':'indicate'})));
  const hid=all.filter(c=>c.service==='1812'&&c.notify), preferred=hid.length?hid:all.filter(c=>c.notify);
  channels=all.map(c=>({...c,selected:preferred.slice(0,4).some(p=>p.handle===c.handle)}));renderChannels();
  notice('正在自动订阅通知通道…');
  await applyChannels();await status();
  run.data.device={...(run.data.device?.address===peer.address?run.data.device:{}),address:peer.address,address_type:peer.address_type,name:deviceName(peer)};
  notice(`设备已就绪，已订阅 ${subscriptions.length} 路通知，可以开始采样。`);
}
async function openSerial(port, preserve = false) {
  lastPort = port;
  const previousPeer = peer, previousNeedsFreshLink = needsFreshLink;
  link=new SerialLink(event,e=>{identity=null;peer={};subscriptions=[];invalidate(`串口断开：${e.message}`);const old=link;link=null;old?.close();},trace);
  try {
    identity=await link.open(port);run.data.firmware=identity;await status();
    const sameLink=preserve && previousPeer.connected && peer.connected && previousPeer.address===peer.address;
    needsFreshLink=sameLink?previousNeedsFreshLink:!!peer.connected;
    if(!sameLink){subscriptions=[];channels=[];channelResults=[];renderChannels();}
    deviceList((await link.request('bonds')).bonds||[],true);
  } catch(e) {await link?.close();link=null;identity=null;peer={};subscriptions=[];throw e;}
  $('identity').textContent=`已验证 ${identity.name} · 固件 ${identity.build} · 协议 ${identity.protocol} · 115200 baud。`;
  if(peer.connected && (needsFreshLink || !subscriptions.length))await prepare();
}
$('serialConnect').onclick = async()=>{
  if (!navigator.serial || busy || link) return;
  let port; try {port=await navigator.serial.requestPort();} catch(e) {notice('未选择串口，可再次点击连接。');return;}
  action('连接 C3 串口',()=>openSerial(port));
};
$('serialReset').onclick=()=>action('重置串口',async()=>{
  const port=lastPort;if(!port)throw Error('请先选择 C3 串口');
  if(link)await link.close();link=null;identity=null;
  await openSerial(port,true);
});
$('scan').onclick=()=>action('扫描设备（5 秒）',async()=>{const r=await link.request('scan',{seconds:5});deviceList(r.devices||[],false);if(r.truncated)$('scanResults').prepend(document.createTextNode('扫描结果达到固件上限，部分设备未列出。'));});
$('disconnect').onclick=()=>action('断开蓝牙',async()=>{await link.request('disconnect');peer={};subscriptions=[];channels=[];renderChannels();});
$('prepare').onclick=()=>action('准备通知通道',prepare);
$('applyChannels').onclick=()=>action('应用通知通道',applyChannels);
$('setup').onclick=()=>{
  if(run.data.keys.length&&!window.confirm('重新生成清单会清除当前按键结果。建议先导出，是否继续？'))return;
  try{run.setup(Number($('count').value));selected=0;save();render();notice('清单已生成。选择按键，命名后开始测试。');}catch(e){notice(e.message,true);}
};
$('keyName').oninput=()=>{if(run.data.keys[selected]){run.data.keys[selected].name=$('keyName').value.slice(0,80);save();renderKeys();renderResults();}};
$('start').onclick=()=>action('开始按键测试',async()=>{
  await status();
  if(!peer.connected)throw Error('设备已断开，请唤醒后连接设备');
  if(needsFreshLink||!subscriptions.length)await prepare();
  if(!peer.connected||!subscriptions.length)throw Error('未能建立通知通道，请查看设备区的错误提示');
  const device=run.data.device?.address||peer.address;
  const previous=run.data.keys.flatMap(k=>k.attempts).find(a=>a.device&&a.device!==device);
  if(previous)throw Error('当前清单已有其他设备的采样，请先导出并新建测试');
  run.start(selected,$('keyName').value);const a=run.data.keys[selected].attempts.at(-1);a.device=device;a.channels=[...subscriptions];a.firmware=identity;
});
$('stop').onclick=()=>{
  const index=run.active;if(index===null)return;
  run.stop();const key=run.data.keys[index];
  if(index<run.data.keys.length-1)selected=index+1;
  save();render();notice(`「${key.name}」已保存：${labels[key.status]}。${index<run.data.keys.length-1?'下一键点击开始采样即可。':'本轮采样结束，可查看或导出结果。'}`);
};
$('clearKey').onclick=()=>{
  const key=run.data.keys[selected];if(!key?.attempts.length)return;
  if(!window.confirm(`清空「${key.name}」的全部采样数据？按键名称会保留。`))return;
  run.clear(selected);save();render();notice('本键采样数据已清空，名称保留。');
};
function download(content,extension,type) {
  const blob=new Blob([content],{type}),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=`c3-test-${new Date().toISOString().replace(/[:.]/g,'-')}.${extension}`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
$('exportJson').onclick=()=>download(JSON.stringify({...run.data,webBuild:'0007',exported:new Date().toISOString()},null,2),'json','application/json');
$('exportCsv').onclick=()=>{
  const rows=[['按键','键值'],...run.data.keys.map(k=>[k.name,keyValues(k)])];
  const csv=rows.map(r=>r.map(v=>'"'+String(v).replace(/^[=+@\-\t\r]/,"'$&").replace(/"/g,'""')+'"').join(',')).join('\r\n');download('\ufeff'+csv,'csv','text/csv;charset=utf-8');
};
$('newRun').onclick=()=>{if(!window.confirm('清空当前本地测试记录？请先导出需要保留的结果。'))return;run=new TestRun();selected=0;save();render();notice('已新建测试，请生成按键清单。');};
window.addEventListener('beforeunload',e=>{if(run.active!==null){e.preventDefault();e.returnValue='';}});
window.addEventListener('pagehide',()=>{run.stop('invalid','页面关闭或刷新中断采样');save();});
if(!navigator.serial || !window.isSecureContext) {notice('当前浏览器或页面环境不支持 Web Serial。请使用桌面 Chrome 打开本地页面，或在 HTTPS 网站打开。',true);$('serialConnect').disabled=true;}
else notice('浏览器支持串口直连。点击「选择 C3 串口」开始，无需运行 Python 程序。');
if(run.data.keys.length)$('count').value=run.data.keys.length;
render();if(!navigator.serial)$('serialConnect').disabled=true;
