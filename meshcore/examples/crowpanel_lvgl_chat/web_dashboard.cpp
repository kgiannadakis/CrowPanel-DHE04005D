// web_dashboard.cpp — Async web server with REST API and WebSocket

#include "app_globals.h"
#include "web_dashboard.h"
#include "persistence.h"
#include "mesh_api.h"
#include "utils.h"

#if defined(ESP32)
#include <WiFi.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <math.h>

/*
  The esp32_https_server headers can trip over a macro named `str`.
  Meshtastic applies the same undef before including the library.
*/
#undef str
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPSServer.hpp>
#include <HTTPServer.hpp>
#include <ResourceNode.hpp>
#include <SSLCert.hpp>
using namespace httpsserver;
#endif

extern bool g_wifi_connected;

// ── State ───────────────────────────────────────────────────

static bool s_running = false;
static bool s_initialized = false;
static char s_status[128] = "Disabled";

#if defined(ESP32)
static AsyncWebServer s_server(80);
static AsyncWebSocket s_ws("/ws");
static HTTPSServer* s_secure_server = nullptr;
static SSLCert* s_ssl_cert = nullptr;
static ResourceNode* s_secure_node_index = nullptr;
static ResourceNode* s_secure_node_position_page = nullptr;
static ResourceNode* s_secure_node_position_json = nullptr;
static ResourceNode* s_secure_node_position_set = nullptr;
static ResourceNode* s_secure_node_position_clear = nullptr;
static volatile bool s_ssl_cert_ready = false;
static volatile bool s_ssl_cert_task_running = false;
static bool s_secure_running = false;
static uint32_t s_last_https_attempt_ms = 0;
static const uint8_t MAX_HTTPS_CONNECTIONS = 2;
static const uint32_t HTTPS_RETRY_MS = 30000;
static const uint32_t MIN_HEAP_FOR_SSL = 40000;
#endif

// ── Embedded HTML ───────────────────────────────────────────

