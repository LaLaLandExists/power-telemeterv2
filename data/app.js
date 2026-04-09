/* -- State ------------------------------------------------------- */
let cNid=null,chart=null,cMet='power',socket=null,wsOk=false,fbT=null;
let NC=[],gwTimeSet=false,gwTime='--:--:--';
let costRate=parseFloat(localStorage.getItem('pt-costRate'))||12.00;

const MC={
  power:  {l:'Power (W)',  c:'#118ab2', k:'p', n:'power'},
  voltage:{l:'Voltage (V)',c:'#ffd166', k:'v', n:'voltage'},
  current:{l:'Current (A)',c:'#06d6a0', k:'i', n:'current'}
};
const SL=['','Waiting','Active'];
const SC=['','sched-badge waiting','sched-badge active'];

/* -- Theme ------------------------------------------------------- */
const SUN_PATHS='<circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/>';
const MOON_PATH='<path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>';

function getTheme(){return localStorage.getItem('pt-theme')||(window.matchMedia('(prefers-color-scheme:light)').matches?'light':'dark');}
function applyTheme(t){
  document.documentElement.setAttribute('data-theme',t);
  $('themeIco').innerHTML=t==='dark'?MOON_PATH:SUN_PATHS;
  localStorage.setItem('pt-theme',t);
  refreshChartTheme();
}
function toggleTheme(){applyTheme(getTheme()==='dark'?'light':'dark');}

function refreshChartTheme(){
  if(!chart)return;
  const grid=getCS('--chart-grid'),tx=getCS('--chart-tx'),
        tipBg=getCS('--chart-tip-bg'),tipBd=getCS('--chart-tip-bd');
  chart.options.plugins.tooltip.backgroundColor=tipBg;
  chart.options.plugins.tooltip.borderColor=tipBd;
  chart.options.plugins.tooltip.titleColor=tx;
  chart.options.plugins.tooltip.bodyColor=tx;
  chart.options.scales.x.grid.color=grid;
  chart.options.scales.x.ticks.color=tx;
  chart.options.scales.y.grid.color=grid;
  chart.options.scales.y.ticks.color=tx;
  chart.update('none');
}

/* -- Helpers ----------------------------------------------------- */
function $(id){return document.getElementById(id);}
function sT(id,v){const e=$(id);if(e)e.textContent=v;}
function sB(id,v,mx){const e=$(id);if(e)e.style.width=Math.min(100,Math.max(0,(v/mx)*100))+'%';}
function fU(s){if(!s||s<0)return'--';return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m '+(s%60)+'s';}
function fP(w){return w>=1000?(w/1000).toFixed(1)+'k':w.toFixed(0);}
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML;}
function pad2(n){return String(n).padStart(2,'0');}
function rssiToBars(r){if(r>=-55)return 4;if(r>=-65)return 3;if(r>=-75)return 2;return 1;}

/* -- Clock ------------------------------------------------------- */
function updateClock(){const d=new Date();$('sysClock').textContent=pad2(d.getHours())+':'+pad2(d.getMinutes());}
setInterval(updateClock,10000);updateClock();

/* -- Favorites --------------------------------------------------- */
function getFavs(){try{return JSON.parse(localStorage.getItem('pt-favs')||'[]');}catch(e){return[];}}
function saveFavs(f){localStorage.setItem('pt-favs',JSON.stringify(f));}
function isFav(id){return getFavs().includes(id);}
function toggleFav(id,ev){ev.stopPropagation();let f=getFavs();if(f.includes(id))f=f.filter(x=>x!==id);else f.push(id);saveFavs(f);renderGrid(NC);}

/* -- WebSocket --------------------------------------------------- */
function connectWS(){
  const u=(location.protocol==='https:'?'wss:':'ws:')+'//'+location.host+'/ws';
  socket=new WebSocket(u);
  socket.onopen=()=>{wsOk=true;wsDot(true);syncTime();wsSend({cmd:'get_nodes'});};
  socket.onmessage=e=>{try{onMsg(JSON.parse(e.data));}catch(x){}};
  socket.onclose=()=>{wsOk=false;wsDot(false);startFallback();setTimeout(connectWS,3000);};
  socket.onerror=()=>socket.close();
}
function wsSend(o){if(wsOk&&socket&&socket.readyState===1)socket.send(JSON.stringify(o));}
function wsDot(ok){
  const el=$('wsStatus');
  el.className='hdr-badge M '+(ok?'ws-ok':'ws-err');
  el.innerHTML=`<svg width="8" height="8" viewBox="0 0 10 10" style="display:inline;margin-right:4px"><circle cx="5" cy="5" r="4" fill="currentColor"/></svg>${ok?'WS':'WS✘'}`;
}
function syncTime(){const d=new Date();wsSend({cmd:'set_time',hour:d.getHours(),minute:d.getMinutes(),second:d.getSeconds()});}
function doSyncTime(){
  syncTime();
  const btn=$('syncBtn');if(!btn)return;
  btn.classList.add('ac-active');
  const prev=btn.innerHTML;
  btn.innerHTML='<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg> Synced';
  clearTimeout(btn._t);btn._t=setTimeout(()=>{btn.innerHTML=prev;btn.classList.remove('ac-active');},2500);
}

