/* -----------------------------------------------------------------
   SIMULATION ENGINE
   Replaces connectWS, wsSend, fetchSys, doSyncTime, openSetup.
   All other UI functions are identical to production.
   ----------------------------------------------------------------- */

/* -- Node profiles -- */
const SIM_PROFILES = [
  { id:1, label:'Refrigerator',  nominalPower:120,  idlePower:5,   cyclePeriod:600, cycleOnFrac:0.6,  pf:0.72, pfNoise:0.04, voltNominal:220, freqNominal:60, rssiBase:-58, alarmThreshold:500,  relayState:1, relayMode:0 },
  { id:2, label:'AC Unit',       nominalPower:1450, idlePower:30,  cyclePeriod:900, cycleOnFrac:0.75, pf:0.82, pfNoise:0.03, voltNominal:220, freqNominal:60, rssiBase:-71, alarmThreshold:2000, relayState:1, relayMode:0 },
  { id:3, label:'Outdoor Lights', nominalPower:85,   idlePower:0,   cyclePeriod:3600,cycleOnFrac:1.0,  pf:0.95, pfNoise:0.01, voltNominal:220, freqNominal:60, rssiBase:-44, alarmThreshold:200,  relayState:1, relayMode:0 },
  { id:4, label:'PC',   nominalPower:320,  idlePower:95,  cyclePeriod:120, cycleOnFrac:0.55, pf:0.88, pfNoise:0.05, voltNominal:220, freqNominal:60, rssiBase:-65, alarmThreshold:600,  relayState:1, relayMode:0 },
  { id:5, label:'Heater',  nominalPower:2000, idlePower:0,   cyclePeriod:600, cycleOnFrac:0.5,  pf:0.99, pfNoise:0.01, voltNominal:220, freqNominal:60, rssiBase:-88, alarmThreshold:3000, relayState:0, relayMode:0, _permanentlyOffline:true },
];

const SIM_TICK_MS  = 3000;
const SIM_HIST_LEN = 120;

const simState = SIM_PROFILES.map(p => ({
  ...p,
  online:       p._permanentlyOffline ? false : true,
  energy:       Math.random() * 800 + 50,
  age:          0,
  pending:      false,
  hasSched:     false,
  schedStart:   '08:00',
  schedEnd:     '17:00',
  schedState:   0,
  _t:           Math.random() * p.cyclePeriod,
  _offlineTimer:0,
  _offlineIn:   p._permanentlyOffline ? Infinity : randomOfflineDelay(),
  _hist:        { p:[], v:[], i:[] },
}));

let simUptime = 0;
let simHeap   = 180 + Math.random() * 40;
let simRSSI   = -52 + Math.floor(Math.random() * 10);
let simSSID   = 'HomeNetwork_5G';

function randomOfflineDelay(){ return 240 + Math.random() * 480; }

function noise(amp){ return (Math.random()-0.5)*2*amp; }

function simTick(){
  const dt  = SIM_TICK_MS / 1000;
  const now = new Date();
  simState.forEach(s=>{
    s._offlineIn -= dt;
    if(s._offlineIn<=0 && s.online){ s.online=false; s._offlineTimer=4+Math.random()*8; }
    if(!s.online){
      s._offlineTimer-=dt;
      if(s._offlineTimer<=0 && !s._permanentlyOffline){ s.online=true; s._offlineIn=randomOfflineDelay(); }
      s.age=0; return;
    }
    s.age=Math.round(dt);
    s._t+=dt; if(s._t>s.cyclePeriod)s._t-=s.cyclePeriod;
    const inOn=(s._t/s.cyclePeriod)<s.cycleOnFrac;
    if(!s.relayState){
      s.voltage=s.voltNominal+noise(0.6); s.current=0; s.power=0;
      s.frequency=s.freqNominal+noise(0.06); s.powerFactor=0; s.alarmState=0;
      appendHist(s,0,s.voltage,0); return;
    }
    const raw = inOn ? s.nominalPower+s.nominalPower*0.12*Math.sin(s._t*0.8)+noise(s.nominalPower*0.05)
                     : s.idlePower+noise(s.idlePower*0.1+0.5);
    const p   = Math.max(0,raw);
    const I0  = p/(s.voltNominal*s.pf+0.001);
    const v   = Math.max(195, s.voltNominal-I0*0.3+noise(0.4));
    const pf  = Math.min(1,Math.max(0.5,s.pf+noise(s.pfNoise)));
    const i   = p/(v*pf+0.001);
    const f   = s.freqNominal+0.06*Math.sin(Date.now()/18000)+noise(0.04);
    s.energy += p*(dt/3600);
    s.alarmState = (p>s.alarmThreshold)?1:0;
    s.voltage=parseFloat(v.toFixed(1)); s.current=parseFloat(i.toFixed(3));
    s.power=parseFloat(p.toFixed(1));   s.frequency=parseFloat(f.toFixed(2));
    s.powerFactor=parseFloat(pf.toFixed(2));
    appendHist(s,s.power,s.voltage,s.current);
  });

  const nodes=simState.map(nodeSnapshot);
  nodes.forEach(n=>{ const i=NC.findIndex(x=>x.id===n.id); if(i>=0)Object.assign(NC[i],n); else NC.push(n); });
  if(!cNid)renderGrid(NC);
  if(cNid){ const s=simState.find(x=>x.id===cNid); if(s){ updateDetail(nodeSnapshot(s)); addChartPoint({p:s.power,v:s.voltage,i:s.current,power:s.power,voltage:s.voltage,current:s.current}); } }
  const cnt=simState.filter(s=>s.online).length;
  $('nodeCount').textContent=cnt+' node'+(cnt!==1?'s':'');
  sT('idxCount',simState.length);
  gwTimeSet=true; gwTime=pad2(now.getHours())+':'+pad2(now.getMinutes())+':'+pad2(now.getSeconds());
  updGwTime();
  simUptime+=dt;
}