#if defined(ESP32)
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshCore Dashboard</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#0e1621;color:#f5f5f5;padding:12px;max-width:600px;margin:0 auto}
h1{font-size:1.3em;color:#5eb5f7;margin-bottom:12px}
h2{font-size:1em;color:#8696a0;margin:16px 0 8px;text-transform:uppercase;letter-spacing:1px}
.card{background:#17212b;border-radius:12px;padding:12px;margin-bottom:10px}
.stat{display:inline-block;margin-right:16px;font-size:0.85em;color:#8696a0}
.stat b{color:#f5f5f5}
select,input{background:#242f3d;border:1px solid #2b3b4d;border-radius:8px;color:#f5f5f5;padding:8px 10px;font-size:0.9em;width:100%}
select{cursor:pointer}
.send-row{display:flex;gap:8px;margin-top:8px}
.send-row input{flex:1;width:auto}
.send-row button{background:#3390ec;color:#fff;border:none;border-radius:8px;padding:8px 16px;cursor:pointer;font-size:0.9em;white-space:nowrap}
.send-row button:hover{background:#5eb5f7}
#chatbox{max-height:400px;overflow-y:auto;display:flex;flex-direction:column;gap:4px;padding:4px 0}
.bubble{padding:8px 12px;border-radius:12px;max-width:85%;font-size:0.88em;word-wrap:break-word;line-height:1.3}
.bubble.rx{background:#242f3d;align-self:flex-start;border-bottom-left-radius:4px}
.bubble.tx{background:#2b5278;align-self:flex-end;border-bottom-right-radius:4px}
.bubble .ts{font-size:0.72em;color:#8696a0;margin-top:2px}
.bubble .sts{font-size:0.72em;margin-left:6px}
.sts-D{color:#6dc264}.sts-N{color:#e74c3c}.sts-P{color:#f5a623}.sts-R{color:#6dc264}
.empty{color:#8696a0;font-size:0.85em;text-align:center;padding:20px}
.badge{display:inline-block;padding:1px 6px;border-radius:6px;font-size:0.75em;font-weight:600}
.badge-flood{background:#f5a623;color:#000}
.badge-direct{background:#6dc264;color:#000}
.badge-hop{background:#5eb5f7;color:#000}
.tab-bar{display:flex;gap:4px;margin-bottom:12px}
.tab{flex:1;text-align:center;padding:8px;border-radius:8px;cursor:pointer;font-size:0.9em;background:#17212b;color:#8696a0}
.tab.active{background:#3390ec;color:#fff}
.section{display:none}.section.active{display:block}
.btn-row{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px}
.btn-row button{background:#242f3d;border:1px solid #2b3b4d;border-radius:8px;color:#f5f5f5;padding:6px 12px;cursor:pointer;font-size:0.85em}
.btn-row button:hover{background:#3390ec}
.btn-row button.danger{border-color:#e74c3c}
.btn-row button.danger:hover{background:#e74c3c}
.btn-row button:disabled{opacity:0.4;cursor:not-allowed}
.hint{color:#9fb0bc;line-height:1.45;margin:0 0 10px}
.status{font-weight:600;color:#f5f5f5;margin-bottom:6px}
.coords{font-family:ui-monospace,Consolas,monospace;color:#d7e0e7;margin-bottom:10px}
.grid{display:grid;gap:12px}
@media(min-width:640px){.grid.two{grid-template-columns:1fr 1fr}}
.banner{display:none;border-radius:10px;padding:12px 14px;margin-top:12px;font-weight:600}
.banner.ok{display:block;background:rgba(51,195,107,0.14);color:#b7f5ca}
.banner.err{display:block;background:rgba(231,76,60,0.16);color:#ffd0cb}
#rptmon{white-space:pre-wrap;font-size:0.82em;color:#e0e0e0;max-height:350px;overflow-y:auto;padding:8px;background:#0e1621;border-radius:8px;margin-top:8px;font-family:monospace;line-height:1.4}
</style></head><body>
<h1>&#x1F4E1; MeshCore Dashboard</h1>
<div class="card" id="stats"></div>

<div class="tab-bar">
<div class="tab active" onclick="showTab('chat')">Chat</div>
<div class="tab" onclick="showTab('rpt')">Repeaters</div>
</div>

<div class="section active" id="sec-chat">
<h2>Conversation</h2>
<div class="card">
<select id="target" onchange="selChanged()"></select>
</div>
<div class="card" id="chatcard">
<div id="chatbox"><div class="empty">Select a contact or channel above</div></div>
<div class="send-row">
<input id="msg" placeholder="Type message..." maxlength="240" onkeydown="if(event.key==='Enter')send()">
<button onclick="send()">Send</button>
</div>
</div>

<h2 id="position">Device Position</h2>
<div class="card" id="poscard">
<div class="status" id="fixedState">Loading...</div>
<p class="coords" id="currentCoords">Checking current position...</p>
<p class="hint" id="geoNote">Tap the button below to try filling the fields from your phone location.</p>
<div class="grid two">
<div>
<label for="lat">Latitude</label>
<input id="lat" inputmode="decimal" placeholder="37.9838">
</div>
<div>
<label for="lon">Longitude</label>
<input id="lon" inputmode="decimal" placeholder="23.7275">
</div>
</div>
<div class="btn-row">
<button id="geoBtn" type="button">Use My Phone Location</button>
<button id="saveBtn" type="button">Save Position</button>
<button class="danger" id="clearBtn" type="button">Clear Position</button>
</div>
<div class="banner" id="banner"></div>
<p class="hint" id="secureHint" style="margin-top:12px;margin-bottom:0">Manual entry works here. Phone geolocation needs the secure dashboard page.</p>
</div>
</div>

<div class="section" id="sec-rpt">
<h2>Repeaters</h2>
<div class="card">
<select id="rptsel" onchange="rptChanged()"></select>
<div class="send-row" style="margin-top:8px">
<input id="rptpw" type="password" placeholder="Password" style="width:auto;flex:1">
<button onclick="rptLogin()">Login</button>
</div>
<div class="btn-row" id="rptbtns" style="display:none">
<button onclick="rptAction('status')">Status</button>
<button onclick="rptAction('neighbours')">Neighbours</button>
<button onclick="rptAction('advert')">Advert</button>
<button class="danger" onclick="rptAction('reboot')">Reboot</button>
</div>
<div style="margin-top:8px;display:none" id="rptdel"><button class="danger" onclick="rptDelete()">Delete Repeater</button></div>
<div id="rptmon">Select a repeater and enter password to login.</div>
</div>
</div>

<script>
let ws,curTarget='',contacts=[],channels=[];
function esc(s){let d=document.createElement('div');d.textContent=s;return d.innerHTML;}

function showTab(t){
  document.querySelectorAll('.tab').forEach((el,i)=>el.classList.toggle('active',i===(t==='chat'?0:1)));
  document.getElementById('sec-chat').classList.toggle('active',t==='chat');
  document.getElementById('sec-rpt').classList.toggle('active',t==='rpt');
  if(t==='rpt')loadRepeaters();
}

function conn(){
  if(location.protocol==='https:') return;
  ws=new WebSocket('ws://'+location.host+'/ws');
  ws.onmessage=e=>{
    let d=JSON.parse(e.data);
    if(d.key && d.key===curTarget){
      appendBubble(d.out,d.text,d.time||'',d.status||'');
    }
  };
  ws.onclose=()=>setTimeout(conn,3000);
}
conn();

function appendBubble(out,text,time,status){
  let box=document.getElementById('chatbox');
  if(box.querySelector('.empty'))box.innerHTML='';
  let div=document.createElement('div');
  div.className='bubble '+(out?'tx':'rx');
  let html=esc(text);
  if(time||status){
    html+='<div class="ts">'+esc(time);
    if(status){
      let sl=status.startsWith('R')?'R':status;
      let st=status==='D'?'Delivered':status==='N'?'Failed':status==='P'?'Pending':status.startsWith('R')?'Relayed':'';
      html+='<span class="sts sts-'+esc(sl)+'"> '+esc(st)+'</span>';
    }
    html+='</div>';
  }
  div.innerHTML=html;
  box.appendChild(div);
  box.scrollTop=box.scrollHeight;
}

async function loadStats(){
  try{
    let r=await fetch('/api/stats');let s=await r.json();
    document.getElementById('stats').innerHTML=
      '<span class="stat">Node: <b>'+esc(s.node)+'</b></span>'+
      '<span class="stat">Uptime: <b>'+Math.floor(s.uptime_s/60)+'m</b></span>'+
      '<span class="stat">Heap: <b>'+Math.floor(s.free_heap/1024)+'KB</b></span>'+
      '<span class="stat">FW: <b>v'+esc(s.version)+'</b></span>';
  }catch(e){}
}

async function loadTargets(){
  try{
    let r=await fetch('/api/contacts');contacts=await r.json();
    r=await fetch('/api/channels');channels=await r.json();
    let sel=document.getElementById('target');
    let prev=sel.value;
    sel.innerHTML='<option value="">-- Select --</option>';
    if(channels.length){
      let g=document.createElement('optgroup');g.label='Channels';
      channels.forEach(x=>{
        let o=document.createElement('option');
        o.value='h:'+x.idx;o.textContent='# '+x.name;g.appendChild(o);
      });
      sel.appendChild(g);
    }
    if(contacts.length){
      let g=document.createElement('optgroup');g.label='Contacts';
      contacts.forEach(x=>{
        let route=x.route==255?'flood':(x.route==0?'direct':x.route+'hop');
        let o=document.createElement('option');
        o.value='c:'+x.pubkey;o.textContent=x.name+' ('+route+', SNR:'+x.snr+')';g.appendChild(o);
      });
      sel.appendChild(g);
    }
    if(prev)sel.value=prev;
  }catch(e){}
}

async function selChanged(){
  let v=document.getElementById('target').value;
  curTarget=v;
  let box=document.getElementById('chatbox');
  if(!v){box.innerHTML='<div class="empty">Select a contact or channel above</div>';return;}
  box.innerHTML='<div class="empty">Loading...</div>';
  try{
    let r=await fetch('/api/messages?target='+encodeURIComponent(v));
    let msgs=await r.json();
    box.innerHTML='';
    if(!msgs.length){box.innerHTML='<div class="empty">No messages yet</div>';return;}
    msgs.forEach(m=>appendBubble(m.out,m.text,m.time,m.status));
  }catch(e){box.innerHTML='<div class="empty">Error loading messages</div>';}
}

async function send(){
  let t=document.getElementById('target').value;
  let m=document.getElementById('msg').value;
  if(!t||!m)return;
  document.getElementById('msg').value='';
  try{
    let r=await fetch('/api/send',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({target:t,text:m})});
    if(r.ok) appendBubble(true,m,'now','');
    else appendBubble(true,m,'now','N');
  }catch(e){appendBubble(true,m,'now','N');}
}

const fixedState=document.getElementById('fixedState');
const currentCoords=document.getElementById('currentCoords');
const banner=document.getElementById('banner');
const lat=document.getElementById('lat');
const lon=document.getElementById('lon');
const geoBtn=document.getElementById('geoBtn');
const saveBtn=document.getElementById('saveBtn');
const clearBtn=document.getElementById('clearBtn');
const geoNote=document.getElementById('geoNote');
const secureHint=document.getElementById('secureHint');

function showBanner(msg,ok){
  banner.textContent=msg;
  banner.className='banner '+(ok?'ok':'err');
}
function hideBanner(){
  banner.className='banner';
  banner.textContent='';
}
function fmt(n){
  return Number(n).toFixed(6).replace(/0+$/,'').replace(/\.$/,'');
}
async function loadPositionState(){
  const res=await fetch('/json/position',{cache:'no-store'});
  const json=await res.json();
  if(json.status!=='ok') throw new Error(json.message||'Failed to load position');
  const p=json.data.position;
  const hasCoords=!!p.fixed;
  fixedState.textContent='Fixed position: '+(hasCoords?'Enabled':'Disabled');
  currentCoords.textContent=hasCoords?('Lat '+fmt(p.latitude)+' | Lon '+fmt(p.longitude)):'No fixed position saved yet.';
  if(hasCoords){
    lat.value=fmt(p.latitude);
    lon.value=fmt(p.longitude);
  }else{
    lat.value='';
    lon.value='';
  }
}
async function callPositionApi(url,successMsg){
  const res=await fetch(url,{cache:'no-store'});
  const json=await res.json();
  if(json.status!=='ok') throw new Error(json.message||'Request failed');
  showBanner(successMsg,true);
  await loadPositionState();
}
saveBtn.addEventListener('click',async()=>{
  hideBanner();
  const params=new URLSearchParams({lat:lat.value.trim(),lon:lon.value.trim()});
  try{
    await callPositionApi('/json/position/set?'+params.toString(),'Fixed position saved.');
  }catch(err){
    showBanner(err.message,false);
  }
});
clearBtn.addEventListener('click',async()=>{
  hideBanner();
  try{
    await callPositionApi('/json/position/clear','Fixed position removed.');
  }catch(err){
    showBanner(err.message,false);
  }
});
geoBtn.addEventListener('click',()=>{
  hideBanner();
  if(!window.isSecureContext){
    window.location.href='https://'+location.hostname+'/#position';
    return;
  }
  if(!navigator.geolocation){
    showBanner('This browser does not support geolocation.',false);
    return;
  }
  geoBtn.disabled=true;
  geoBtn.textContent='Getting phone location...';
  navigator.geolocation.getCurrentPosition(
    (pos)=>{
      lat.value=String(pos.coords.latitude);
      lon.value=String(pos.coords.longitude);
      geoBtn.disabled=false;
      geoBtn.textContent='Use My Phone Location';
      showBanner('Phone coordinates loaded into the fields. Tap Save Position to store them.',true);
    },
    (err)=>{
      geoBtn.disabled=false;
      geoBtn.textContent='Use My Phone Location';
      showBanner(err.message||'Could not get phone location.',false);
    },
    {enableHighAccuracy:true,timeout:12000,maximumAge:0}
  );
});
if(!window.isSecureContext){
  geoNote.textContent='Manual entry works here. For phone location, open the secure dashboard page.';
  secureHint.innerHTML='For phone geolocation, open <b>https://'+location.hostname+'/</b> and accept the certificate warning once.';
  geoBtn.textContent='Open Secure Dashboard';
}else{
  secureHint.textContent='You are on the secure dashboard. Phone geolocation should be available if your browser grants permission.';
}

loadStats();loadTargets();
loadPositionState().catch(err=>showBanner(err.message,false));
setInterval(loadStats,15000);
setInterval(loadTargets,30000);

// ── Repeater functions ──
let curRpt='',rptLoggedIn=false;
async function loadRepeaters(){
  try{
    let r=await fetch('/api/repeaters');let rpts=await r.json();
    let sel=document.getElementById('rptsel');
    let prev=sel.value;
    sel.innerHTML='<option value="">-- Select Repeater --</option>';
    rpts.forEach(x=>{
      let o=document.createElement('option');
      o.value=x.pubkey;
      let hops=x.hops===0?'direct':(x.hops===255?'unknown':x.hops+'hop');
      o.textContent=x.name+' ('+hops+')';
      sel.appendChild(o);
    });
    if(prev)sel.value=prev;
  }catch(e){}
}
function rptChanged(){
  curRpt=document.getElementById('rptsel').value;
  rptLoggedIn=false;
  document.getElementById('rptbtns').style.display='none';
  document.getElementById('rptdel').style.display=curRpt?'block':'none';
  document.getElementById('rptmon').textContent=curRpt?'Enter password and click Login.':'Select a repeater.';
}
async function rptLogin(){
  if(!curRpt)return;
  let pw=document.getElementById('rptpw').value;
  document.getElementById('rptmon').textContent='Logging in...';
  try{
    let r=await fetch('/api/repeater/login',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({pubkey:curRpt,password:pw})});
    if(r.ok){
      document.getElementById('rptmon').textContent='Login sent, waiting for response...';
      rptLoggedIn=true;
      document.getElementById('rptbtns').style.display='flex';
      setTimeout(rptPollMon,3000);
    }else{
      document.getElementById('rptmon').textContent='Login failed to send.';
    }
  }catch(e){document.getElementById('rptmon').textContent='Error: '+e;}
}
async function rptAction(act){
  if(!curRpt||!rptLoggedIn)return;
  document.getElementById('rptmon').textContent='Sending '+act+' request...';
  try{
    await fetch('/api/repeater/'+act,{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({pubkey:curRpt})});
    setTimeout(rptPollMon,3000);
  }catch(e){}
}
async function rptPollMon(){
  try{
    let r=await fetch('/api/repeater/monitor');
    let d=await r.json();
    if(d.text)document.getElementById('rptmon').textContent=d.text;
    if(d.logged_in!==undefined){
      rptLoggedIn=d.logged_in;
      document.getElementById('rptbtns').style.display=d.logged_in?'flex':'none';
    }
  }catch(e){}
}
async function rptDelete(){
  if(!curRpt)return;
  let name=document.getElementById('rptsel').selectedOptions[0].textContent;
  if(!confirm('Permanently delete repeater "'+name+'"? It will only reappear when it advertises again.'))return;
  try{
    let r=await fetch('/api/repeater/delete',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({pubkey:curRpt})});
    if(r.ok){
      document.getElementById('rptmon').textContent='Repeater deleted.';
      curRpt='';rptLoggedIn=false;
      document.getElementById('rptbtns').style.display='none';
      document.getElementById('rptdel').style.display='none';
      loadRepeaters();
    }else{
      document.getElementById('rptmon').textContent='Delete failed: '+(await r.text());
    }
  }catch(e){document.getElementById('rptmon').textContent='Error: '+e;}
}
setInterval(()=>{if(document.getElementById('sec-rpt').classList.contains('active')&&curRpt)rptPollMon();},5000);
</script></body></html>
)rawliteral";

static const char POSITION_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshCore Position</title>
<style>
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;background:#0e1621;color:#f5f5f5;padding:16px;max-width:640px;margin:0 auto}
h1{font-size:1.45em;color:#5eb5f7;margin:0 0 10px}
p{color:#9fb0bc;line-height:1.45}
.card{background:#17212b;border-radius:14px;padding:14px;margin:14px 0}
.status{font-weight:600;color:#f5f5f5}
.coords{font-family:ui-monospace,Consolas,monospace;color:#d7e0e7}
label{display:block;font-size:0.92em;color:#9fb0bc;margin-bottom:6px}
input{width:100%;background:#242f3d;border:1px solid #2b3b4d;border-radius:10px;color:#f5f5f5;padding:11px 12px;font-size:1em}
.grid{display:grid;gap:12px}
@media(min-width:640px){.grid.two{grid-template-columns:1fr 1fr}}
.btns{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}
button{border:none;border-radius:10px;padding:11px 16px;font-size:0.95em;font-weight:700;cursor:pointer}
.primary{background:#33c36b;color:#072714}
.secondary{background:#3390ec;color:#fff}
.danger{background:#e74c3c;color:#fff}
.muted{background:#242f3d;color:#f5f5f5;border:1px solid #2b3b4d}
.banner{display:none;border-radius:10px;padding:12px 14px;margin-top:12px;font-weight:600}
.banner.ok{display:block;background:rgba(51,195,107,0.14);color:#b7f5ca}
.banner.err{display:block;background:rgba(231,76,60,0.16);color:#ffd0cb}
a{color:#8ec8ff}
</style></head><body>
<div class="card">
  <h1>Fixed Device Position</h1>
  <p>Save a fixed latitude and longitude for this MeshCore device.</p>
  <div class="status" id="fixedState">Loading...</div>
  <p class="coords" id="currentCoords">Checking current position...</p>
  <div class="banner" id="banner"></div>
</div>

<div class="card">
  <h1 style="font-size:1.08em">Phone Location</h1>
  <p id="geoNote">Tap the button below to try filling the fields from your phone location.</p>
  <div class="btns">
    <button class="secondary" id="geoBtn" type="button">Use My Phone Location</button>
  </div>
</div>

<div class="card">
  <h1 style="font-size:1.08em">Manual Entry</h1>
  <div class="grid two">
    <div>
      <label for="lat">Latitude</label>
      <input id="lat" inputmode="decimal" placeholder="37.9838">
    </div>
    <div>
      <label for="lon">Longitude</label>
      <input id="lon" inputmode="decimal" placeholder="23.7275">
    </div>
  </div>
  <div class="btns">
    <button class="primary" id="saveBtn" type="button">Save Position</button>
    <button class="danger" id="clearBtn" type="button">Clear Position</button>
    <button class="muted" id="backBtn" type="button">Back to Dashboard</button>
  </div>
</div>

<div class="card">
  <p id="secureHint">For phone geolocation, use the HTTPS version of this page. Manual entry still works over plain HTTP.</p>
</div>

<script>
const fixedState=document.getElementById('fixedState');
const currentCoords=document.getElementById('currentCoords');
const banner=document.getElementById('banner');
const lat=document.getElementById('lat');
const lon=document.getElementById('lon');
const geoBtn=document.getElementById('geoBtn');
const saveBtn=document.getElementById('saveBtn');
const clearBtn=document.getElementById('clearBtn');
const backBtn=document.getElementById('backBtn');
const geoNote=document.getElementById('geoNote');
const secureHint=document.getElementById('secureHint');

function showBanner(msg,ok){
  banner.textContent=msg;
  banner.className='banner '+(ok?'ok':'err');
}
function hideBanner(){
  banner.className='banner';
  banner.textContent='';
}
function fmt(n){
  return Number(n).toFixed(6).replace(/0+$/,'').replace(/\.$/,'');
}
async function loadState(){
  const res=await fetch('/json/position',{cache:'no-store'});
  const json=await res.json();
  if(json.status!=='ok') throw new Error(json.message||'Failed to load position');
  const p=json.data.position;
  const hasCoords=!!p.fixed;
  fixedState.textContent='Fixed position: '+(hasCoords?'Enabled':'Disabled');
  currentCoords.textContent=hasCoords?('Lat '+fmt(p.latitude)+' | Lon '+fmt(p.longitude)):'No fixed position saved yet.';
  if(hasCoords){
    lat.value=fmt(p.latitude);
    lon.value=fmt(p.longitude);
  } else {
    lat.value='';
    lon.value='';
  }
}
async function callApi(url,successMsg){
  const res=await fetch(url,{cache:'no-store'});
  const json=await res.json();
  if(json.status!=='ok') throw new Error(json.message||'Request failed');
  showBanner(successMsg,true);
  await loadState();
}
saveBtn.addEventListener('click',async()=>{
  hideBanner();
  const params=new URLSearchParams({lat:lat.value.trim(),lon:lon.value.trim()});
  try{
    await callApi('/json/position/set?'+params.toString(),'Fixed position saved.');
  }catch(err){
    showBanner(err.message,false);
  }
});
clearBtn.addEventListener('click',async()=>{
  hideBanner();
  try{
    await callApi('/json/position/clear','Fixed position removed.');
  }catch(err){
    showBanner(err.message,false);
  }
});
backBtn.addEventListener('click',()=>window.location.href='/');
geoBtn.addEventListener('click',()=>{
  hideBanner();
  if(!navigator.geolocation){
    showBanner('This browser does not support geolocation.',false);
    return;
  }
  geoBtn.disabled=true;
  geoBtn.textContent='Getting phone location...';
  navigator.geolocation.getCurrentPosition(
    (pos)=>{
      lat.value=String(pos.coords.latitude);
      lon.value=String(pos.coords.longitude);
      geoBtn.disabled=false;
      geoBtn.textContent='Use My Phone Location';
      showBanner('Phone coordinates loaded into the fields. Tap Save Position to store them.',true);
    },
    (err)=>{
      geoBtn.disabled=false;
      geoBtn.textContent='Use My Phone Location';
      showBanner(err.message||'Could not get phone location.',false);
    },
    {enableHighAccuracy:true,timeout:12000,maximumAge:0}
  );
});
if(!window.isSecureContext){
  geoNote.textContent='Your browser may block phone location on this HTTP page. Manual entry still works.';
  secureHint.innerHTML='For phone geolocation, open <b>https://'+location.hostname+'/position</b> and accept the certificate warning once.';
}else{
  secureHint.textContent='You are on the secure position page. Phone geolocation should be available if your browser grants permission.';
}
loadState().catch(err=>showBanner(err.message,false));
</script>
</body></html>
)rawliteral";
#endif

// ── REST API handlers ───────────────────────────────────────

#if defined(ESP32)

static String position_state_json() {
    double lat = 0.0;
    double lon = 0.0;
    mesh_get_fixed_position(&lat, &lon);

    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"data\":{\"position\":{\"latitude\":%.7f,\"longitude\":%.7f,\"fixed\":%s},\"advertise\":%s}}",
        lat, lon,
        (lat != 0.0 || lon != 0.0) ? "true" : "false",
        g_position_advert_enabled ? "true" : "false");
    return String(buf);
}

static String position_error_json(const char* message) {
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"status\":\"error\",\"message\":\"%s\"}", message ? message : "Request failed.");
    return String(buf);
}

static void update_webdash_status() {
    if (!g_webdash_enabled) {
        snprintf(s_status, sizeof(s_status), "Disabled");
    } else if (!g_wifi_connected) {
        snprintf(s_status, sizeof(s_status), "WiFi required");
    } else {
        String ip = WiFi.localIP().toString();
        if (s_running && s_secure_running) {
            snprintf(s_status, sizeof(s_status), "http://%s | https://%s", ip.c_str(), ip.c_str());
        } else if (s_running && s_ssl_cert_task_running) {
            snprintf(s_status, sizeof(s_status), "http://%s | HTTPS starting...", ip.c_str());
        } else if (s_running) {
            snprintf(s_status, sizeof(s_status), "http://%s", ip.c_str());
        } else if (s_ssl_cert_task_running) {
            snprintf(s_status, sizeof(s_status), "Generating HTTPS cert...");
        } else {
            snprintf(s_status, sizeof(s_status), "Starting...");
        }
    }
    g_deferred_features_dirty = true;
}

static bool parse_bounded_double(const String& text, double min_value, double max_value, double& out) {
    char* end = nullptr;
    out = strtod(text.c_str(), &end);
    while (end && *end == ' ') end++;
    return end && end != text.c_str() && *end == '\0' && isfinite(out) && out >= min_value && out <= max_value;
}

static void handle_position_page(AsyncWebServerRequest* req) {
    if (s_secure_running) {
        const String url = String("https://") + WiFi.localIP().toString() + "/#position";
        req->redirect(url);
        return;
    }
    req->redirect("/#position");
}

static void handle_position_json(AsyncWebServerRequest* req) {
    req->send(200, "application/json", position_state_json());
}

static void handle_position_set(AsyncWebServerRequest* req) {
    if (!req->hasParam("lat") || !req->hasParam("lon")) {
        req->send(400, "application/json", position_error_json("Latitude and longitude are required."));
        return;
    }

    double lat = 0.0;
    double lon = 0.0;
    if (!parse_bounded_double(req->getParam("lat")->value(), -90.0, 90.0, lat)) {
        req->send(400, "application/json", position_error_json("Latitude must be between -90 and 90."));
        return;
    }
    if (!parse_bounded_double(req->getParam("lon")->value(), -180.0, 180.0, lon)) {
        req->send(400, "application/json", position_error_json("Longitude must be between -180 and 180."));
        return;
    }

    mesh_set_fixed_position(lat, lon);
    handle_position_json(req);
}

static void handle_position_clear(AsyncWebServerRequest* req) {
    mesh_clear_fixed_position();
    handle_position_json(req);
}

static void secure_write_json(HTTPResponse* res, uint16_t statusCode, const String& body) {
    if (!res) return;
    res->setStatusCode(statusCode);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Cache-Control", "no-store");
    res->print(body.c_str());
}

static void secure_redirect(HTTPResponse* res, const char* location) {
    if (!res) return;
    res->setStatusCode(302);
    res->setHeader("Location", location ? location : "/");
    res->setHeader("Cache-Control", "no-store");
    res->println("");
}

static void secure_handle_index_page(HTTPRequest* req, HTTPResponse* res) {
    if (!res) return;
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Cache-Control", "no-store");
    res->print(INDEX_HTML);
}

static void secure_handle_position_page(HTTPRequest* req, HTTPResponse* res) {
    secure_redirect(res, "/#position");
}

static void secure_handle_position_json(HTTPRequest* req, HTTPResponse* res) {
    secure_write_json(res, 200, position_state_json());
}

static bool secure_get_query_parameter(HTTPRequest* req, const char* name, String& value) {
    if (!req || !name) return false;
    ResourceParameters* params = req->getParams();
    if (!params) return false;
    std::string text;
    if (!params->getQueryParameter(name, text)) return false;
    value = String(text.c_str());
    return true;
}

static void secure_handle_position_set(HTTPRequest* req, HTTPResponse* res) {
    String latText;
    String lonText;
    if (!secure_get_query_parameter(req, "lat", latText) || !secure_get_query_parameter(req, "lon", lonText)) {
        secure_write_json(res, 400, position_error_json("Latitude and longitude are required."));
        return;
    }

    double lat = 0.0;
    double lon = 0.0;
    if (!parse_bounded_double(latText, -90.0, 90.0, lat)) {
        secure_write_json(res, 400, position_error_json("Latitude must be between -90 and 90."));
        return;
    }
    if (!parse_bounded_double(lonText, -180.0, 180.0, lon)) {
        secure_write_json(res, 400, position_error_json("Longitude must be between -180 and 180."));
        return;
    }

    mesh_set_fixed_position(lat, lon);
    secure_write_json(res, 200, position_state_json());
}

static void secure_handle_position_clear(HTTPRequest* req, HTTPResponse* res) {
    mesh_clear_fixed_position();
    secure_write_json(res, 200, position_state_json());
}

static void handle_stats(AsyncWebServerRequest* req) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"node\":\"%s\",\"uptime_s\":%lu,\"free_heap\":%lu,\"version\":\"%s\","
        "\"contacts\":%d,\"channels\":%d}",
        mesh_get_node_name(),
        (unsigned long)(millis() / 1000),
        (unsigned long)ESP.getFreeHeap(),
        FIRMWARE_VERSION,
        dd_contacts_count,
        dd_channels_count);
    req->send(200, "application/json", buf);
}

static void handle_contacts(AsyncWebServerRequest* req) {
    String json = "[";
    for (int i = 0; i < dd_contacts_count; i++) {
        if (i > 0) json += ",";
        int8_t snr = 0;
        for (int s = 0; s < MAX_UNREAD_SLOTS; s++) {
            if (g_contact_snr[s].valid &&
                memcmp(g_contact_snr[s].pub_key, dd_contacts[i].contact_id.pub_key, 32) == 0) {
                snr = g_contact_snr[s].last_snr;
                break;
            }
        }
        char pubhex[9];
        snprintf(pubhex, sizeof(pubhex), "%02x%02x%02x%02x",
                 dd_contacts[i].contact_id.pub_key[0], dd_contacts[i].contact_id.pub_key[1],
                 dd_contacts[i].contact_id.pub_key[2], dd_contacts[i].contact_id.pub_key[3]);

        char entry[160];
        snprintf(entry, sizeof(entry),
            "{\"name\":\"%.30s\",\"pubkey\":\"%.8s\",\"route\":255,\"snr\":%d}",
            dd_contacts[i].name, pubhex, (int)snr);
        json += entry;
    }
    json += "]";
    req->send(200, "application/json", json);
}

static void handle_channels(AsyncWebServerRequest* req) {
    String json = "[";
    for (int i = 0; i < dd_channels_count; i++) {
        if (i > 0) json += ",";
        char entry[80];
        snprintf(entry, sizeof(entry), "{\"name\":\"%.30s\",\"idx\":%d}",
                 dd_channels[i].name, dd_channels[i].channel_idx);
        json += entry;
    }
    json += "]";
    req->send(200, "application/json", json);
}

// Build chat file key from target string "c:abcd1234" or "h:0"
static String target_to_key(const String& target) {
    if (target.startsWith("c:")) {
        String hex = target.substring(2);
        for (int i = 0; i < dd_contacts_count; i++) {
            char pubhex[9];
            snprintf(pubhex, sizeof(pubhex), "%02x%02x%02x%02x",
                     dd_contacts[i].contact_id.pub_key[0], dd_contacts[i].contact_id.pub_key[1],
                     dd_contacts[i].contact_id.pub_key[2], dd_contacts[i].contact_id.pub_key[3]);
            if (hex.equalsIgnoreCase(pubhex)) {
                return key_for_contact(dd_contacts[i].contact_id);
            }
        }
    } else if (target.startsWith("h:")) {
        int idx = target.substring(2).toInt();
        return key_for_channel(idx);
    }
    return "";
}

static void handle_messages(AsyncWebServerRequest* req) {
    if (!req->hasParam("target")) {
        req->send(400, "text/plain", "Missing target param");
        return;
    }
    String target = req->getParam("target")->value();
    String key = target_to_key(target);
    if (key.length() == 0) {
        req->send(200, "application/json", "[]");
        return;
    }

    String path = chat_path_for(key);
    if (!SPIFFS.exists(path)) {
        req->send(200, "application/json", "[]");
        return;
    }

    File f = SPIFFS.open(path, FILE_READ);
    if (!f) {
        req->send(200, "application/json", "[]");
        return;
    }

    // Count lines then read last 50
    int total = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.startsWith("TX|") || line.startsWith("RX|")) total++;
    }
    f.seek(0);

    int skip = (total > 50) ? total - 50 : 0;
    int idx = 0;

    String json = "[";
    bool first = true;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;
        bool out = line.startsWith("TX|");
        if (!out && !line.startsWith("RX|")) continue;
        if (idx++ < skip) continue;

        // Parse: TX|timestamp|status|[time] text  or  RX|timestamp||[time] text
        int p1 = line.indexOf('|');
        int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
        int p3 = (p2 >= 0) ? line.indexOf('|', p2 + 1) : -1;
        if (p3 < 0) continue;

        String status_str = line.substring(p2 + 1, p3);
        String msg = line.substring(p3 + 1);

        // Extract time from "[HH:MM] ..." and the rest as text
        String timeStr = "";
        String textStr = msg;
        if (msg.startsWith("[")) {
            int cb = msg.indexOf(']');
            if (cb > 0) {
                timeStr = msg.substring(1, cb);
                textStr = msg.substring(cb + 2);  // skip "] "
            }
        }

        if (!first) json += ",";
        first = false;

        // JSON-escape the text
        String escaped = "";
        for (unsigned int c = 0; c < textStr.length() && c < 260; c++) {
            char ch = textStr.charAt(c);
            if (ch == '"') escaped += "\\\"";
            else if (ch == '\\') escaped += "\\\\";
            else if (ch == '\n') escaped += "\\n";
            else escaped += ch;
        }

        json += "{\"out\":";
        json += out ? "true" : "false";
        json += ",\"text\":\"";
        json += escaped;
        json += "\",\"time\":\"";
        json += timeStr;
        json += "\",\"status\":\"";
        json += status_str;
        json += "\"}";
    }
    f.close();
    json += "]";
    req->send(200, "application/json", json);
}

static void handle_send(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
    String body = String((char*)data, len);

    int t_pos = body.indexOf("\"target\"");
    int t_start = body.indexOf('"', t_pos + 8);
    int t_end = body.indexOf('"', t_start + 1);

    int x_pos = body.indexOf("\"text\"");
    int x_start = body.indexOf('"', x_pos + 6);
    int x_end = body.indexOf('"', x_start + 1);

    if (t_start < 0 || t_end < 0 || x_start < 0 || x_end < 0) {
        req->send(400, "text/plain", "Bad request");
        return;
    }

    String target = body.substring(t_start + 1, t_end);
    String text = body.substring(x_start + 1, x_end);

    if (text.length() == 0 || text.length() > 240) {
        req->send(400, "text/plain", "Invalid text");
        return;
    }

    bool ok = false;
    if (target.startsWith("c:")) {
        String hex = target.substring(2);
        for (int i = 0; i < dd_contacts_count; i++) {
            char pubhex[9];
            snprintf(pubhex, sizeof(pubhex), "%02x%02x%02x%02x",
                     dd_contacts[i].contact_id.pub_key[0], dd_contacts[i].contact_id.pub_key[1],
                     dd_contacts[i].contact_id.pub_key[2], dd_contacts[i].contact_id.pub_key[3]);
            if (hex.equalsIgnoreCase(pubhex)) {
                ok = mesh_send_text_to_contact(dd_contacts[i].contact_id.pub_key, text.c_str());
                break;
            }
        }
    } else if (target.startsWith("h:")) {
        int idx = target.substring(2).toInt();
        ok = mesh_send_text_to_channel(idx, text.c_str());
    }

    req->send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Send failed");
}

// ── Repeater API handlers ──────────────────────────────────

// Helper: find repeater ContactInfo by hex pubkey prefix (first 4 bytes = 8 hex chars)
static ContactInfo* find_repeater_by_hex(const String& hex) {
    for (int i = 0; i < g_repeater_count; i++) {
        if (!g_repeater_list[i]) continue;
        char pubhex[9];
        snprintf(pubhex, sizeof(pubhex), "%02x%02x%02x%02x",
                 g_repeater_list[i]->id.pub_key[0], g_repeater_list[i]->id.pub_key[1],
                 g_repeater_list[i]->id.pub_key[2], g_repeater_list[i]->id.pub_key[3]);
        if (hex.equalsIgnoreCase(pubhex)) return g_repeater_list[i];
    }
    return nullptr;
}

// Extract "pubkey" from JSON body
static String extract_pubkey(const uint8_t* data, size_t len) {
    String body = String((char*)data, len);
    int p = body.indexOf("\"pubkey\"");
    if (p < 0) return "";
    int s = body.indexOf('"', p + 8);
    int e = body.indexOf('"', s + 1);
    if (s < 0 || e < 0) return "";
    return body.substring(s + 1, e);
}

static void handle_repeaters(AsyncWebServerRequest* req) {
    // Refresh the repeater list from mesh contacts
    mesh_populate_repeater_list();
    String json = "[";
    int count = 0;
    for (int i = 0; i < g_repeater_count; i++) {
        if (!g_repeater_list[i]) continue;
        if (count > 0) json += ",";
        char pubhex[9];
        snprintf(pubhex, sizeof(pubhex), "%02x%02x%02x%02x",
                 g_repeater_list[i]->id.pub_key[0], g_repeater_list[i]->id.pub_key[1],
                 g_repeater_list[i]->id.pub_key[2], g_repeater_list[i]->id.pub_key[3]);
        char entry[128];
        snprintf(entry, sizeof(entry),
            "{\"name\":\"%.30s\",\"pubkey\":\"%.8s\",\"hops\":%d}",
            g_repeater_list[i]->name, pubhex,
            g_repeater_list[i]->out_path_len == 0xFF ? 255 : (int)(g_repeater_list[i]->out_path_len & 63));
        json += entry;
        count++;
    }
    json += "]";
    req->send(200, "application/json", json);
}

static void handle_rpt_login(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
    String body = String((char*)data, len);
    String hex = extract_pubkey(data, len);
    // Extract password
    int pp = body.indexOf("\"password\"");
    int ps = (pp >= 0) ? body.indexOf('"', pp + 10) : -1;
    int pe = (ps >= 0) ? body.indexOf('"', ps + 1) : -1;
    String pw = (ps >= 0 && pe > ps) ? body.substring(ps + 1, pe) : "";

    ContactInfo* rpt = find_repeater_by_hex(hex);
    if (!rpt) { req->send(404, "text/plain", "Repeater not found"); return; }

    int result = mesh_repeater_login(rpt->id.pub_key, pw.c_str());
    req->send(result == 0 ? 200 : 500, "text/plain", result == 0 ? "OK" : "Failed");
}

static void handle_rpt_action(const char* action, AsyncWebServerRequest* req, uint8_t* data, size_t len) {
    String hex = extract_pubkey(data, len);
    ContactInfo* rpt = find_repeater_by_hex(hex);
    if (!rpt) { req->send(404, "text/plain", "Repeater not found"); return; }

    int result = -1;
    if (strcmp(action, "status") == 0)      result = mesh_repeater_request_status(rpt->id.pub_key);
    else if (strcmp(action, "neighbours") == 0) result = mesh_repeater_request_neighbours(rpt->id.pub_key);
    else if (strcmp(action, "advert") == 0)  result = mesh_repeater_send_advert(rpt->id.pub_key);
    else if (strcmp(action, "reboot") == 0)  result = mesh_repeater_send_reboot(rpt->id.pub_key);

    req->send(result == 0 ? 200 : 500, "text/plain", result == 0 ? "OK" : "Failed");
}

static void handle_rpt_monitor(AsyncWebServerRequest* req) {
    // Return the current repeater monitor text and login state
    // JSON-escape the monitor text
    String escaped = "";
    for (int i = 0; g_deferred_repeater_mon[i] && i < 600; i++) {
        char c = g_deferred_repeater_mon[i];
        if (c == '"') escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') continue;
        else escaped += c;
    }

    char buf[700];
    snprintf(buf, sizeof(buf),
        "{\"text\":\"%.600s\",\"logged_in\":%s,\"name\":\"%.30s\"}",
        escaped.c_str(),
        g_repeater_logged_in ? "true" : "false",
        g_selected_repeater ? g_selected_repeater->name : "");
    req->send(200, "application/json", buf);
}

static void handle_rpt_delete(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
    String hex = extract_pubkey(data, len);
    ContactInfo* rpt = find_repeater_by_hex(hex);
    if (!rpt) { req->send(404, "text/plain", "Repeater not found"); return; }

    bool ok = mesh_delete_repeater_by_pubkey(rpt->id.pub_key);
    req->send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Failed");
}

static void ensure_secure_nodes() {
    if (!s_secure_node_index)          s_secure_node_index          = new ResourceNode("/", "GET", &secure_handle_index_page);
    if (!s_secure_node_position_page)  s_secure_node_position_page  = new ResourceNode("/position", "GET", &secure_handle_position_page);
    if (!s_secure_node_position_json)  s_secure_node_position_json  = new ResourceNode("/json/position", "GET", &secure_handle_position_json);
    if (!s_secure_node_position_set)   s_secure_node_position_set   = new ResourceNode("/json/position/set", "GET", &secure_handle_position_set);
    if (!s_secure_node_position_clear) s_secure_node_position_clear = new ResourceNode("/json/position/clear", "GET", &secure_handle_position_clear);
}

static void stop_secure_server() {
    if (s_secure_server) {
        delete s_secure_server;
        s_secure_server = nullptr;
    }
    if (s_secure_running) {
        s_secure_running = false;
        serialmon_append("HTTPS dashboard stopped");
    }
    update_webdash_status();
}

static void start_secure_server() {
    if (s_secure_running || !g_wifi_connected || !s_ssl_cert_ready || !s_ssl_cert) return;
    if (ESP.getFreeHeap() < MIN_HEAP_FOR_SSL) {
        serialmon_append("HTTPS dashboard skipped: low heap");
        return;
    }

    ensure_secure_nodes();
    s_secure_server = new HTTPSServer(s_ssl_cert, 443, MAX_HTTPS_CONNECTIONS);
    s_secure_server->registerNode(s_secure_node_index);
    s_secure_server->registerNode(s_secure_node_position_page);
    s_secure_server->registerNode(s_secure_node_position_json);
    s_secure_server->registerNode(s_secure_node_position_set);
    s_secure_server->registerNode(s_secure_node_position_clear);

    if (s_secure_server->start()) {
        s_secure_running = true;
        char buf[96];
        snprintf(buf, sizeof(buf), "Secure dashboard at https://%s", WiFi.localIP().toString().c_str());
        serialmon_append(buf);
    } else {
        serialmon_append("HTTPS dashboard failed to start");
        delete s_secure_server;
        s_secure_server = nullptr;
    }
    update_webdash_status();
}

static void task_create_ssl_cert(void* parameter) {
    Preferences prefs;
    prefs.begin("MeshCoreHTTPS", false);

    size_t pkLen = prefs.getBytesLength("PK");
    size_t certLen = prefs.getBytesLength("cert");

    if (pkLen && certLen) {
        uint8_t* pkBuffer = new uint8_t[pkLen];
        prefs.getBytes("PK", pkBuffer, pkLen);

        uint8_t* certBuffer = new uint8_t[certLen];
        prefs.getBytes("cert", certBuffer, certLen);

        s_ssl_cert = new SSLCert(certBuffer, certLen, pkBuffer, pkLen);
    } else {
        s_ssl_cert = new SSLCert();
        int result = createSelfSignedCert(*s_ssl_cert, KEYSIZE_2048, "CN=meshcore.local,O=MeshCore,C=US",
                                          "20260101000000", "20360101000000");
        if (result == 0) {
            prefs.putBytes("PK", (uint8_t*)s_ssl_cert->getPKData(), s_ssl_cert->getPKLength());
            prefs.putBytes("cert", (uint8_t*)s_ssl_cert->getCertData(), s_ssl_cert->getCertLength());
        } else {
            delete s_ssl_cert;
            s_ssl_cert = nullptr;
        }
    }

    prefs.end();
    s_ssl_cert_ready = (s_ssl_cert != nullptr);
    s_ssl_cert_task_running = false;
    update_webdash_status();
    vTaskDelete(NULL);
}

static void ensure_ssl_cert_task() {
    if (s_ssl_cert_ready || s_ssl_cert_task_running || s_ssl_cert) return;
    if (s_last_https_attempt_ms != 0 && millis() - s_last_https_attempt_ms < HTTPS_RETRY_MS) return;

    s_last_https_attempt_ms = millis();
    s_ssl_cert_task_running = true;
    serialmon_append("Generating HTTPS certificate...");
    update_webdash_status();
    xTaskCreate(task_create_ssl_cert, "meshcoreHttpsCert", 8192, NULL, 2, NULL);
}

#endif // ESP32

// ── Public API ──────────────────────────────────────────────

void webdash_init() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("webdash", false);
    g_webdash_enabled = true;
    prefs.putBool("enabled", true);
    prefs.putBool("init", true);
    prefs.end();
    update_webdash_status();
#endif
}

void webdash_save_settings() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("webdash", false);
    g_webdash_enabled = true;
    prefs.putBool("enabled", true);
    prefs.putBool("init", true);
    prefs.end();
    update_webdash_status();
#endif
}

void webdash_start() {
#if defined(ESP32)
    if (s_running || !g_wifi_connected) return;

    if (!s_initialized) {
        s_server.addHandler(&s_ws);
        s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
            req->send_P(200, "text/html", INDEX_HTML);
        });
        s_server.on("/position", HTTP_GET, handle_position_page);
        s_server.on("/json/position", HTTP_GET, handle_position_json);
        s_server.on("/json/position/set", HTTP_GET, handle_position_set);
        s_server.on("/json/position/clear", HTTP_GET, handle_position_clear);
        s_server.on("/api/stats", HTTP_GET, handle_stats);
        s_server.on("/api/contacts", HTTP_GET, handle_contacts);
        s_server.on("/api/channels", HTTP_GET, handle_channels);
        s_server.on("/api/messages", HTTP_GET, handle_messages);
        s_server.on("/api/send", HTTP_POST, [](AsyncWebServerRequest* req) {
            req->send(400, "text/plain", "Body required");
        }, NULL, handle_send);
        s_server.on("/api/repeaters", HTTP_GET, handle_repeaters);
        s_server.on("/api/repeater/login", HTTP_POST, [](AsyncWebServerRequest* req) {
            req->send(400, "text/plain", "Body required");
        }, NULL, handle_rpt_login);
        s_server.on("/api/repeater/status", HTTP_POST, [](AsyncWebServerRequest* req) {
            req->send(400, "text/plain", "Body required");
        }, NULL, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            handle_rpt_action("status", req, data, len);
        });
        s_server.on("/api/repeater/neighbours", HTTP_POST, [](AsyncWebServerRequest* req) {
            req->send(400, "text/plain", "Body required");
        }, NULL, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            handle_rpt_action("neighbours", req, data, len);
        });
        s_server.on("/api/repeater/advert", HTTP_POST, [](AsyncWebServerRequest* req) {
            req->send(400, "text/plain", "Body required");
        }, NULL, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            handle_rpt_action("advert", req, data, len);
        });
        s_server.on("/api/repeater/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
            req->send(400, "text/plain", "Body required");
        }, NULL, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            handle_rpt_action("reboot", req, data, len);
        });
        s_server.on("/api/repeater/monitor", HTTP_GET, handle_rpt_monitor);
        s_server.on("/api/repeater/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
            req->send(400, "text/plain", "Body required");
        }, NULL, handle_rpt_delete);
        s_initialized = true;
    }

    s_server.begin();
    s_running = true;

    char buf[96];
    snprintf(buf, sizeof(buf), "Web dashboard at http://%s", WiFi.localIP().toString().c_str());
    serialmon_append(buf);
    snprintf(buf, sizeof(buf), "Secure dashboard at https://%s", WiFi.localIP().toString().c_str());
    serialmon_append(buf);
    update_webdash_status();
#endif
}

void webdash_stop() {
#if defined(ESP32)
    if (s_running) {
        s_ws.closeAll();
        s_server.end();
        s_running = false;
        serialmon_append("Web dashboard stopped");
    }
    stop_secure_server();
    update_webdash_status();
#endif
}

void webdash_loop() {
#if defined(ESP32)
    if (g_webdash_enabled && g_wifi_connected && !s_running) {
        webdash_start();
    } else if (s_running && (!g_wifi_connected || !g_webdash_enabled)) {
        webdash_stop();
    }
    if (s_running) s_ws.cleanupClients();
    if (g_webdash_enabled && g_wifi_connected) {
        ensure_ssl_cert_task();
        if (s_ssl_cert_ready && !s_secure_running) start_secure_server();
    }
    if (s_secure_running && s_secure_server) {
        s_secure_server->loop();
    } else if ((!g_wifi_connected || !g_webdash_enabled) && (s_secure_running || s_secure_server)) {
        stop_secure_server();
    }
#endif
}

void webdash_broadcast_message(const char* source, const char* text, bool outgoing, const char* target_key) {
#if defined(ESP32)
    if (!s_running || s_ws.count() == 0) return;

    // Determine the target key for JS to match against current chat view
    String key = "";
    if (target_key && target_key[0]) {
        key = target_key;
    }

    char buf[400];
    snprintf(buf, sizeof(buf),
        "{\"source\":\"%.30s\",\"text\":\"%.240s\",\"out\":%s,\"key\":\"%.20s\",\"time\":\"now\"}",
        source, text, outgoing ? "true" : "false", key.c_str());
    s_ws.textAll(buf);
#endif
}

const char* webdash_status_text() {
    return s_status;
}