function updGwTime(){
  const el=$('gwTimeVal');if(el)el.textContent=gwTimeSet?gwTime:'Not synced';
  const w=$('gwTimeWarn');if(w)w.style.display=gwTimeSet?'none':'block';
}

/* -- Message handler --------------------------------------------- */
function onMsg(m){
  if(m.timeSet!==undefined)gwTimeSet=m.timeSet;
  if(m.time)gwTime=m.time;
  updGwTime();

  if(m.type==='nodes'){
    NC=m.nodes||[];if(!cNid)renderGrid(NC);
    sT('idxCount',m.count||0);
    $('nodeCount').textContent=(m.count||0)+' node'+((m.count||0)!==1?'s':'');
  }
  else if(m.type==='telemetry'){
    const n=m.node;
    const o={id:n.id,label:n.label,online:n.online,rssi:n.rssi,voltage:n.voltage,current:n.current,power:n.power,energy:n.energy,frequency:n.frequency,powerFactor:n.powerFactor,relayState:n.relayState,relayMode:n.relayMode,schedState:n.schedState,alarmState:n.alarmState,age:n.age,pending:n.pending,hasSched:n.hasSched,schedStart:n.schedStart,schedEnd:n.schedEnd};
    const i=NC.findIndex(x=>x.id===n.id);if(i>=0)Object.assign(NC[i],o);else NC.push(o);
    if(!cNid)renderGrid(NC);
    if(cNid===n.id){updateDetail(o);addChartPoint(n);}
  }
  else if(m.type==='name_changed'){
    const i=NC.findIndex(x=>x.id===m.node);
    if(i>=0)NC[i].label=m.name;
    if(cNid===m.node&&!document.querySelector('.rename-input'))sT('detLabel',m.name);
    if(!cNid)renderGrid(NC);
  }
  else if(m.type==='time_set'){gwTimeSet=true;if(m.time)gwTime=m.time;updGwTime();}
  else if(m.type==='threshold_ack'){$('thrBanner').classList.remove('show');$('thrCtrl').classList.remove('frozen');if(!m.success)alert('Threshold command failed');}
  else if(m.type==='energy_cleared'){const i=NC.findIndex(x=>x.id===m.node);if(i>=0)NC[i].energy=0;if(cNid===m.node)updateDetail(NC.find(x=>x.id===cNid)||{});}
  else if(m.type==='all_energy_cleared'){NC.forEach(n=>n.energy=0);if(cNid){const c=NC.find(x=>x.id===cNid);if(c)updateDetail(c);}if(!cNid)renderGrid(NC);}
  else if((m.type==='relay_ack'||m.type==='schedule_ack'||m.type==='clear_ack')&&!m.success)alert('Command failed');
}

/* -- Fallback polling -------------------------------------------- */
function startFallback(){
  if(fbT)return;
  fbT=setInterval(async()=>{
    try{
      if(cNid){const r=await fetch('/api/node/'+cNid+'/live');const d=await r.json();if(!d.error)updateDetail(d);}
      else{const r=await fetch('/api/nodes');const d=await r.json();NC=d.nodes||[];renderGrid(NC);}
    }catch(e){}
  },3000);
}

