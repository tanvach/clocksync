// webui.h - the single-page web UI served at "/".
// Plain HTML/CSS/JS with no external resources so it also works on the
// offline setup hotspot. tools/mock_server.py serves it for local testing.
#pragma once

#ifndef PROGMEM
#define PROGMEM
#endif

static const char WEBUI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>clocksync</title>
<style>
:root{color-scheme:light dark;--bg:#f7f7f5;--fg:#111214;--mut:#8a8d93;--line:#e3e3df;--acc:#0b8a55;--warn:#b86e00;--bad:#d2383e}
@media (prefers-color-scheme:dark){:root{--bg:#0b0c0e;--fg:#ecedee;--mut:#71757c;--line:#1e2126;--acc:#4ff0a0;--warn:#ffb547;--bad:#ff6b6b}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif;-webkit-font-smoothing:antialiased}
main{max-width:920px;margin:0 auto;padding:22px 20px 60px}
header{display:flex;justify-content:space-between;align-items:center;gap:16px}
.brand{font-weight:600;letter-spacing:-.01em}
.brand span{color:var(--mut);font-weight:400;margin-left:6px;font-size:12px}
h2{font-size:11px;letter-spacing:.12em;text-transform:uppercase;font-weight:600}
.tag{display:flex;align-items:center;gap:8px;color:var(--mut);text-align:right;font-size:12px;font-weight:600;letter-spacing:.02em}
.dot{width:8px;height:8px;border-radius:50%;background:currentColor;flex:none;transition:opacity .08s}
.on{color:var(--acc)}.wait{color:var(--warn)}.bad{color:var(--bad)}
#clock{font:500 clamp(64px,17vw,132px)/1 ui-monospace,"SF Mono",Menlo,Consolas,monospace;letter-spacing:-.04em;font-variant-numeric:tabular-nums;margin:44px 0 10px}
#clock.dim{color:var(--line)}
#date{color:var(--mut);font-variant-numeric:tabular-nums}
.trace{margin:34px 0 0;position:relative}
.trace svg{display:block;width:100%;height:56px;overflow:visible}
.trace .all{stroke:var(--line);stroke-width:1.5;fill:none}
.trace .done{stroke:var(--acc);stroke-width:1.5;fill:none}
.trace line{stroke:var(--fg);stroke-width:1}
.axis{display:flex;justify-content:space-between;color:var(--mut);font:11px ui-monospace,Menlo,monospace;margin-top:6px}
#traceMsg{color:var(--mut);border-top:1px dashed var(--line);padding-top:12px;margin-top:34px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:0 56px;margin-top:40px}
@media (max-width:720px){.grid{grid-template-columns:1fr}}
section{min-width:0}
section.first{order:-1}
h2{color:var(--mut);margin:28px 0 4px}
.row{display:flex;justify-content:space-between;align-items:center;gap:14px;min-height:46px;border-bottom:1px solid var(--line)}
.row>:first-child{flex:none}
.val{color:var(--mut);text-align:right;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
select,input[type=text],input[type=password]{font:inherit;color:inherit;background:transparent;border:0;padding:8px 0;min-width:0;text-align:right;outline:none;flex:1}
select{cursor:pointer;text-align-last:right;max-width:72%}
input[type=text],input[type=password]{text-align:left}
input::placeholder{color:var(--mut)}
select option,select optgroup{background:var(--bg);color:var(--fg)}
.sw{appearance:none;-webkit-appearance:none;width:34px;height:20px;border-radius:20px;background:var(--line);position:relative;cursor:pointer;margin:0;flex:none}
.sw::after{content:"";position:absolute;top:3px;left:3px;width:14px;height:14px;border-radius:50%;background:var(--bg)}
.ready .sw,.ready .sw::after{transition:background .15s,transform .15s}
.sw:checked{background:var(--acc)}
.sw:checked::after{transform:translateX(14px)}
button{font:inherit;color:var(--fg);background:transparent;border:1px solid var(--line);border-radius:999px;padding:5px 13px;cursor:pointer;white-space:nowrap}
button:hover{border-color:var(--mut)}
button:disabled{opacity:.4}
button.link{border:0;padding:0;color:var(--acc);border-radius:0}
.note{color:var(--mut);font-size:12px;margin:8px 0 0}
.note:empty{display:none}
ul{list-style:none;margin:0;padding:0}
li{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid var(--line);cursor:pointer}
li:hover span:first-child{color:var(--acc)}
summary{cursor:pointer;list-style:none}
summary::-webkit-details-marker{display:none}
summary h2::after{content:" +"}
details[open] summary h2::after{content:" −"}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0 4px}
pre{white-space:pre-wrap;font:12px/1.5 ui-monospace,Menlo,monospace;color:var(--mut);margin:10px 0 0}
a{color:var(--acc);text-decoration:none}
#toast{position:fixed;left:50%;bottom:18px;transform:translate(-50%,8px);background:var(--fg);color:var(--bg);padding:7px 14px;border-radius:999px;font-size:13px;opacity:0;transition:opacity .2s,transform .2s;pointer-events:none}
#toast.show{opacity:1;transform:translate(-50%,0)}
.hidden{display:none!important}
</style>
</head>
<body>
<main>
<header>
 <div class="brand" id="host">clocksync</div>
 <div class="tag" id="state"><i class="dot" id="dot"></i><span id="stateText">connecting</span></div>
</header>

<div id="clock" class="dim">00:00:00</div>
<div id="date">&nbsp;</div>

<div class="trace hidden" id="trace">
 <svg viewBox="0 0 600 40" preserveAspectRatio="none">
  <defs><clipPath id="elapsed"><rect id="clipRect" x="0" y="-5" width="0" height="50"/></clipPath></defs>
  <path class="all" id="pathAll" vector-effect="non-scaling-stroke"/>
  <path class="done" id="pathDone" clip-path="url(#elapsed)" vector-effect="non-scaling-stroke"/>
  <line id="cursor" x1="0" x2="0" y1="-4" y2="44" vector-effect="non-scaling-stroke"/>
 </svg>
 <div class="axis"><span>:00</span><span>:10</span><span>:20</span><span>:30</span><span>:40</span><span>:50</span><span>:60</span></div>
</div>
<div id="traceMsg" class="hidden"></div>

<div class="grid">
 <section>
  <h2>Transmitter</h2>
  <div class="row"><label for="tx">Transmit</label><input type="checkbox" class="sw" id="tx"></div>
  <div class="row"><label for="station">Station</label><select id="station"></select></div>
 </section>

 <section>
  <h2>Time</h2>
  <div class="row"><label for="tz">Zone</label><select id="tz"><option value="">…</option></select></div>
  <div class="row hidden" id="customRow"><input type="text" id="posix" placeholder="POSIX rule" spellcheck="false" autocapitalize="off" autocomplete="off"><button id="posixSave">Save</button></div>
  <div class="row"><label for="ntp">Internet time</label><span class="val" id="ntpInfo"></span><input type="checkbox" class="sw" id="ntp"></div>
  <p class="note" id="tzNote"></p>
 </section>

 <section id="wifiSec">
  <h2>Wi-Fi</h2>
  <div class="row" id="wifiRow"><span>Network</span><span class="val" id="wifiInfo"></span><button class="link" id="wifiChange">Change</button></div>
  <div id="wifiForm" class="hidden">
   <div class="row"><input type="text" id="ssid" placeholder="Network name" autocapitalize="off" autocomplete="off" spellcheck="false"><button id="scan">Scan</button></div>
   <ul id="nets" class="hidden"></ul>
   <div class="row"><input type="password" id="pass" placeholder="Password" autocomplete="off"><button id="connect">Connect</button></div>
  </div>
  <p class="note" id="wifiMsg"></p>
 </section>

 <section>
  <details>
   <summary><h2>Advanced</h2></summary>
   <div class="row"><label for="ntpServer">NTP server</label><input type="text" id="ntpServer" autocapitalize="off" autocomplete="off" spellcheck="false" style="text-align:right"></div>
   <div class="row"><span>Clock</span><button class="link setTime">Set from this device</button></div>
   <div class="row"><label for="dst">DST flag</label><select id="dst"><option value="2">Automatic</option><option value="0">Always standard</option><option value="1">Always summer</option></select></div>
   <div class="row"><label for="pin">Antenna pin</label><select id="pin"></select></div>
   <div class="row"><label for="drive">Drive strength</label><select id="drive"><option value="0">0 · weakest</option><option value="1">1</option><option value="2">2</option><option value="3">3 · strongest</option></select></div>
   <div class="row"><label for="led">Status LED</label><input type="checkbox" class="sw" id="led"></div>
   <div class="row"><label for="hostname">Name</label><input type="text" id="hostname" autocapitalize="off" autocomplete="off" spellcheck="false" style="text-align:right"></div>
   <div class="row"><input type="text" id="cmd" placeholder="Command · h for help" autocapitalize="off" autocomplete="off" spellcheck="false"><button id="cmdSend">Run</button></div>
   <pre id="cmdOut" class="hidden"></pre>
   <div class="btns"><button id="reboot">Restart</button><button id="setupAp">Setup hotspot</button><button id="reset">Reset settings</button><button id="forget">Forget Wi-Fi</button></div>
   <p class="note" id="about"></p>
  </details>
 </section>
</div>
</main>
<div id="toast" role="status"></div>
<script>
(function () {
'use strict';
var $ = function (id) { return document.getElementById(id); };
var S = null, base = 0, baseAt = 0, zones = null, pollTimer = 0, lastTrace = '';
var wifiWatch = null, wifiOpen = false, customPending = false, built = {};

function pad(n) { return (n < 10 ? '0' : '') + n; }
function hms(d) { return pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes()) + ':' + pad(d.getUTCSeconds()); }
var DAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
function ymd(d) { return DAYS[d.getUTCDay()] + ' ' + d.getUTCFullYear() + '-' + pad(d.getUTCMonth() + 1) + '-' + pad(d.getUTCDate()); }
function fmtOffset(sec) {
  if (!sec) return 'UTC';
  var a = Math.abs(sec), m = Math.floor(a % 3600 / 60);
  return 'UTC' + (sec < 0 ? '−' : '+') + Math.floor(a / 3600) + (m ? ':' + pad(m) : '');
}
function zoneOffset(z) {
  try {
    var p = new Intl.DateTimeFormat('en-US', {timeZone: z, timeZoneName: 'shortOffset'}).formatToParts(new Date());
    for (var i = 0; i < p.length; i++) if (p[i].type === 'timeZoneName') return p[i].value.replace('GMT', 'UTC').replace('-', '−');
  } catch (e) {}
  return '';
}
function ago(s) { return s < 120 ? s + ' s' : s < 7200 ? Math.round(s / 60) + ' min' : Math.round(s / 3600) + ' h'; }
function nice(z) { return z.replace(/_/g, ' '); }
function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]; }); }
function show(id, on) { $(id).classList.toggle('hidden', !on); }
function setVal(id, v) { var el = $(id); if (document.activeElement !== el && el.value !== String(v)) el.value = v; }
function station(key) {
  for (var i = 0; S && i < S.stations.length; i++) if (S.stations[i].key === key) return S.stations[i];
  return null;
}

