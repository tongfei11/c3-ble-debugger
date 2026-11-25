const {test}=require('node:test');
const assert=require('node:assert/strict');
const {SerialLink,TestRun}=require('../desktop/web/serial.js');
test('only capture between start/stop; retain previous attempts',()=>{
 const r=new TestRun();r.setup(4);r.notify({hex:'outside'});r.start(0,'上');r.notify({hex:'4200'});r.notify({hex:'0000'});r.stop();r.notify({hex:'outside'});r.confirm(0);
 assert.equal(r.data.keys[0].status,'passed');assert.equal(r.data.keys[0].attempts[0].notifications.length,2);
 r.start(0,'上');r.notify({hex:'4200'});r.stop('invalid','disconnect');assert.throws(()=>r.confirm(0));assert.equal(r.data.keys[0].attempts.length,2);
 assert.equal(r.data.keys[0].attempts[0].status,'passed');
});
test('empty sampling cannot pass; limits and concurrent capture',()=>{
 const r=new TestRun();assert.throws(()=>r.setup(0));assert.throws(()=>r.setup(101));r.setup(2);r.start(0,'1');assert.throws(()=>r.start(1,'2'));assert.throws(()=>r.setup(3));r.stop();assert.throws(()=>r.confirm(0));r.skip(0);assert.equal(r.data.keys[0].status,'skipped');
});
test('fragments, events and malformed lines preserve response association',async()=>{
 const events=[];const link=new SerialLink(e=>events.push(e),()=>{});link.writer={write:async()=>{}};
 const request=link.request('hello');link.ingest('boot\n{"event":"notification","hex":"42');link.ingest('00"}\n{"id":1,"ok":true,"data":{"build":"0002"}}\n');assert.deepEqual(await request,{build:'0002'});assert.equal(events[0].hex,'4200');
});
test('timeout and serial failure reject requests; late response ignored',async()=>{
 const link=new SerialLink(()=>{},()=>{});link.writer={write:async()=>{}};
 await assert.rejects(link.request('read',{},5),/超时/);link.ingest('{"id":1,"ok":true}\n');
 const pending=link.request('status');link.fail(Error('unplugged'));await assert.rejects(pending,/unplugged/);
});
test('device rejection and concurrent commands are surfaced',async()=>{
 const link=new SerialLink(()=>{},()=>{});link.writer={write:async()=>{}};
 const p=link.request('subscribe');await assert.rejects(link.request('status'),/尚未完成/);link.ingest('{"id":1,"ok":false,"error":"no channel"}\n');await assert.rejects(p,/no channel/);
});
test('oversize malformed line recovers on next newline',()=>{
 let count=0;const link=new SerialLink(()=>count++,()=>{});link.ingest('x'.repeat(131074));link.ingest('\n{"event":"ready"}\n');assert.equal(count,1);
});