/* -- Routing ----------------------------------------------------- */
function route(){const h=location.hash||'#/';const m=h.match(/^#\/node\/(\d+)$/);m?showDetail(parseInt(m[1])):showIndex();}
window.addEventListener('hashchange',route);
function goHome(){location.hash='#/';}
function goNode(id){location.hash='#/node/'+id;}

function showIndex(){
  cNid=null;
  document.body.classList.remove('vDet-active');
  $('vIdx').classList.add('active');$('vDet').classList.remove('active');
  $('backBtn').style.display='none';
  renderGrid(NC);wsSend({cmd:'get_nodes'});
}

/* -- Render node grid -------------------------------------------- */
function renderGrid(ns){
  const g=$('nodeGrid'),em=$('emptyState');
  if(!ns.length){g.innerHTML='';em.style.display='block';return;}
  em.style.display='none';
  const favs=getFavs();
  const sorted=[...ns].sort((a,b)=>{const af=favs.includes(a.id)?0:1,bf=favs.includes(b.id)?0:1;return af-bf||a.id-b.id;});
  const ids=new Set(sorted.map(n=>String(n.id)));
  g.querySelectorAll('.node-card').forEach(c=>{if(!ids.has(c.dataset.id))c.remove();});
  const curOrder=[...g.querySelectorAll('.node-card')].map(c=>c.dataset.id);
  const newOrder=sorted.map(n=>String(n.id));
  const orderChanged=curOrder.length!==newOrder.length||curOrder.some((v,i)=>v!==newOrder[i]);
  sorted.forEach(n=>{
    let c=g.querySelector(`.node-card[data-id="${n.id}"]`);
    const isNew=!c;
    if(isNew){c=document.createElement('div');c.className='node-card';c.dataset.id=n.id;c.onclick=()=>goNode(n.id);}
    const rs=n.relayState!==undefined?n.relayState:n.relay;
    const al=n.alarmState||n.alarm||0;
    const faved=isFav(n.id);
    const fp=`${n.label}|${n.online}|${(n.voltage||0).toFixed(1)}|${(n.current||0).toFixed(2)}|${fP(n.power||0)}|${rs}|${al}|${faved}|${n.rssi}`;
    if(c._fp!==fp){
      c._fp=fp;
      c.classList.toggle('alarm',!!al);
      const bars=rssiToBars(n.rssi||0);
      c.innerHTML=`
        <button class="fav-star ${faved?'faved':''}" onclick="toggleFav(${n.id},event)">${faved?'★':'☆'}</button>
        <div class="nc-head">
          <div>
            <div class="nc-id M">Node #${n.id}</div>
            <div class="nc-label">${esc(n.label)}</div>
          </div>
          <span class="nc-status ${n.online?'on':'off'} M">${n.online?'ONLINE':'OFFLINE'}</span>
        </div>
        <div class="nc-metrics">
          <div class="nc-metric">
            <div class="nc-metric-val M" style="color:var(--volt)">${(n.voltage||0).toFixed(1)}<small style="font-size:10px;color:var(--txd)"> V</small></div>
            <div class="nc-metric-lbl">Voltage</div>
          </div>
          <div class="nc-metric">
            <div class="nc-metric-val M" style="color:${al?'var(--dg)':'var(--watt)'}">${fP(n.power||0)}<small style="font-size:10px;color:var(--txd)"> W</small></div>
            ${!al ? '<div class="nc-metric-lbl">Power</div>' : '<div class="nc-metric-lbl alarm">⚠ ALARM</div>'}
          </div>
          <div class="nc-metric">
            <div class="nc-metric-val M" style="color:var(--amp)">${(n.current||0).toFixed(2)}<small style="font-size:10px;color:var(--txd)"> A</small></div>
            <div class="nc-metric-lbl">Current</div>
          </div>
        </div>
        <div class="nc-foot M">
          <span class="nc-rssi">
            <div class="sig-bars">
              <div class="bar ${bars>=1?'lit':''}"></div>
              <div class="bar ${bars>=2?'lit':''}"></div>
              <div class="bar ${bars>=3?'lit':''}"></div>
              <div class="bar ${bars>=4?'lit':''}"></div>
            </div>
            ${n.online?(n.rssi||'--')+' dBm':'--'}
          </span>
          <button class="nudge-btn M" data-nid="${n.id}" onclick="doNudge(${n.id},event)" ${n.online?'':'disabled style="opacity:.3;pointer-events:none"'}>
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
            Nudge
          </button>
          <span class="nc-relay ${rs?'on':'off'}">Relay: ${rs?'ON':'OFF'}</span>
          <span class="nc-arrow">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>
          </span>
        </div>
      `;
    }
    if(isNew||orderChanged)g.appendChild(c);
  });
}

/* -- Show detail ------------------------------------------------- */
function showDetail(id){
  cNid=id;
  document.body.classList.add('vDet-active');
  $('vIdx').classList.remove('active');
  $('vDet').classList.add('active');
  $('backBtn').style.display='flex';
  cMet='power';
  initChart();
  // Reset chart tab buttons via the single authoritative path
  document.querySelectorAll('[data-m]').forEach(b=>{
    const active=b.dataset.m==='power';
    b.style.borderColor=active?'var(--ac)':'';
    b.style.color=active?'var(--ac)':'';
    b.style.background=active?'rgba(0,229,160,.08)':'';
  });
  fetchHistory(id);fetchSys();
  const c=NC.find(n=>n.id===id);if(c)updateDetail(c);
}

/* -- Update detail view ------------------------------------------ */
function updateDetail(d){
  if(!document.querySelector('.rename-input'))sT('detLabel',d.label||('Node '+d.id));
  $('detSub').textContent=`ID: ${d.id}  •  LoRa: ${d.rssi||'--'} dBm  •  ${d.age||0}s ago`;

  const st=$('detStatus');
  st.textContent=d.online?'ONLINE':'OFFLINE';
  st.className='panel-badge M '+(d.online?'ac':'dg');

  sT('dV',(d.voltage||0).toFixed(1));
  sT('dI',(d.current||0).toFixed(3));
  sT('dP',(d.power||0).toFixed(1));
  if((d.energy||0)>=10000){sT('dE',((d.energy||0)/1000).toFixed(2));sT('dEu','kWh');}
  else{sT('dE',String(d.energy||0));sT('dEu','Wh');}
  sT('dF',(d.frequency||0).toFixed(1));
  sT('dPF',(d.powerFactor||0).toFixed(2));

  sB('bV',d.voltage||0,260);sB('bI',d.current||0,100);sB('bP',d.power||0,23000);
  sB('bE',d.energy||0,50000);sB('bF',d.frequency||0,65);sB('bPF',d.powerFactor||0,1);

  const al=d.alarmState||d.alarm||0;
  $('pwrCard').classList.toggle('alarm-card',!!al);
  $('alarmTag').classList.toggle('show',!!al);

  const rs=d.relayState!==undefined?d.relayState:(d.relay||0);
  const on=rs===1;
  $('relayState').textContent=on?'ON':'OFF';
  $('relayState').className='relay-state-val '+(on?'on':'off')+' M';
  $('relayToggle').checked=on;
  const rm=d.relayMode||0;
  $('relayMode').textContent=rm===1?'SCHEDULED':'MANUAL';

  const pend=d.pending||false;
  $('commitBanner').classList.toggle('show',pend);
  $('commitBannerSched').classList.toggle('show',pend);
  $('relayTabs').classList.toggle('frozen',pend);
  $('relayInner').classList.toggle('frozen',pend);

  const off=!d.online;
  $('offlineMsg').classList.toggle('show',off);
  $('relayCtrl').style.display=off?'none':'block';
  $('thrCtrl').classList.toggle('frozen',off);
  const nb=$('detNudge');if(nb){nb.disabled=off;nb.style.opacity=off?'.3':'';nb.style.pointerEvents=off?'none':'';}

  const ss=d.schedState||0;
  const si=$('schedInfo');
  const schedActive=(rm===1&&ss>0&&d.hasSched);
  const manTab=$('relayTabs').querySelector('[data-mode="manual"]');
  if(manTab)manTab.classList.toggle('tab-disabled',schedActive);
  if(schedActive){
    si.className='sched-info M show';
    si.innerHTML=`<div><strong>Daily schedule</strong><span class="${SC[ss]||''}">${SL[ss]||''}</span><br>ON: ${d.schedStart||'--:--'} → OFF: ${d.schedEnd||'--:--'}</div><div class="sched-clear" onclick="doClear()">Clear</div>`;
    setRelayTab('schedule');
  }else{si.className='sched-info M';}

  const cost=((d.energy||0)/1000)*costRate;
  $('costVal').textContent=cost.toFixed(2);
  $('costRate2').textContent='@ '+costRate.toFixed(2)+' / kWh';
  $('costEnergy').textContent=(d.energy||0)+' Wh';


}

/* -- Relay controls ---------------------------------------------- */
function setRelayTab(m){
  document.querySelectorAll('#relayTabs div').forEach(t=>t.classList.toggle('active',t.dataset.mode===m));
  $('tabManual').classList.toggle('active',m==='manual');
  $('tabSchedule').classList.toggle('active',m==='schedule');
}
function doManual(c){wsSend({cmd:'relay_manual',node:cNid,state:c?1:0});$('commitBanner').classList.add('show');}
function doSchedule(){
  const sv=$('schedStart').value,ev=$('schedEnd').value;
  if(!sv||!ev){alert('Set both times.');return;}
  const[sh,sm]=sv.split(':').map(Number),[eh,em]=ev.split(':').map(Number);
  wsSend({cmd:'relay_schedule',node:cNid,startH:sh,startM:sm,endH:eh,endM:em});
  $('commitBannerSched').classList.add('show');
}
function doClear(){wsSend({cmd:'relay_clear',node:cNid});$('commitBannerSched').classList.add('show');}
function doSetThreshold(){
  const v=parseInt($('thrInput').value);
  if(!v||v<1||v>23000){alert('Enter 1–23000 W');return;}
  wsSend({cmd:'set_threshold',node:cNid,watts:v});
  $('thrBanner').classList.add('show');$('thrCtrl').classList.add('frozen');
}

/* -- Confirm dialogs --------------------------------------------- */
function showConfirm(id){$(id).classList.add('show');}
function hideConfirm(id){$(id).classList.remove('show');}
function execClearEnergy(){hideConfirm('cfmEnergy');wsSend({cmd:'clear_energy',node:cNid});}
function execClearAllEnergy(){hideConfirm('cfmAllEnergy');wsSend({cmd:'clear_all_energy'});}

/* -- Nudge ------------------------------------------------------- */
function doNudge(nodeId,ev){
  if(ev)ev.stopPropagation();
  wsSend({cmd:'nudge',node:nodeId});
  document.querySelectorAll(`.nudge-btn[data-nid="${nodeId}"]`).forEach(btn=>{
    btn.classList.add('nudged');
    btn.innerHTML='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg> Nudged';
    clearTimeout(btn._t);btn._t=setTimeout(()=>{btn.classList.remove('nudged');btn.innerHTML='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg> Nudge';},3000);
  });
  const dtBtn=$('detNudge');
  if(dtBtn&&cNid===nodeId){
    dtBtn.classList.add('nudged');
    dtBtn.innerHTML='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg> Nudged';
    clearTimeout(dtBtn._t);dtBtn._t=setTimeout(()=>{dtBtn.classList.remove('nudged');dtBtn.innerHTML='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg> Nudge';},3000);
  }
}

/* -- Cost rate --------------------------------------------------- */
function setCostRate(){
  const v=parseFloat($('costRateIn').value);
  if(!v||v<=0){alert('Enter a valid rate.');return;}
  costRate=v;localStorage.setItem('pt-costRate',v);
  const btn=$('costSetBtn');
  const prev=btn.textContent;
  btn.textContent='✓ Saved';btn.style.background='rgba(0,229,160,.15)';btn.style.color='var(--ac)';
  btn.disabled=true;clearTimeout(btn._t);
  btn._t=setTimeout(()=>{btn.textContent=prev;btn.style.background='var(--ac)';btn.style.color='var(--bg)';btn.disabled=false;},2000);
}

/* -- Rename ------------------------------------------------------- */
function startRename(){
  const el=$('detLabel');if(!el)return;
  const cur=el.textContent;
  const inp=document.createElement('input');
  inp.className='rename-input';inp.value=cur;inp.maxLength=29;
  el.replaceWith(inp);inp.focus();inp.select();
  let done=false;
  function commit(){
    if(done)return;done=true;
    const nv=inp.value.trim();
    if(nv&&nv!==cur)wsSend({cmd:'rename',node:cNid,name:nv});
    const h2=document.createElement('h2');h2.id='detLabel';h2.textContent=nv||cur;
    h2.onclick=startRename;h2.title='Click to rename';
    if(inp.isConnected)inp.replaceWith(h2);
    else{const p=document.querySelector('.det-hdr-l');if(p)p.prepend(h2);}
  }
  inp.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();commit();}if(e.key==='Escape'){inp.value=cur;commit();}});
  inp.addEventListener('blur',commit);
}