function toast(msg) {
  var t = $('toast');
  t.textContent = msg;
  t.className = 'show';
  clearTimeout(toast.t);
  toast.t = setTimeout(function () { t.className = ''; }, 2400);
}

function api(path, params) {
  var opt = {cache: 'no-store'};
  if (params) {
    opt.method = 'POST';
    opt.headers = {'Content-Type': 'application/x-www-form-urlencoded', 'X-Clocksync': '1'};
    opt.body = new URLSearchParams(params).toString();
  }
  return fetch(path, opt).then(function (r) {
    return r.json().catch(function () { return {}; }).then(function (j) {
      if (!r.ok || j.ok === false) throw new Error(j.error || ('HTTP ' + r.status));
      return j;
    });
  });
}

function save(params, msg) {
  return api('/api/set', params).then(function (j) { render(j); toast(msg || 'Saved'); })
    .catch(function (e) { toast(e.message); refresh(); });
}

// -- render -----------------------------------------------------------------

function render(j) {
  S = j;
  base = j.ms;
  baseAt = performance.now();
  var st = station(j.station), w = j.wifi;
  document.title = j.host;
  $('host').innerHTML = esc(j.host) + '<span>v' + esc(j.version) + '</span>';

  var cls, txt;
  if (w.ap && !w.connected) { cls = 'wait'; txt = 'Setup'; }
  else if (j.state === 'transmitting') { cls = 'on'; txt = st.name + ' · ' + st.freq / 1000 + ' kHz'; }
  else if (j.state === 'waiting') { cls = 'wait'; txt = 'Waiting for time'; }
  else { cls = ''; txt = 'Off'; }
  $('state').className = 'tag ' + cls;
  $('stateText').textContent = txt;

  renderTrace(j.state === 'transmitting' ? j.frame : '', st);
  var msg = '';
  if (j.state === 'waiting') {
    msg = 'No time yet. ' + (w.connected ? 'Waiting for ' + esc(j.ntpServer) + ', or ' : 'Connect Wi-Fi, or ') +
      '<button class="link setTime">set it from this device</button>.';
  } else if (j.state === 'off') {
    msg = 'Transmitter off.';
  }
  if ($('traceMsg').innerHTML !== msg) $('traceMsg').innerHTML = msg;
  show('traceMsg', !!msg);

  $('tx').checked = j.tx;
  if (!built.stations) {
    built.stations = true;
    $('station').innerHTML = j.stations.map(function (s) {
      return '<option value="' + s.key + '">' + esc(s.name + ' · ' + s.region) + '</option>';
    }).join('');
  }
  setVal('station', j.station);

  if (zones) {
    $('tzAuto').textContent = 'Auto · ' + nice(st.tz);
    if (j.tz && j.tz !== 'custom' && zones.indexOf(j.tz) < 0) {
      zones.push(j.tz);
      $('tz').insertAdjacentHTML('beforeend', '<option value="' + esc(j.tz) + '">' + esc(nice(j.tz)) + '</option>');
    }
    if (!customPending) setVal('tz', j.tz);
  }
  show('customRow', j.tz === 'custom' || customPending);
  if (j.tz === 'custom') setVal('posix', j.posix);
  $('tzNote').textContent = st.utc ? 'WWVB sends UTC; the zone only sets its DST flag.' : '';
  $('ntp').checked = j.ntp;
  $('ntpInfo').textContent = !j.ntp ? '' : j.lastSync >= 0 ? ago(j.lastSync) + ' ago' : 'not synced';

  renderWifi();

  setVal('ntpServer', j.ntpServer);
  setVal('dst', j.dstMode);
  if (built.pins !== j.pins.join()) {
    built.pins = j.pins.join();
    $('pin').innerHTML = j.pins.map(function (n) { return '<option value="' + n + '">GPIO ' + n + '</option>'; }).join('');
  }
  setVal('pin', j.pin);
  setVal('drive', j.drive);
  $('led').checked = j.led;
  setVal('hostname', j.hostSaved || j.host);
  $('about').innerHTML = esc(j.board) + ' · up ' + ago(j.uptime) + (j.ledcHz ? ' · carrier ' + j.ledcHz + ' Hz' : '') +
    ' · <a href="/status.txt">status.txt</a>';
  tick();
  if (!document.body.classList.contains('ready')) setTimeout(function () { document.body.classList.add('ready'); }, 50);
}