function appendHist(s,p,v,i){
  s._hist.p.push(parseFloat(p.toFixed(1)));
  s._hist.v.push(parseFloat(v.toFixed(1)));
  s._hist.i.push(parseFloat(i.toFixed(3)));
  if(s._hist.p.length>SIM_HIST_LEN){s._hist.p.shift();s._hist.v.shift();s._hist.i.shift();}
}

function nodeSnapshot(s){
  return {id:s.id,label:s.label,online:s.online,rssi:s.online?Math.round(s.rssiBase+noise(6)):s.rssiBase,
    voltage:s.voltage||s.voltNominal,current:s.current||0,power:s.power||0,
    energy:parseFloat((s.energy||0).toFixed(1)),frequency:s.frequency||s.freqNominal,
    powerFactor:s.powerFactor||s.pf,relayState:s.relayState,relayMode:s.relayMode,
    schedState:s.schedState,alarmState:s.alarmState||0,age:s.age||0,pending:s.pending||false,
    hasSched:s.hasSched,schedStart:s.schedStart,schedEnd:s.schedEnd};
}

connectWS = function(){
  wsDot(true);
  NC=simState.map(nodeSnapshot);
  renderGrid(NC);
  sT('idxCount',simState.length);
  $('nodeCount').textContent=simState.length+' nodes';
  setInterval(simTick,SIM_TICK_MS);
  simTick();
}

wsSend = function(cmd){
  if(!cmd||!cmd.cmd)return;
  switch(cmd.cmd){
    case 'get_nodes': break;
    case 'relay_manual':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      s.pending=true; renderGrid(NC);
      setTimeout(()=>{ s.relayState=cmd.state; s.relayMode=0; s.pending=false;
        if(cNid===cmd.node){$('commitBanner').classList.remove('show');updateDetail(nodeSnapshot(s));}
        renderGrid(NC); }, 600+Math.random()*400); break;
    }
    case 'relay_schedule':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      s.pending=true;
      setTimeout(()=>{
        s.relayMode=1; s.hasSched=true;
        s.schedStart=pad2(cmd.startH)+':'+pad2(cmd.startM);
        s.schedEnd=pad2(cmd.endH)+':'+pad2(cmd.endM);
        const now=new Date(); const nm=now.getHours()*60+now.getMinutes();
        const on=cmd.startH*60+cmd.startM; const off=cmd.endH*60+cmd.endM;
        const inW=on<off?(nm>=on&&nm<off):(nm>=on||nm<off);
        s.schedState=inW?2:1; s.relayState=inW?1:0; s.pending=false;
        if(cNid===cmd.node){$('commitBannerSched').classList.remove('show');updateDetail(nodeSnapshot(s));}
      },700+Math.random()*300); break;
    }
    case 'relay_clear':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      s.pending=true;
      setTimeout(()=>{ s.relayMode=0; s.hasSched=false; s.schedState=0; s.pending=false;
        if(cNid===cmd.node){$('commitBannerSched').classList.remove('show');updateDetail(nodeSnapshot(s));} },500); break;
    }
    case 'set_threshold':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      setTimeout(()=>{ s.alarmThreshold=cmd.watts;
        $('thrBanner').classList.remove('show'); $('thrCtrl').classList.remove('frozen');
        if(cNid===cmd.node)updateDetail(nodeSnapshot(s)); },400); break;
    }
    case 'clear_energy':{
      const s=simState.find(x=>x.id===cmd.node); if(s){s.energy=0;} break;
    }
    case 'clear_all_energy':{ simState.forEach(s=>s.energy=0); break; }
    case 'rename':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      s.label=cmd.name; const n=NC.find(x=>x.id===cmd.node); if(n)n.label=cmd.name;
      if(!cNid)renderGrid(NC); break;
    }
    case 'nudge':{
      const s=simState.find(x=>x.id===cmd.node);
      if(s&&s.online){s.online=false;setTimeout(()=>{s.online=true;},1200);} break;
    }
    case 'set_time': gwTimeSet=true; break;
  }
}