/* -- Chart ------------------------------------------------------- */
function getCS(v){return getComputedStyle(document.documentElement).getPropertyValue(v).trim();}

function initChart(){
  if(chart)chart.destroy();
  const mc=MC[cMet];
  chart=new Chart($('chartCanvas').getContext('2d'),{
    type:'line',
    data:{labels:[],datasets:[{label:mc.l,data:[],borderColor:mc.c,backgroundColor:mc.c+'18',fill:true,tension:.35,borderWidth:2,pointRadius:0,pointHitRadius:10}]},
    options:{
      responsive:true,maintainAspectRatio:false,animation:{duration:300},
      interaction:{intersect:false,mode:'index'},
      plugins:{
        legend:{display:false},
        tooltip:{
          backgroundColor:getCS('--chart-tip-bg'),borderColor:getCS('--chart-tip-bd'),borderWidth:1,
          titleColor:getCS('--chart-tx'),bodyColor:getCS('--chart-tx'),
          titleFont:{family:"'JetBrains Mono'",size:10},bodyFont:{family:"'JetBrains Mono'",size:11},
          padding:8,cornerRadius:5
        }
      },
      scales:{
        x:{grid:{color:getCS('--chart-grid')},ticks:{color:getCS('--chart-tx'),font:{family:"'JetBrains Mono'",size:9},maxTicksLimit:8}},
        y:{grid:{color:getCS('--chart-grid')},ticks:{color:getCS('--chart-tx'),font:{family:"'JetBrains Mono'",size:9}}}
      }
    }
  });
}