function renderWifi() {
  var w = S.wifi, info, form;
  if (w.connected) info = w.ssid + ' · ' + w.ip;
  else if (!w.ssid) info = 'Not set up';
  else if (w.connecting) info = 'Connecting to ' + w.ssid + '…';
  else info = w.ssid + ' · ' + (w.error || 'offline');
  $('wifiInfo').textContent = info;
  $('wifiInfo').className = 'val' + (!w.connected && w.error ? ' bad' : '');
  form = wifiOpen || !w.connected;
  show('wifiForm', form);
  show('wifiChange', !form);
  show('wifiRow', !!w.ssid);
  $('wifiSec').classList.toggle('first', w.ap && !w.connected);
  if (!$('ssid').value && w.ssid) $('ssid').value = w.ssid;

  var m = $('wifiMsg');
  if (wifiWatch) {
    if (w.connected && w.ssid === wifiWatch.ssid) {
      m.innerHTML = 'Connected. Open <a href="http://' + esc(S.host) + '.local/">' + esc(S.host) + '.local</a>' +
        (w.ap ? ' after rejoining ' + esc(w.ssid) + '.' : '.');
      wifiWatch = null;
      wifiOpen = false;
    } else if (!w.connecting && w.error && Date.now() - wifiWatch.at > 4000) {
      m.textContent = 'Could not connect: ' + w.error + '.';
      wifiWatch = null;
    }
  } else if (w.ap && !w.ssid && !m.textContent) {
    m.textContent = 'Join a network so the clock can fetch the exact time.';
  }
}

