/* JSON-line transport, shared by the browser and Node tests. */
class SerialLink {
  constructor(onEvent, onFailure, onTrace = () => {}) {
    this.onEvent = onEvent; this.onFailure = onFailure; this.onTrace = onTrace;
    this.pending = new Map(); this.nextId = 0; this.port = null; this.buffer = ''; this.discarding = false;
  }
  async open(port) {
    this.port = port;
    try {
      await port.open({baudRate:115200, bufferSize:65536, flowControl:'none'});
      await port.setSignals({dataTerminalReady:false, requestToSend:false});
      this.writer = port.writable.getWriter(); this.reader = port.readable.getReader();
      this.readTask = this.readLoop();
      const identity = await this.request('hello', {}, 8000);
      if (identity.name !== 'c3-ble-debugger' || identity.protocol !== 1) throw Error('该串口不是兼容的 C3 调试固件');
      return identity;
    } catch (e) { await this.close(); throw e; }
  }
  ingest(text) {
    for (const char of text) {
      if (char === '\n') {
        if (!this.discarding) {
          let message;
          try { message = JSON.parse(this.buffer); } catch (_) {}
          if (message && typeof message === 'object') {
            this.onTrace('rx', message);
            if (message.event) this.onEvent(message);
            else {
              const pending = this.pending.get(message.id);
              if (pending) { clearTimeout(pending.timer); this.pending.delete(message.id);
                if (message.ok) pending.resolve(message.data || {});
                else pending.reject(Error(message.error || '设备拒绝命令')); }
            }
          }
        }
        this.buffer = ''; this.discarding = false;
      } else if (!this.discarding) {
        if (this.buffer.length >= 131072) { this.buffer = ''; this.discarding = true; }
        else this.buffer += char;
      }
    }
  }
  async readLoop() {
    const decoder = new TextDecoder();
    try {
      while (true) { const {value,done} = await this.reader.read(); if (done) break; this.ingest(decoder.decode(value,{stream:true})); }
      if (!this.closing) throw Error('串口已断开');
    } catch (e) { if (!this.closing) { this.fail(e); this.onFailure(e); } }
    finally { this.reader.releaseLock(); this.reader = null; }
  }
  request(cmd, args = {}, timeout = 35000) {
    if (!this.writer || this.closing) return Promise.reject(Error('请先连接 C3 串口'));
    if (this.pending.size) return Promise.reject(Error('上一条设备命令尚未完成'));
    const id = ++this.nextId, message = {...args,id,cmd};
    const bytes = new TextEncoder().encode(JSON.stringify(message)+'\n');
    if (bytes.length > 768) return Promise.reject(Error('命令超出固件长度限制'));
    this.onTrace('tx', message);
    return new Promise((resolve,reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(Error(`${cmd} 超时；先检查状态再重试`)); }, timeout);
      this.pending.set(id,{resolve,reject,timer});
      this.writer.write(bytes).catch(e => { clearTimeout(timer); this.pending.delete(id); reject(e); });
    });
  }
  fail(error) { for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(error); } this.pending.clear(); }
  async close() {
    this.closing = true; this.fail(Error('串口会话已关闭'));
    if (this.reader) await this.reader.cancel().catch(()=>{});
    if (this.readTask) await this.readTask;
    if (this.writer) { this.writer.releaseLock(); this.writer = null; }
    if (this.port) await this.port.close().catch(()=>{});
    this.port = null;
  }
}
class TestRun {
  constructor(data) {
    this.data = data || {schema:2,created:new Date().toISOString(),keys:[],trace:[]}; this.active = null;
    // Normalize prior manual verdicts into observations without losing samples.
    for (const key of this.data.keys) {
      for (const attempt of key.attempts) {
        if (['passed','review'].includes(attempt.status)) attempt.status = attempt.notifications.length ? 'captured' : 'empty';
      }
      if (['passed','review','skipped'].includes(key.status)) key.status = key.attempts.at(-1)?.status || 'pending';
    }
    this.data.schema = 2;
  }
  setup(count) {
    if (!Number.isInteger(count) || count < 1 || count > 100) throw Error('按键数量须为 1–100');
    if (this.active !== null) throw Error('请先停止当前采样');
    this.data.keys = Array.from({length:count},(_,i)=>({name:`按键 ${i+1}`,status:'pending',attempts:[]}));
  }
  start(index, name) {
    if (this.active !== null) throw Error('已有按键正在采样');
    const key = this.data.keys[index]; if (!key) throw Error('请选择按键');
    key.name = name.trim() || `按键 ${index+1}`; key.status = 'recording';
    key.attempts.push({started:new Date().toISOString(),notifications:[],status:'recording'}); this.active = index;
  }
  notify(event) {
    if (this.active === null) return;
    const attempt = this.data.keys[this.active].attempts.at(-1);
    if (attempt.notifications.length >= 10000) { this.stop('invalid','单次采样达到 10000 条上限，请缩短采样'); return; }
    attempt.notifications.push({time:new Date().toISOString(),...event});
  }
  stop(status = 'auto', reason = '') {
    if (this.active === null) return;
    const key = this.data.keys[this.active], attempt = key.attempts.at(-1);
    if (status === 'auto') status = attempt.notifications.length ? 'captured' : 'empty';
    attempt.ended = new Date().toISOString(); attempt.status = status; attempt.reason = reason; key.status = status; this.active = null;
  }
  clear(index) {
    if (this.active !== null) throw Error('请先停止采样');
    const key = this.data.keys[index]; if (!key) throw Error('请选择按键');
    key.attempts = []; key.status = 'pending';
  }
}
if (typeof module !== 'undefined') module.exports = {SerialLink,TestRun};