function swChart(m){
  cMet=m;
  // Update tab button styles
  document.querySelectorAll('[data-m]').forEach(b=>{
    const active=b.dataset.m===m;
    b.style.borderColor=active?'var(--ac)':'';
    b.style.color=active?'var(--ac)':'';
    b.style.background=active?'rgba(0,229,160,.08)':'';
  });
  const mc=MC[m];
  chart.data.datasets[0].label=mc.l;
  chart.data.datasets[0].borderColor=mc.c;
  chart.data.datasets[0].backgroundColor=mc.c+'18';
  chart.data.datasets[0].data=[];chart.data.labels=[];
  chart.update('none');fetchHistory(cNid);
}

function addChartPoint(n){
  if(!chart)return;
  const mc=MC[cMet];
  const v=n[mc.n]!==undefined?n[mc.n]:n[mc.k];
  if(v===undefined)return;
  const ds=chart.data.datasets[0];
  ds.data.push(v);chart.data.labels.push('');
  if(ds.data.length>120){ds.data.shift();chart.data.labels.shift();}
  chart.data.labels=ds.data.map((_,i)=>{const s=(ds.data.length-1-i)*3;return s>60?Math.floor(s/60)+'m':s+'s';});
  chart.update('none');
}

async function fetchHistory(id){
  try{
    const r=await fetch('/api/node/'+id+'/history');
    const d=await r.json();
    if(!d.length||!chart)return;
    const mc=MC[cMet];
    chart.data.labels=d.map((_,i)=>{const s=(d.length-1-i)*3;return s>60?Math.floor(s/60)+'m':s+'s';});
    chart.data.datasets[0].data=d.map(x=>x[mc.k]);
    chart.update('none');
  }catch(e){}
}