// The signal trace: the carrier envelope for this minute, one step per 100 ms.
function renderTrace(frame, st) {
  var key = frame + st.key;
  if (key === lastTrace) return;
  lastTrace = key;
  show('trace', !!frame);
  if (!frame) return;
  var d = '', prev = null;
  for (var i = 0; i < 600; i++) {
    var c = frame.charAt(Math.floor(i / 10)), shape = st.shapes[c === 'M' ? 4 : +c] || 'HHHHHHHHHH';
    var y = shape.charAt(i % 10) === 'H' ? 4 : 32;
    if (y !== prev) { d += (prev === null ? 'M0 ' + y : 'H' + i + 'V' + y); prev = y; }
  }
  d += 'H600';
  $('pathAll').setAttribute('d', d);
  $('pathDone').setAttribute('d', d);
}

function tick() {
  if (!S) return;
  if (!S.timeValid) {
    $('clock').textContent = '00:00:00';
    $('clock').className = 'dim';
    $('date').textContent = 'Clock not set';
    $('dot').style.opacity = 1;
    return;
  }
  var ms = base + (performance.now() - baseAt), local = new Date(ms + S.offset * 1000);
  $('clock').textContent = hms(local);
  $('clock').className = '';
  $('date').textContent = ymd(local) + ' · ' + (S.abbr && S.abbr !== fmtOffset(S.offset) ? S.abbr + ' ' : '') + fmtOffset(S.offset);

  var pos = (ms / 100) % 600;  // 100 ms steps into the minute
  $('clipRect').setAttribute('width', pos);
  $('cursor').setAttribute('x1', pos);
  $('cursor').setAttribute('x2', pos);
  // The status dot mirrors the carrier, like the LED on the device.
  var st = station(S.station), f = S.state === 'transmitting' ? S.frame : '';
  if (f) {
    var c = f.charAt(Math.floor(pos / 10)), shape = st.shapes[c === 'M' ? 4 : +c] || '';
    $('dot').style.opacity = shape.charAt(Math.floor(pos) % 10) === 'L' ? 0.2 : 1;
  } else {
    $('dot').style.opacity = 1;
  }
  // Fetch the new minute's frame right after it starts.
  var minute = Math.floor(ms / 60000);
  if (pos >= 10 && tick.minute !== minute) {
    if (tick.minute !== undefined) refresh();
    tick.minute = minute;
  }
}