fetchHistory = async function(id){
  const s=simState.find(x=>x.id===id); if(!s||!chart)return;
  const mc=MC[cMet]; const buf=s._hist[mc.k]||[];
  if(!buf.length)return;
  chart.data.labels=buf.map((_,i)=>{ const sec=(buf.length-1-i)*(SIM_TICK_MS/1000); return sec>60?Math.floor(sec/60)+'m':sec+'s'; });
  chart.data.datasets[0].data=[...buf]; chart.update('none');
}

fetchSys = async function(){
  simHeap=Math.max(140,Math.min(220,simHeap+(Math.random()-0.5)*4));
  simRSSI=Math.max(-75,Math.min(-35,simRSSI+Math.round((Math.random()-0.5)*2)));
  sT('gwUptime',fU(Math.round(simUptime)));
  sT('gwHeap',simHeap.toFixed(0)+' KB');
  sT('gwNodes',simState.length+'/'+simState.length);
  sT('fwVer','Simulation Edition');
  gwTimeSet=true; updGwTime();
  updNetPanel({ wifiMode:'sta', ssid:simSSID, wifiRSSI:simRSSI, ip:'192.168.1.42' });
}

doSyncTime = function(){
  gwTimeSet=true;
  const btn=$('syncBtn'); if(!btn)return;
  btn.classList.add('ac-active');
  const prev=btn.innerHTML;
  btn.innerHTML='<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg> Synced';
  clearTimeout(btn._t); btn._t=setTimeout(()=>{ btn.innerHTML=prev; btn.classList.remove('ac-active'); },2500);
}

function injectSimBanner(){
  const hdr=document.querySelector('.hdr-r');
  const badge=document.createElement('span');
  badge.className='hdr-badge M';
  badge.style.cssText='border-color:rgba(255,159,67,.4);color:var(--wn);background:rgba(255,159,67,.08)';
  badge.innerHTML='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" style="margin-right:4px;vertical-align:middle"><polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/></svg>SIM';
  hdr.prepend(badge);
  document.title='PowerTelemetry \u00b7 Dashboard \u00b7 SIM';
}

wifiDoScan = function(){
  const btn=$('wifiScanBtn'); const list=$('wifiNetList');
  btn.classList.add('spinning'); btn.disabled=true;
  list.innerHTML='<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">Scanning…</div>';
  setTimeout(()=>{
    try{
      wifiRenderNetworks([
        {ssid:'HomeNetwork_5G',  rssi:-42, secure:true},
        {ssid:'PLDT_FIBER_001A', rssi:-61, secure:true},
        {ssid:'AndroidAP',      rssi:-73, secure:true},
        {ssid:'GuestWifi',      rssi:-55, secure:false},
        {ssid:'DIRECT-Printer', rssi:-80, secure:false},
      ]);
    }finally{ btn.classList.remove('spinning'); btn.disabled=false; }
  },900);
};

wifiDoConnect = function(){
  const manual=$('wifiManualCb').checked;
  const ssid=manual?$('wifiManualSSID').value.trim():wifiSelectedSSID;
  if(!ssid)return;
  wifiShowConnecting(ssid);
  let tries=0;
  clearInterval(wifiPollIv);
  wifiPollIv=setInterval(()=>{
    tries++;
    if(tries>=4){
      clearInterval(wifiPollIv);
      if (ssid === 'FailureSSID') {
        wifiShowResult(false, '', ssid); fetchSys();
      } else {
        simSSID=ssid; simRSSI=-52;
        wifiShowResult(true,'192.168.1.42',ssid); fetchSys();
      }
    }
  },800);
};

wifiCancelConnect = function(){
  clearInterval(wifiPollIv);
  wifiShowScan();
};

wifiDoForget = function(){
  hideConfirm('cfmForget');
  simSSID='PowerTelemetry-Setup'; simRSSI=0;
  wifiSelectedSSID='';
  $('wifiNetList').innerHTML='<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">Credentials cleared. Scan to reconnect.</div>';
  $('wifiCredSection').style.display='none';
  $('wifiManualCb').checked=false;
  wifiToggleManual();
  setTimeout(fetchSys,500);
};

oldInit = init;
init = function() {
  // Inject the simulation indicator
  oldInit();
  document.addEventListener('DOMContentLoaded',()=>{
    injectSimBanner();
  });
};