async function fetchSys(){
  try{
    const r=await fetch('/api/status');const d=await r.json();
    sT('gwUptime',fU(d.uptime));
    sT('gwHeap',(d.freeHeap/1024).toFixed(0)+' KB');
    sT('gwNodes',(d.nodeCount||0)+'/'+(d.maxNodes||8));
    if(d.timeSet!==undefined)gwTimeSet=d.timeSet;
    if(d.time)gwTime=d.time;
    updGwTime();
    if(d.version)sT('fwVer',d.version);
    updNetPanel(d);
  }catch(e){}
}

function updNetPanel(d){
  const apMode = d.wifiMode==='ap' || !d.wifiRSSI;
  sT('gwSSID', d.ssid||'--');
  const badge=$('gwModeBadge');
  if(badge){
    badge.textContent=apMode?'AP':'STA';
    badge.className='panel-badge M '+(apMode?'wn':'ac');
  }
  const icon=$('gwNetIcon');
  if(icon){
    icon.className='gw-net-icon '+(apMode?'ap':'sta');
    if(apMode){
      icon.innerHTML='<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" opacity="0.4" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>';
    }else{
      const rssi=d.wifiRSSI||0;
      icon.innerHTML=(rssi>=-55?'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':rssi>=-65?'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':rssi>=-75?'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>');
    }
  }
  sT('gwIP', d.ip||'--');
}


/* -- WiFi config drawer ------------------------------------------ */
let wifiSelectedSSID = '';
let wifiPollIv = null;
let wifiDrawerOpen = false;