function buildZones(text) {
  zones = text.split('\n').filter(Boolean).map(function (l) { return l.split('\t')[0]; });
  var groups = {}, html = '<option value="" id="tzAuto"></option>';
  zones.forEach(function (z) {
    var i = z.indexOf('/');
    if (i < 0) html += '<option value="' + z + '">' + z + '</option>';
    else (groups[z.slice(0, i)] = groups[z.slice(0, i)] || []).push(z);
  });
  Object.keys(groups).sort().forEach(function (g) {
    html += '<optgroup label="' + g + '">' + groups[g].map(function (z) {
      var off = zoneOffset(z);
      return '<option value="' + z + '">' + esc(nice(z.slice(g.length + 1)).replace(/\//g, ' / ') + (off ? '  ' + off : '')) + '</option>';
    }).join('') + '</optgroup>';
  });
  $('tz').innerHTML = html + '<option value="custom">Custom POSIX rule…</option>';
  if (S) render(S);
}

// -- polling ------------------------------------------------------------------

function refresh() {
  return api('/api/status').then(render).catch(function () {
    $('state').className = 'tag bad';
    $('stateText').textContent = 'Offline';
    if (wifiWatch && Date.now() - wifiWatch.at > 3000) {
      $('wifiMsg').textContent = 'Lost the setup hotspot, which usually means it connected. Rejoin ' + wifiWatch.ssid +
        ' and open ' + (S ? S.host : 'clocksync') + '.local';
    }
  });
}

function schedule() {
  clearTimeout(pollTimer);
  pollTimer = setTimeout(function () { (document.hidden ? Promise.resolve() : refresh()).then(schedule); }, wifiWatch ? 2000 : 5000);
}

// -- actions ------------------------------------------------------------------

$('tx').onchange = function () { save({tx: this.checked ? 1 : 0}, this.checked ? 'Transmitter on' : 'Transmitter off'); };
$('station').onchange = function () { save({station: this.value}); };
$('tz').onchange = function () {
  customPending = this.value === 'custom';
  if (customPending) { show('customRow', true); $('posix').focus(); return; }
  save({tz: this.value});
};
$('posixSave').onclick = function () {
  save({tz: 'custom', posix: $('posix').value.trim()}).then(function () { customPending = S.tz !== 'custom'; });
};
$('ntp').onchange = function () { save({ntp: this.checked ? 1 : 0}); };
$('ntpServer').onchange = function () { save({ntpServer: this.value.trim()}); };
$('dst').onchange = function () { save({dst: this.value}); };
$('pin').onchange = function () { save({pin: this.value}); };
$('drive').onchange = function () { save({drive: this.value}); };
$('led').onchange = function () { save({led: this.checked ? 1 : 0}); };
$('hostname').onchange = function () { save({host: this.value.trim().toLowerCase()}, 'Saved · applies after restart'); };
document.addEventListener('click', function (e) {
  if (!e.target.classList.contains('setTime')) return;
  api('/api/time', {ms: Date.now()}).then(function (j) { render(j); toast('Clock set'); }).catch(function (err) { toast(err.message); });
});

$('wifiChange').onclick = function () { wifiOpen = true; renderWifi(); $('ssid').focus(); };
$('scan').onclick = function () {
  var btn = this, tries = 0;
  btn.disabled = true;
  function poll(start) {
    api('/api/scan' + (start ? '?start=1' : '')).then(function (j) {
      if (j.scanning && ++tries < 20) { setTimeout(poll, 1000); return; }
      btn.disabled = false;
      var nets = (j.networks || []).sort(function (a, b) { return b.rssi - a.rssi; });
      $('nets').innerHTML = nets.length ? nets.map(function (n) {
        return '<li data-ssid="' + esc(n.ssid) + '"><span>' + esc(n.ssid) + '</span><span class="val">' +
          (n.open ? 'open · ' : '') + n.rssi + ' dBm</span></li>';
      }).join('') : '<li class="val">No networks found</li>';
      show('nets', true);
    }).catch(function (e) {
      // Scanning hops channels, so a phone on the setup hotspot can briefly lose it.
      if (++tries < 20) { setTimeout(poll, 1000); return; }
      btn.disabled = false;
      toast(e.message);
    });
  }
  poll(true);
};
$('nets').onclick = function (e) {
  var li = e.target.closest('li');
  if (!li || li.dataset.ssid === undefined) return;
  $('ssid').value = li.dataset.ssid;
  show('nets', false);
  $('pass').focus();
};
$('connect').onclick = function () {
  var ssid = $('ssid').value, m = $('wifiMsg');
  if (!ssid) { toast('Enter a network name'); return; }
  api('/api/wifi', {ssid: ssid, pass: $('pass').value}).then(function () {
    wifiWatch = {ssid: ssid, at: Date.now()};
    m.textContent = 'Connecting to ' + ssid + '…';
    schedule();
  }).catch(function (e) { m.textContent = e.message; });
};
$('pass').onkeydown = function (e) { if (e.key === 'Enter') $('connect').click(); };

$('cmdSend').onclick = function () {
  var c = $('cmd').value.trim();
  if (!c) return;
  fetch('/cmd?c=' + encodeURIComponent(c), {cache: 'no-store', headers: {'X-Clocksync': '1'}}).then(function (r) { return r.text(); }).then(function (t) {
    $('cmdOut').textContent = t;
    show('cmdOut', true);
    refresh();
  }).catch(function (e) { toast(e.message); });
};
$('cmd').onkeydown = function (e) { if (e.key === 'Enter') $('cmdSend').click(); };

function post(path, question, msg, after) {
  if (question && !confirm(question)) return;
  api(path, {}).then(function (j) { toast(msg); if (after) after(j); }).catch(function (e) { toast(e.message); });
}
$('reboot').onclick = function () { post('/api/reboot', 'Restart?', 'Restarting…'); };
$('setupAp').onclick = function () { post('/api/setup', null, 'Setup hotspot on', refresh); };
$('reset').onclick = function () { post('/api/reset', 'Reset station, zone and radio settings? Wi-Fi is kept.', 'Settings reset', render); };
$('forget').onclick = function () { post('/api/forget', 'Forget Wi-Fi and restart into setup?', 'Restarting…'); };

document.addEventListener('visibilitychange', function () { if (!document.hidden) refresh(); });
setInterval(tick, 100);
refresh().then(schedule);
(function loadZones() {
  // Retry: a request can drop while the hotspot is scanning.
  fetch('/api/zones').then(function (r) { if (!r.ok) throw r; return r.text(); }).then(buildZones)
    .catch(function () { setTimeout(loadZones, 3000); });
})();
})();
</script>
</body>
</html>
)HTML";
