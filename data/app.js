let ws = null;
let paused = false;
let uartMode = 0; 
let termLines = [];
let seqLines = [];
const MAX_TERM_LINES = 300; // Optimized for Mobile DOM
const MAX_SEQ_LINES = 100;
let reconnectTimer = null;

function initWs() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
  ws = new WebSocket('ws://' + location.hostname + '/ws'); // BUG FIXED: Port 80 /ws
  
  ws.onopen = () => {
    document.getElementById('ws-status').className = 'status';
    document.getElementById('ws-status').innerText = 'ONLINE';
    if(reconnectTimer) clearInterval(reconnectTimer);
  };
  
  ws.onclose = () => {
    document.getElementById('ws-status').className = 'status offline';
    document.getElementById('ws-status').innerText = 'OFFLINE';
    if(!reconnectTimer) reconnectTimer = setInterval(initWs, 2000);
  };
  
  ws.onmessage = (e) => {
    try {
      let d = JSON.parse(e.data);
      if (d.type === 'v') {
        document.getElementById('v-val').innerText = d.val.toFixed(2) + ' V';
        document.getElementById('v-avg').innerText = d.avg.toFixed(2);
      }
      else if (d.type === 'clk') {
        let hz = d.val, out = hz + " Hz";
        if (hz >= 1000000) out = (hz/1000000).toFixed(3) + " MHz";
        else if (hz >= 1000) out = (hz/1000).toFixed(2) + " kHz";
        document.getElementById('clk-val').innerText = out;
      }
      else if (d.type === 'uart' && !paused) {
        addTermLine(d.val);
      }
      else if (d.type === 'i2c') {
        document.getElementById('i2c-res').innerHTML = d.val;
      }
      else if (d.type === 'seq') {
        addSeqLine(d.time, d.pin, d.state);
        let el = document.getElementById('log-' + d.pin.toLowerCase());
        if (el) {
          el.className = d.state ? 'badge high' : 'badge low';
          el.innerText = d.state ? 'H' : 'L';
        }
      }
      else if (d.type === 'usb') {
        document.getElementById('usb-dp').className = d.dp ? 'badge high' : 'badge low';
        document.getElementById('usb-dp').innerText = d.dp ? 'H' : 'L';
        document.getElementById('usb-dn').className = d.dn ? 'badge high' : 'badge low';
        document.getElementById('usb-dn').innerText = d.dn ? 'H' : 'L';
      }
      else if (d.type === 'sys') {
        document.getElementById('sys-heap').innerText = (d.heap/1024).toFixed(1) + ' KB';
        document.getElementById('sys-psram').innerText = (d.psram/1024).toFixed(1) + ' KB';
        document.getElementById('sys-temp').innerText = d.temp.toFixed(1) + ' °C';
      }
    } catch(ex) {}
  };
}

function send(obj) { 
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj)); 
}

function nav(view, el) {
  document.querySelectorAll('.view').forEach(e => e.classList.add('hidden'));
  document.querySelectorAll('.nav-item').forEach(e => e.classList.remove('active'));
  document.getElementById('view-' + view).classList.remove('hidden');
  if (el) el.classList.add('active');
}

function setBaud() { send({cmd:'baud', val: parseInt(document.getElementById('baud').value)}); }
function switchUartMode() {
  uartMode = uartMode ? 0 : 1;
  send({cmd:'uart_mode', val:uartMode});
  document.getElementById('uart-mode-btn').innerText = uartMode ? 'ASCII Mode' : 'HEX Mode';
}
function togglePause() {
  paused = !paused;
  let btn = document.getElementById('pause-btn');
  btn.innerText = paused ? 'Resume' : 'Pause';
  btn.style.borderColor = paused ? 'var(--warning)' : '';
  btn.style.color = paused ? 'var(--warning)' : '';
}
function clearTerm() { termLines = []; document.getElementById('term').innerHTML = ''; }

function downloadLog() {
  let blob = new Blob([termLines.join('\n').replace(/<br>/g, '\n')], {type:'text/plain'});
  let a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `uart_log_${Date.now()}.txt`;
  a.click();
}

function addTermLine(line) {
  termLines.push(line);
  if (termLines.length > MAX_TERM_LINES) termLines.shift();
  let term = document.getElementById('term');
  // BUG FIXED: Use insertAdjacentHTML instead of innerHTML += to prevent browser freeze
  term.insertAdjacentHTML('beforeend', `<div>${line}</div>`); 
  term.scrollTop = term.scrollHeight;
  // Cleanup old DOM nodes
  if(term.childElementCount > MAX_TERM_LINES) term.removeChild(term.firstChild);
}

function addSeqLine(time, pin, state) {
  seqLines.push(`[${time}ms] ${pin}: ${state?'HIGH':'LOW'}`);
  if (seqLines.length > MAX_SEQ_LINES) seqLines.shift();
  let log = document.getElementById('seq-log');
  log.insertAdjacentHTML('beforeend', `<div>[${time}ms] ${pin}: ${state?'HIGH':'LOW'}</div>`);
  log.scrollTop = log.scrollHeight;
  if(log.childElementCount > MAX_SEQ_LINES) log.removeChild(log.firstChild);
}

function scanI2C() {
  document.getElementById('i2c-res').innerHTML = 'Scanning I2C Bus...<br>Please wait...';
  send({cmd:'i2c'});
}

let pwmOn = false;
function togglePWM() {
  if (!pwmOn) {
    if (!confirm('CRITICAL SAFETY:\nOutputting 3.3V to PWM pin.\nEnsure target logic level is compatible. Proceed?')) return;
    let f = parseInt(document.getElementById('pwm-f').value);
    let d = parseInt(document.getElementById('pwm-d').value);
    pwmOn = true;
    send({cmd:'pwm', en:1, f:f, d:d});
    let btn = document.getElementById('btn-pwm');
    btn.innerText = 'Disable PWM';
    btn.style.background = '#27272a';
  } else {
    pwmOn = false;
    send({cmd:'pwm', en:0});
    let btn = document.getElementById('btn-pwm');
    btn.innerText = 'Inject PWM Signal';
    btn.style.background = 'var(--danger)';
  }
}

function saveSettings() {
  let ssid = document.getElementById('set-ssid').value.trim();
  let pass = document.getElementById('set-pass').value.trim();
  if (ssid.length > 0) send({cmd:'settings_save', ssid:ssid, pass:pass});
}
function restartDevice() { if (confirm('Reboot tool?')) send({cmd:'restart'}); }
function factoryReset() { if (confirm('⚠️ Erase all settings?')) send({cmd:'factory'}); }

window.onload = initWs;