function toggleWifiDrawer() {
  wifiDrawerOpen = !wifiDrawerOpen;
  const drawer = $('wifiDrawer');
  const btn    = $('wifiCfgBtn');
  drawer.classList.toggle('open', wifiDrawerOpen);
  btn.classList.toggle('ac-active', wifiDrawerOpen);
  if (wifiDrawerOpen) wifiShowScan();
}

/* -- Screen routing -------------------------------------------- */
function wifiShowScan() {
  $('wifiScr-scan').style.display       = '';
  $('wifiScr-connecting').style.display = 'none';
  $('wifiScr-result').style.display     = 'none';
}
function wifiShowConnecting(ssid) {
  $('wifiScr-scan').style.display       = 'none';
  $('wifiScr-connecting').style.display = '';
  $('wifiScr-result').style.display     = 'none';
  $('wifiConnTitle').textContent = 'Connecting…';
  $('wifiConnSub').textContent   = ssid;
}
function wifiShowResult(ok, ip, ssid) {
  $('wifiScr-scan').style.display       = 'none';
  $('wifiScr-connecting').style.display = 'none';
  $('wifiScr-result').style.display     = '';
  const ico = $('wifiResultIcon');
  if (ok) {
    ico.style.borderColor  = 'var(--ac)';
    ico.style.background   = 'rgba(0,229,160,.1)';
    ico.style.color        = 'var(--ac)';
    ico.innerHTML = '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>';
  } else {
    ico.style.borderColor  = 'var(--dg)';
    ico.style.background   = 'rgba(255,56,96,.1)';
    ico.style.color        = 'var(--dg)';
    ico.innerHTML = '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>';
  }
  $('wifiResultTitle').textContent = ok ? 'Connected!' : 'Failed';
  $('wifiResultSub').innerHTML = ok
    ? `Gateway joined <strong>${esc(ssid)}</strong>.<br>IP: <span style="color:var(--ac)">${esc(ip)}</span>`
    : `Could not connect to <strong>${esc(ssid)}</strong>.<br>Check password and try again.`;
  const backBtn = $('wifiBackBtn');
  backBtn.onclick = ok ? (() => { toggleWifiDrawer() }) : (() => { wifiShowScan() });
}

/* -- Scan ------------------------------------------------------- */
function wifiDoScan() {
  const btn  = $('wifiScanBtn');
  const list = $('wifiNetList');
  btn.classList.add('spinning');
  btn.disabled = true;
  list.innerHTML = '<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">Scanning…</div>';
  fetch('/api/scan')
    .then(r => r.json())
    .then(d => wifiRenderNetworks(d.networks || []))
    .catch(() => {
      list.innerHTML = '<div style="text-align:center;padding:18px 0;color:var(--dg);font-size:11px;font-family:monospace">Scan failed — check connection</div>';
    })
    .finally(() => { btn.classList.remove('spinning'); btn.disabled = false; });
}

function wifiRenderNetworks(nets) {
  const list = $('wifiNetList');
  if (!nets.length) {
    list.innerHTML = '<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">No networks found</div>';
    return;
  }
  const lockSvg = '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>';
  const openSvg = '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>';
  list.innerHTML = '';
  nets.sort((a, b) => b.rssi - a.rssi).forEach((n, i) => {
    const bars = rssiToBars(n.rssi);
    const el = document.createElement('div');
    el.className = 'wifi-net-item';
    el.style.animationDelay = (i * 0.04) + 's';
    el.style.animation = 'slideIn .3s ease both';
    el.innerHTML =
      '<span style="color:var(--txd);display:flex;align-items:center">' + (n.secure ? lockSvg : openSvg) + '</span>' +
      '<div class="wifi-net-info">' +
        '<div class="wifi-net-name">' + esc(n.ssid) + '</div>' +
        '<div class="wifi-net-meta">' + n.rssi + ' dBm · ' + (n.secure ? 'WPA2' : 'Open') + '</div>' +
      '</div>' +
      (bars>=4?'<svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M1.42 9a16 16 0 0 1 21.16 0\"/><path d=\"M5 12.55a11 11 0 0 1 14.08 0\"/><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"/><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"2.5\"/></svg>':bars>=3?'<svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M5 12.55a11 11 0 0 1 14.08 0\"/><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"/><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"2.5\"/></svg>':bars>=2?'<svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"/><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"2.5\"/></svg>':'<svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"2.5\"/></svg>');
    el.addEventListener('click', () => wifiSelectNetwork(n.ssid, n.secure, el));
    list.appendChild(el);
  });
}

