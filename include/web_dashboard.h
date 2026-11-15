// web_dashboard.h: Self-contained read-only monitoring dashboard served at GET /.
// Stored in Flash (.rodata); uses zero SRAM until served in chunks / single send.
#pragma once

constexpr const char kDashboardHtml[] = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#0b0f14">
<title>AtomS3 IAQ Purifier Monitor</title>
<style>
:root{--bg:#0b0f14;--card:#131a24;--b:#1f2a38;--t:#e6edf3;--m:#8b9bb0;--ok:#34d399;--warn:#fbbf24;--bad:#f87171;--acc:#38bdf8;--gas:#a78bfa;--pm:#38bdf8}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--t);padding:14px;max-width:960px;margin:0 auto;line-height:1.4}
header{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:8px;margin-bottom:14px}
h1{font-size:1.1rem;font-weight:600;display:flex;align-items:center;gap:8px}
.sub{font-size:.8rem;color:var(--m);display:flex;align-items:center;gap:8px}
button{background:var(--card);color:var(--t);border:1px solid var(--b);border-radius:8px;padding:6px 11px;font-size:.8rem;cursor:pointer}
button:hover{border-color:var(--acc)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(270px,1fr));gap:12px;margin-bottom:12px}
.card{background:var(--card);border:1px solid var(--b);border-radius:12px;padding:14px}
.hero{background:linear-gradient(135deg,#131d2b 0%,#131a24 100%);margin-bottom:12px}
.hero-top{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:12px}
.spd-wrap{display:flex;align-items:baseline;gap:12px}
.spd{font-size:2.4rem;font-weight:700;letter-spacing:-.02em}
.flow{font-size:.95rem;color:var(--m)}
.pills{display:flex;flex-wrap:wrap;gap:6px}
.pill{font-size:.75rem;font-weight:600;padding:3px 9px;border-radius:999px;border:1px solid var(--b);background:#0f1620;text-transform:uppercase;letter-spacing:.03em}
.pill.ok{color:var(--ok);border-color:rgba(52,211,153,.35);background:rgba(52,211,153,.08)}
.pill.warn{color:var(--warn);border-color:rgba(251,191,36,.35);background:rgba(251,191,36,.08)}
.pill.bad{color:var(--bad);border-color:rgba(248,113,113,.35);background:rgba(248,113,113,.08)}
.pill.acc{color:var(--acc);border-color:rgba(56,189,248,.35);background:rgba(56,189,248,.08)}
.pill.gas{color:var(--gas);border-color:rgba(167,139,250,.35);background:rgba(167,139,250,.08)}
.why{margin-top:12px;padding:10px 12px;border-radius:8px;background:#0e151f;border-left:3px solid var(--acc);font-size:.88rem}
.why strong{color:var(--t)}
.hdr{font-size:.78rem;text-transform:uppercase;letter-spacing:.06em;color:var(--m);margin-bottom:10px;display:flex;justify-content:space-between}
.metric{margin-bottom:10px}
.metric:last-child{margin-bottom:0}
.mrow{display:flex;justify-content:space-between;align-items:baseline;font-size:.88rem;margin-bottom:4px}
.mval{font-weight:600;font-variant-numeric:tabular-nums}
.mraw{font-size:.75rem;color:var(--m);margin-left:6px}
.bar{height:7px;background:#0b1017;border-radius:4px;overflow:hidden;border:1px solid #1b2532}
.fill{height:100%;border-radius:4px;transition:width .3s ease}
.thresh{font-size:.74rem;color:var(--m);margin-top:3px}
.kv{display:grid;grid-template-columns:1fr auto;gap:6px 10px;font-size:.85rem}
.k{color:var(--m)}.v{font-weight:600;text-align:right;font-variant-numeric:tabular-nums}
.ev-list{display:flex;flex-direction:column;gap:7px;max-height:320px;overflow-y:auto}
.ev{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:6px;padding:8px 10px;border-radius:8px;background:#0e151f;border:1px solid #192330;font-size:.82rem}
.ev-l{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.ev-r{color:var(--m);font-size:.77rem;font-variant-numeric:tabular-nums}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;background:var(--ok)}
.dot.err{background:var(--bad)}
@media(max-width:520px){.spd{font-size:2rem}.card{padding:12px}}
</style>
</head>
<body>
<header>
  <h1><span class="dot" id="connDot"></span>AtomS3 IAQ Purifier</h1>
  <div class="sub">
    <span id="lastUpd">Connecting...</span>
    <button id="refBtn" type="button">Refresh</button>
  </div>
</header>

<section class="card hero">
  <div class="hero-top">
    <div class="spd-wrap">
      <div class="spd" id="spdTxt">Speed --</div>
      <div class="flow" id="flowTxt">-- m&sup3;/h &bull; -- ACH</div>
    </div>
    <div class="pills">
      <span class="pill acc" id="modePill">--</span>
      <span class="pill" id="trigPill">Trigger: --</span>
      <span class="pill" id="diurnalPill">--</span>
    </div>
  </div>
  <div class="why" id="whyBox">Loading controller state...</div>
</section>

<div class="grid">
  <section class="card">
    <div class="hdr"><span>Contaminant Telemetry</span><span id="volTxt">-- m&sup3;</span></div>
    <div class="metric">
      <div class="mrow"><span>PM2.5 (SMA-5)</span><span><span class="mval" id="pmVal">--</span><span class="mraw" id="pmRaw"></span></span></div>
      <div class="bar"><div class="fill" id="pmBar" style="width:0;background:var(--pm)"></div></div>
      <div class="thresh" id="pmTh">--</div>
    </div>
    <div class="metric">
      <div class="mrow"><span>TVOC Index (SMA-5)</span><span><span class="mval" id="tvocVal">--</span><span class="mraw" id="tvocRaw"></span></span></div>
      <div class="bar"><div class="fill" id="tvocBar" style="width:0;background:var(--gas)"></div></div>
      <div class="thresh" id="tvocTh">--</div>
    </div>
    <div class="metric">
      <div class="mrow"><span>NOx Index (SMA-5)</span><span><span class="mval" id="noxVal">--</span><span class="mraw" id="noxRaw"></span></span></div>
      <div class="bar"><div class="fill" id="noxBar" style="width:0;background:var(--warn)"></div></div>
      <div class="thresh" id="noxTh">--</div>
    </div>
    <div class="kv" style="margin-top:10px;padding-top:10px;border-top:1px solid var(--b)">
      <span class="k">CO&#8322; / Temp / RH</span>
      <span class="v" id="envTxt">--</span>
    </div>
  </section>

  <section class="card">
    <div class="hdr"><span>AirGradient &amp; System Health</span><span id="agBadge" class="pill">--</span></div>
    <div class="kv">
      <span class="k">AG Target IP (Source)</span><span class="v" id="agIp">--</span>
      <span class="k">AG Fail Count / Stale</span><span class="v" id="agFails">--</span>
      <span class="k">PM / Gas Target Speed</span><span class="v" id="targets">--</span>
      <span class="k">Step-Down Hold</span><span class="v" id="dwell">--</span>
      <span class="k">Wi-Fi RSSI</span><span class="v" id="rssi">--</span>
      <span class="k">Free / Min Heap</span><span class="v" id="heap">--</span>
      <span class="k">Uptime / IR Frames</span><span class="v" id="uptime">--</span>
      <span class="k">Matter Fabrics / Changes</span><span class="v" id="matter">--</span>
    </div>
  </section>
</div>

<section class="card">
  <div class="hdr"><span>Recent Decision &amp; Health Log</span><span id="evCnt">0 events</span></div>
  <div class="ev-list" id="evList"><div class="ev">Loading activity log...</div></div>
</section>

<script>
const $=id=>document.getElementById(id);
const fmtNum=(v,d)=>v==null||Number.isNaN(v)?'--':Number(v).toFixed(d);
const fmtDur=s=>{
  s=Math.max(0,Math.floor(s||0));
  const d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),sec=s%60;
  if(d>0)return d+'d '+h+'h';
  if(h>0)return h+'h '+m+'m';
  if(m>0)return m+'m '+sec+'s';
  return sec+'s';
};
const setPill=(el,cls,txt)=>{el.className='pill '+cls;el.textContent=txt;};

function buildWhy(s){
  if(s.operating_mode==='manual')return 'Manual override active at Speed '+s.current_speed+'. Autonomous PM/gas control is paused.';
  const capPolls=s.is_night_mode?3:15;
  if(s.ag_fail_count>=capPolls)return 'AirGradient unreachable ('+s.ag_fail_count+' failed polls). Watchdog cap active at Speed '+s.current_speed+'.';
  if(s.alarm_sensor_unreachable)return 'AirGradient unreachable ('+s.ag_fail_count+' failed polls, alarm active). Holding Speed '+s.current_speed+' until cap at '+capPolls+' fails.';
  const trig=s.dominant_trigger||'none';
  if(s.current_speed===0)return 'Air quality is within clean baselines (PM target 0, Gas target 0). Purifier off.';
  const parts=[];
  if(trig==='gas'){
    parts.push('Triggered by GAS path (target Speed '+s.gas_target_speed+')');
    if(s.telemetry&&s.telemetry.tvoc_sma5!=null)parts.push('TVOC SMA-5 '+fmtNum(s.telemetry.tvoc_sma5,1));
    if(s.telemetry&&s.telemetry.nox_sma5!=null&&s.telemetry.nox_sma5>=4)parts.push('NOx SMA-5 '+fmtNum(s.telemetry.nox_sma5,1));
  }else if(trig==='pm25'){
    parts.push('Triggered by PM2.5 path (target Speed '+s.pm_target_speed+')');
    if(s.telemetry&&s.telemetry.pm25_sma5!=null)parts.push('PM2.5 SMA-5 '+fmtNum(s.telemetry.pm25_sma5,1)+' ug/m3');
  }else{
    parts.push('Running at Speed '+s.current_speed+' (PM '+s.pm_target_speed+', Gas '+s.gas_target_speed+')');
  }
  if(s.dwell_stepdown_mins>0)parts.push('holding step-down dwell ('+s.dwell_stepdown_mins+'/'+s.min_stepdown_hold_mins+'m)');
  return parts.join(' — ')+'.';
}

function renderStatus(s){
  $('spdTxt').textContent=s.current_speed===0?'Off (0)':'Speed '+s.current_speed;
  $('flowTxt').textContent=s.current_airflow_m3h+' m\u00b3/h \u2022 '+fmtNum(s.effective_ach,2)+' ACH';
  const m=s.operating_mode||'auto';
  setPill($('modePill'),m==='manual'?'warn':(m==='night_force'?'gas':'acc'),m.replace('_',' '));
  const tr=s.operating_mode==='manual'?'manual':(s.dominant_trigger||'none');
  setPill($('trigPill'),tr==='gas'?'gas':(tr==='pm25'?'acc':(tr==='manual'?'warn':'')),'Trigger: '+tr);
  setPill($('diurnalPill'),s.is_night_mode?'gas':'',s.is_night_mode?'Night Mode':'Day Mode');
  const why=$('whyBox');
  why.textContent=buildWhy(s);
  why.style.borderLeftColor=tr==='gas'?'var(--gas)':(tr==='manual'||s.alarm_sensor_unreachable?'var(--warn)':'var(--acc)');

  const t=s.telemetry||{};
  const rho=s.rho_volume_factor||1;
  $('volTxt').textContent=fmtNum(s.room_volume_m3,0)+' m\u00b3 (\u03c1='+fmtNum(rho,2)+')';
  $('pmVal').textContent=fmtNum(t.pm25_sma5,1)+' \u00b5g/m\u00b3';
  $('pmRaw').textContent='(raw '+fmtNum(t.pm25_raw,1)+')';
  $('pmBar').style.width=Math.min(100,((t.pm25_sma5||0)/55)*100)+'%';
  const pmT1=s.is_night_mode?10:5,pmT3=s.is_night_mode?25:(20*rho);
  $('pmTh').textContent=(s.is_night_mode?'Night':'Day')+' thresholds: Speed 1 >'+pmT1.toFixed(1)+', Speed 3 >'+pmT3.toFixed(1)+' \u00b5g/m\u00b3';

  $('tvocVal').textContent=fmtNum(t.tvoc_sma5,1);
  $('tvocRaw').textContent='(raw '+fmtNum(t.tvoc_raw,1)+')';
  $('tvocBar').style.width=Math.min(100,((t.tvoc_sma5||0)/500)*100)+'%';
  $('tvocTh').textContent=s.is_night_mode?'Night gas: Speed 1 >300 (release <=230)':'Day gas: Speed 2 >200 (<=150), Speed 3 >300 (<=230)';

  $('noxVal').textContent=fmtNum(t.nox_sma5,1);
  $('noxRaw').textContent='(raw '+fmtNum(t.nox_raw,1)+')';
  $('noxBar').style.width=Math.min(100,((t.nox_sma5||0)/10)*100)+'%';
  $('noxTh').textContent=s.is_night_mode?'Night gas: Speed 1 >=5.0 (release <=3.0)':'Day gas: Speed 2 >=4.0 (<=2.5), Speed 3 >=6.0 (<=4.0)';

  $('envTxt').textContent=fmtNum(t.co2_ppm,0)+' ppm \u2022 '+fmtNum(t.temperature_c,1)+'\u00b0C \u2022 '+fmtNum(t.humidity_pct,1)+'%';

  const agOk=s.ag_status==='online'&&!s.ag_data_stale;
  setPill($('agBadge'),s.alarm_sensor_unreachable?'bad':(agOk?'ok':'warn'),s.ag_status+(s.ag_data_stale?' (stale)':''));
  $('agIp').textContent=(s.ag_resolved_ip||'--')+' ('+(s.ag_source||'none')+')';
  $('agFails').textContent=s.ag_fail_count+' fails / '+(s.ag_data_stale?'STALE':'fresh');
  $('targets').textContent='PM '+s.pm_target_speed+' / Gas '+s.gas_target_speed;
  $('dwell').textContent=s.dwell_stepdown_mins+' / '+s.min_stepdown_hold_mins+' min';
  $('rssi').textContent=s.wifi_rssi_dbm+' dBm';
  $('heap').textContent=Math.round((s.free_heap_bytes||0)/1024)+' KB / '+Math.round((s.min_free_heap_bytes||0)/1024)+' KB min';
  $('uptime').textContent=fmtDur(s.uptime_s)+' / '+s.ir_frames_sent+' tx';
  $('matter').textContent=(s.matter_commissioned?s.matter_fabrics_count+' fabric(s)':'unpaired')+' / '+s.speed_changes_today+' today';
}

function renderEvents(data){
  const list=$('evList');
  const evs=(data&&data.events)||[];
  $('evCnt').textContent=evs.length+' shown ('+(data?data.total_events:0)+' total)';
  list.replaceChildren();
  if(!evs.length){
    const d=document.createElement('div');
    d.className='ev';
    d.textContent='No state transitions recorded yet since boot.';
    list.appendChild(d);
    return;
  }
  const upNow=data.uptime_s||0;
  for(const ev of evs){
    const row=document.createElement('div');row.className='ev';
    const left=document.createElement('div');left.className='ev-l';
    const p=document.createElement('span');
    let label=ev.type,cls='acc';
    if(ev.type==='speed_change'){label='Speed '+(ev.from<0?'?':ev.from)+' \u2192 '+ev.to;cls=ev.to>0?'ok':'';}
    else if(ev.type==='mode_change'){label='Mode \u2192 '+ev.mode+' (S'+ev.to+')';cls=ev.mode==='manual'?'warn':'acc';}
    else if(ev.type==='ag_alarm'){label='AG Unreachable Alarm';cls='bad';}
    else if(ev.type==='ag_recovered'){label='AG Recovered';cls='ok';}
    else if(ev.type==='watchdog_cap'){label='Watchdog Cap \u2192 S'+ev.to;cls='warn';}
    else if(ev.type==='boot'){label='Boot (S'+ev.to+', '+ev.mode+')';cls='';}
    setPill(p,cls,label);
    const tp=document.createElement('span');
    setPill(tp,ev.trigger==='gas'?'gas':(ev.trigger==='manual'?'warn':''),ev.trigger);
    left.appendChild(p);left.appendChild(tp);

    const right=document.createElement('div');right.className='ev-r';
    const age=upNow>=ev.uptime_s?fmtDur(upNow-ev.uptime_s)+' ago':fmtDur(ev.uptime_s);
    const vals=[];
    if(ev.pm25!=null)vals.push('PM '+fmtNum(ev.pm25,1));
    if(ev.tvoc!=null)vals.push('VOC '+fmtNum(ev.tvoc,0));
    if(ev.nox!=null&&ev.nox>=2)vals.push('NOx '+fmtNum(ev.nox,1));
    right.textContent=(vals.length?vals.join(' \u2022 ')+' \u2022 ':'')+age;
    row.appendChild(left);row.appendChild(right);
    list.appendChild(row);
  }
}

async function refresh(){
  try{
    const [sr,er]=await Promise.all([fetch('/api/v1/status'),fetch('/api/v1/events')]);
    if(!sr.ok)throw new Error('HTTP '+sr.status);
    renderStatus(await sr.json());
    if(er.ok)renderEvents(await er.json());
    $('connDot').className='dot';
    $('lastUpd').textContent='Updated '+new Date().toLocaleTimeString();
  }catch(e){
    $('connDot').className='dot err';
    $('lastUpd').textContent='Offline ('+e.message+')';
  }
}
$('refBtn').addEventListener('click',refresh);
refresh();
setInterval(refresh,5000);
</script>
</body>
</html>
)rawliteral";
