#ifndef CPU0_ESP_REPORT_WEB_H
#define CPU0_ESP_REPORT_WEB_H

static const char g_esp_report_web_page[] =
"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>RA8P1 RF Monitor</title><style>"
"*{box-sizing:border-box}body{margin:0;background:#101418;color:#eef2f5;font-family:Arial,sans-serif;letter-spacing:0}"
"header{height:58px;display:flex;align-items:center;justify-content:space-between;padding:0 18px;border-bottom:1px solid #303840;background:#171c21}"
"h1{font-size:18px;margin:0}.net{font-size:12px;color:#9eabb5}.wrap{max-width:760px;margin:auto;padding:14px}"
".state{display:flex;align-items:center;gap:14px;padding:14px 0;border-bottom:1px solid #303840}"
".lamp{width:18px;height:18px;border-radius:50%;background:#34c77b;box-shadow:0 0 0 5px #34c77b22}"
".alarm .lamp{background:#ff4f5e;box-shadow:0 0 0 5px #ff4f5e22}.state b{font-size:22px}.muted{color:#9eabb5;font-size:12px}"
"h2{font-size:13px;margin:18px 0 8px;color:#9eabb5;text-transform:uppercase}"
".targets{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}.target{height:54px;display:flex;align-items:center;justify-content:space-between;padding:0 12px;border:1px solid #303840;border-left:4px solid #59636d;background:#171c21}"
".target.on{border-left-color:#ffcc45;background:#242117}.target span{font-size:14px}.target i{font-style:normal;font-size:11px;color:#9eabb5}"
".target.on i{color:#ffcc45}.stats{display:grid;grid-template-columns:repeat(3,1fr);border:1px solid #303840;background:#171c21}"
".stat{padding:12px;border-right:1px solid #303840}.stat:last-child{border:0}.stat b{display:block;font-size:16px}.stat small{color:#9eabb5}"
"footer{padding-top:16px;color:#73808a;font-size:11px}@media(max-width:520px){.targets{grid-template-columns:1fr}.stats{grid-template-columns:1fr}.stat{border-right:0;border-bottom:1px solid #303840}}"
"</style></head><body><header><h1>RA8P1 RF Monitor</h1><div class='net' id='net'>Connecting</div></header>"
"<main class='wrap'><section class='state' id='state'><div class='lamp'></div><div><b id='headline'>Waiting for data</b><div class='muted' id='event'>--</div></div></section>"
"<h2>Detected targets</h2><section class='targets'>"
"<div class='target' id='t0'><span>DJI</span><i>INACTIVE</i></div>"
"<div class='target' id='t1'><span>RadioLink AT9S</span><i>INACTIVE</i></div>"
"<div class='target' id='t2'><span>Yunzhuo T12</span><i>INACTIVE</i></div>"
"<div class='target' id='t3'><span>Xiaobawang</span><i>INACTIVE</i></div></section>"
"<h2>System</h2><section class='stats'><div class='stat'><b id='gen'>--</b><small>Generation</small></div>"
"<div class='stat'><b id='up'>--</b><small>Uptime</small></div><div class='stat'><b id='sent'>--</b><small>Reports sent</small></div></section>"
"<footer>Local read-only monitor &middot; Connected Wi-Fi network</footer></main><script>"
"const names=['DJI','AT9S','T12','Xiaobawang'];function target(i,on){const e=document.getElementById('t'+i);e.classList.toggle('on',on);e.querySelector('i').textContent=on?'ACTIVE':'INACTIVE'}"
"function render(d){const alarm=d.mask!==0,s=document.getElementById('state');s.classList.toggle('alarm',alarm);document.getElementById('headline').textContent=alarm?'RF TARGET DETECTED':'AREA CLEAR';document.getElementById('event').textContent=d.event+' / mask '+d.mask;for(let i=0;i<4;i++)target(i,(d.mask&(1<<i))!==0);document.getElementById('gen').textContent=d.generation;document.getElementById('up').textContent=Math.floor(d.uptime_ms/1000)+' s';document.getElementById('sent').textContent=d.publish_successes;document.getElementById('net').textContent='STA '+(d.sta?'ON':'OFF')+' / MQTT '+(d.mqtt?'OK':'--')}"
"async function poll(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw 0;render(await r.json())}catch(e){document.getElementById('net').textContent='Connection lost'}}poll();setInterval(poll,1000);"
"</script></body></html>";

#endif