function wifiSelectNetwork(ssid, secure, el) {
  document.querySelectorAll('.wifi-net-item').forEach(i => i.classList.remove('selected'));
  el.classList.add('selected');
  wifiSelectedSSID = ssid;
  // Uncheck manual if was checked
  $('wifiManualCb').checked = false;
  wifiToggleManual();
  // Show cred section
  $('wifiCredSection').style.display = 'block';
  const pw = $('wifiPwInput');
  if (!secure) { pw.value = ''; pw.placeholder = 'No password required'; pw.disabled = true; }
  else         { pw.placeholder = 'Enter password…'; pw.disabled = false; pw.focus(); }
  wifiValidateConnect();
}

/* -- Manual SSID toggle ----------------------------------------- */
function wifiToggleManual() {
  const manual = $('wifiManualCb').checked;
  $('wifiManualInput').style.display = manual ? 'block' : 'none';
  if (manual) {
    document.querySelectorAll('.wifi-net-item').forEach(i => i.classList.remove('selected'));
    wifiSelectedSSID = '';
    $('wifiCredSection').style.display = 'block';
    const pw = $('wifiPwInput');
    pw.placeholder = 'Enter password…'; pw.disabled = false;
    $('wifiManualSSID').focus();
  } else {
    if (!wifiSelectedSSID) $('wifiCredSection').style.display = 'none';
  }
  wifiValidateConnect();
}

function wifiValidateConnect() {
  const manual = $('wifiManualCb').checked;
  const ssid   = manual ? $('wifiManualSSID').value.trim() : wifiSelectedSSID;
  $('wifiConnectBtn').disabled = !ssid;
}

/* -- Password eye ----------------------------------------------- */
function wifiTogglePw() {
  const inp  = $('wifiPwInput');
  const ico  = $('wifiEyeIco');
  const show = inp.type === 'password';
  inp.type = show ? 'text' : 'password';
  ico.innerHTML = show
    ? '<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94"/><path d="M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19"/><line x1="1" y1="1" x2="23" y2="23"/>'
    : '<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>';
}

/* -- Connect ---------------------------------------------------- */
function wifiDoConnect() {
  const manual = $('wifiManualCb').checked;
  const ssid   = manual ? $('wifiManualSSID').value.trim() : wifiSelectedSSID;
  const pw     = $('wifiPwInput').value;
  if (!ssid) return;
  wifiShowConnecting(ssid);
  fetch('/api/connect', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pw)
  })
  .then(r => r.json())
  .then(d => {
    if (d.status === 'connecting') {
      wifiPollStatus(ssid);
    } else {
      wifiShowResult(false, '', ssid);
    }
  })
  .catch(() => { wifiShowResult(false, '', ssid); });
}

function wifiPollStatus(ssid) {
  let tries = 0;
  clearInterval(wifiPollIv);
  wifiPollIv = setInterval(() => {
    fetch('/api/wifistatus')
      .then(r => r.json())
      .then(d => {
        if (d.connected) {
          clearInterval(wifiPollIv);
          wifiShowResult(true, d.ip, ssid);
          fetchSys();
        } else if (tries++ > 25) {
          clearInterval(wifiPollIv);
          wifiShowResult(false, '', ssid);
        }
      })
      .catch(() => { tries++; });
  }, 800);
}

function wifiCancelConnect() {
  clearInterval(wifiPollIv);
  fetch('/api/disconnect').catch(() => {});
  wifiShowScan();
}



/* -- Forget ----------------------------------------------------- */
function wifiDoForget() {
  hideConfirm('cfmForget');
  fetch('/api/forget')
    .then(() => {
      // Clear selection state
      wifiSelectedSSID = '';
      $('wifiNetList').innerHTML = '<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">Credentials cleared. Scan to reconnect.</div>';
      $('wifiCredSection').style.display = 'none';
      $('wifiManualCb').checked = false;
      wifiToggleManual();
      // Refresh status panel — gateway is now AP-only
      setTimeout(fetchSys, 500);
    })
    .catch(() => {});
}

/* -- Init -------------------------------------------------------- */
function init() {
  document.addEventListener('DOMContentLoaded',()=>{
    applyTheme(getTheme());
    $('costRateIn').value=costRate;
    connectWS();
    route();
    setInterval(fetchSys,10000);
    fetchSys();
  });
}