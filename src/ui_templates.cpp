#include <Arduino.h>

extern const char UI_THEME_BASE_VARS[] PROGMEM = R"rawliteral(
  --bg:#121212;
  --text:#f2f2f2;
  --muted:#a8a8a8;
  --ok:#4caf50;
  --warn:#ffb74d;
  --bad:#ef5350;
  --accent:#80cbc4;
  --panel:#1d1d1d;
  --panel2:#262626;
  --line:#353535;
  --cell-low-bg:#181818;
  --cell-high-bg:#242424;
  --fill-low:#6e6e6e;
  --fill-mid:#9c9c9c;
  --fill-high:#d0d0d0;
)rawliteral";

extern const char UI_THEME_CUSTOM_VARS[] PROGMEM = "";

extern const char UI_SHARED_CSS[] PROGMEM = R"rawliteral(
  *{box-sizing:border-box}
    body{margin:0;font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:var(--text)}
  .muted{color:var(--muted)}
  .ok{color:var(--ok)} .warn{color:var(--warn)} .bad{color:var(--bad)}
  a{color:var(--accent);text-decoration:none}
)rawliteral";

extern const char UI_SHARED_JS[] PROGMEM = R"rawliteral(
  const byId = (id) => document.getElementById(id);
  const hex16 = (n) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(4, '0');
  const fmt = (v, d = 0) => Number(v).toFixed(d);
  const clamp = (v, min, max) => Math.max(min, Math.min(max, v));
  const formatUptime = (totalSeconds) => {
    const sec = Math.max(0, Math.floor(Number(totalSeconds || 0)));
    const days = Math.floor(sec / 86400);
    const hours = Math.floor((sec % 86400) / 3600);
    const minutes = Math.floor((sec % 3600) / 60);
    const seconds = sec % 60;
    return days + 'd ' + hours + 'h ' + minutes + 'm ' + seconds + 's';
  };
  const setText = (id, val) => {
    const el = byId(id);
    if (el) el.textContent = val;
  };
  function badgeForOnline(online) {
    return online ? { text: 'ONLINE', cls: 'ok' } : { text: 'OFFLINE', cls: 'bad' };
  }
  function deriveOperatingText(data) {
    const status = data.status || {};
    const pack = data.pack || {};
    const raw = status.raw || {};
    if (!status.online) return 'Offline';
    const currentA = Number(pack.current_a || 0);
    const soc = Number(pack.soc_pct || 0);
    const warning = Number(raw.warning || 0);
    const protect = Number(raw.protect || 0);
    const statusBits = Number(raw.status || 0);
    if (warning === 2 && protect === 4096 && statusBits === 0 && Math.abs(currentA) < 0.5 && soc >= 95) return 'Standby';
    if (currentA > 0.05) return 'Charging';
    if (currentA < -0.05) return 'Discharging';
    return status.text || status.operating_text || status.health_text || '-';
  }
  function extractCellSlots(data) {
    const cells = [];
    if (data.cells && Array.isArray(data.cells.voltages_v)) {
      data.cells.voltages_v.forEach((v, index) => {
        cells.push({ index: index + 1, v: Number(v || 0) });
      });
    }
    return cells;
  }
)rawliteral";

extern const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="uk">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BMS Dashboard</title>
  <style>
    __UI_SHARED_CSS__
    body{__UI_THEME_BASE_VARS__}
    __CUSTOM_CSS__
    .wrap{max-width:1280px;margin:0 auto;padding:20px}
    .hero{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;margin-bottom:18px}
    .hero-actions{display:flex;align-items:center;gap:14px;flex-wrap:wrap;justify-content:flex-end}
    h1{margin:0;font-size:28px}
    .sub{color:var(--muted);margin-top:6px}
    .pill{display:inline-block;padding:4px 10px;border-radius:999px;background:var(--panel2);border:1px solid var(--line);color:var(--muted);font-size:12px}
    .pill.live{color:var(--ok)}
    .pill.offline{color:var(--bad)}
    .grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}
    .card{grid-column:span 12;background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:16px}
    .half{grid-column:span 6}
    .stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}
    .stat{padding:14px;border-radius:14px;background:var(--panel2);border:1px solid var(--line)}
    #crc{font-size:.7em}
    .label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}
    .value{font-size:22px;font-weight:700;margin-top:6px}
    .row{display:flex;gap:12px;flex-wrap:wrap;align-items:center}
    .grid-cells{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:10px;margin-top:12px}
    .cell{padding:10px;border-radius:12px;background:var(--panel2);border:1px solid var(--line)}
    .cell.low{background:var(--cell-low-bg);border-color:var(--bad)}
    .cell.high{background:var(--cell-high-bg);border-color:var(--ok)}
    .cell .a{color:var(--muted);font-size:12px}
    .cell .v{font-weight:700;margin-top:6px}
    .bar{height:10px;background:var(--panel);border:1px solid var(--line);border-radius:999px;overflow:hidden}
    .fill{height:100%;background:var(--fill-mid)}
    .fill.low{background:var(--fill-low)}
    .fill.mid{background:var(--fill-mid)}
    .fill.high{background:var(--fill-high)}
    @media (max-width:1100px){.grid-cells{grid-template-columns:repeat(3,minmax(0,1fr))}}
    @media (max-width:900px){.half{grid-column:span 12}.stats{grid-template-columns:repeat(2,minmax(0,1fr))}.grid-cells{grid-template-columns:repeat(5,minmax(0,1fr))}}
    @media (max-width:650px){.grid-cells{grid-template-columns:repeat(3,minmax(0,1fr))}}
    @media (max-width:560px){.hero{flex-direction:column;align-items:flex-start}.hero-actions{justify-content:flex-start}.stats{grid-template-columns:1fr}}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <div>
        <div style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">
          <h1>BMS Dashboard</h1>
          <div class="pill" id="pollState">loading</div>
        </div>
        <div class="sub">Vision V-LFP48100</div>
      </div>
      <div class="hero-actions">
        __TOP_ACTION__
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="stats">
          <div class="stat"><div class="label">Wi-Fi</div><div class="value" id="wifiState">-</div><div class="muted" id="wifiIp">-</div></div>
          <div class="stat"><div class="label">Uptime</div><div class="value" id="uptime">-</div><div class="muted" id="lastUpdate">-</div></div>
          <div class="stat"><div class="label">Errors</div><div class="value" id="errors">-</div><div class="muted" id="canErrors">-</div><div class="muted" id="crc">-</div></div>
        </div>
      </div>

      <div class="card half">
        <div class="row">
          <div>
            <div class="value" id="statusText" style="margin-right:30px">-</div>
          </div>
          <div style="min-width:220px;flex:1">
            <div class="label" style="text-align:right">SOC: <strong id="socText" style="color:var(--text)">-</strong></div>
            <div class="bar"><div class="fill" id="socBar" style="width:0%"></div></div>
            <div class="label" style="text-align:right"><strong id="remainingAh" style="color:var(--text)">-</strong>Ah</div>
          </div>
        </div>
        <div class="row" style="margin-top:12px;font-size:1.5em;">
          <div>
            <span class="muted">Status:</span> <strong id="statusSummary">-</strong>
          </div>
        </div>
        <div class="row" style="margin-top:12px;font-size:1.3em;">
          <div><span class="muted">Voltage:</span> <strong id="packV">-</strong></div>
          <div><span class="muted">Current:</span> <strong id="currentA">-</strong></div>
        </div>
        <div class="row" style="margin-top:12px">
          <div><span class="muted">Health:</span> <strong id="healthText">-</strong></div>
          <div><span class="muted">SOH:</span> <strong id="soh">-</strong></div>
          <div><span class="muted">Cell count:</span> <strong id="cellCount">-</strong></div>
        </div>
        <div class="row" style="margin-top:12px">
          Temp:
          <div><span class="muted">PCB:</span> <strong id="pcbTemp">-</strong></div>
          <div><span class="muted">min:</span> <strong id="tempMin">-</strong></div>
          <div><span class="muted">max:</span> <strong id="tempMax">-</strong></div>
          <div><span class="muted">avg:</span> <strong id="tempAvg">-</strong></div>
        </div>
        <div class="row" style="margin-top:8px">
          <div><span class="muted">1:</span> <strong id="temp1">-</strong></div>
          <div><span class="muted">2:</span> <strong id="temp2">-</strong></div>
          <div><span class="muted">3:</span> <strong id="temp3">-</strong></div>
          <div><span class="muted">4:</span> <strong id="temp4">-</strong></div>
          <div><span class="muted">Raw:</span> <strong id="tempRaw">-</strong></div>
        </div>
        <div class="row" style="margin-top:8px">
          Cell:
          <div><span class="muted">min:</span> <strong id="cellMin">-</strong></div>
          <div><span class="muted">avg:</span> <strong id="cellAvg">-</strong></div>
          <div><span class="muted">max:</span> <strong id="cellMax">-</strong></div>
          <div><span class="muted">Delta:</span> <strong id="cellDelta">-</strong></div>
        </div>
      </div>

      <div class="card half">
        <div class="label">Cells</div>
        <div class="grid-cells" id="cellGrid"></div>
      </div>

    </div>
  </div>
  <script>
__UI_SHARED_JS__
    function renderCells(data, minV, maxV) {
      const grid = byId('cellGrid');
      grid.innerHTML = '';
      const cellSlots = extractCellSlots(data);
      cellSlots.forEach((slot) => {
        if (slot.v == null || slot.v <= 0) return;
        const v = Number(slot.v).toFixed(3);
        const el = document.createElement('div');
        el.className = 'cell';
        if (Math.abs(slot.v - minV) < 0.0005) el.classList.add('low');
        if (Math.abs(slot.v - maxV) < 0.0005) el.classList.add('high');
        el.innerHTML = '<div class="a">Cell ' + slot.index + '</div>' +
                       '<div class="v">' + v + ' V</div>';
        grid.appendChild(el);
      });
    }
    async function refresh() {
      try {
        const res = await fetch('/json', { cache: 'no-store' });
        const data = await res.json();
        const system = data.system || {};
        const status = data.status || {};
        const pack = data.pack || {};
        const temperatures = data.temperatures || {};
        const limits = data.limits || {};
        const cells = data.cells || {};
        const diagnostics = data.diagnostics || {};
        const debug = diagnostics.debug || {};
        const crc = diagnostics.crc || {};
        setText('pollState', 'live');
        byId('pollState').className = 'pill live';
        setText('wifiState', system.wifi.connected ? 'connected' : 'disconnected');
        byId('wifiState').className = 'value ' + (system.wifi.connected ? 'ok' : 'bad');
        setText('wifiIp', 'IP: ' + system.wifi.ip);
        setText('uptime', formatUptime(system.uptime_ms / 1000));
        setText('lastUpdate', system.last_update_ms ? ('last update: ' + Math.floor((system.uptime_ms - system.last_update_ms) / 1000) + ' s ago') : 'no data yet');
        setText('errors', 'BMS ' + system.errors);
        if (debug.can_started) {
          setText('canErrors', 'CAN ' + (debug.can_tx_fail ?? 0) + '/' + (debug.can_tx_error_counter ?? 0));
        } else {
          setText('canErrors', '');
        }
        setText('crc', 'CRC rx ' + hex16(crc.received || 0) + ' / calc ' + hex16(crc.calculated || 0));
        const s = badgeForOnline(status.online);
        setText('statusText', s.text);
        byId('statusText').className = 'value ' + s.cls;
        setText('statusSummary', status.text || '-');
        setText('healthText', status.health_text || '-');
        setText('packV', fmt(pack.voltage_v || 0, 2) + ' V');
        setText('currentA', fmt(pack.current_a || 0, 2) + ' A');
        setText('soh', (pack.soh_pct ?? '-') + '%');
        const tempUnit = '\u00B0C';
        setText('pcbTemp', (temperatures.pcb_c ?? '-') + ' ' + tempUnit);
        setText('tempMin', (temperatures.cell_min_c ?? '-') + ' ' + tempUnit);
        setText('tempMax', (temperatures.cell_max_c ?? '-') + ' ' + tempUnit);
        setText('tempAvg', (temperatures.temp_avg_c ?? '-') + ' ' + tempUnit);
        const cellTemps = temperatures.cell_sensors_c || [];
        const rawTemps = temperatures.raw_c || [];
        setText('temp1', (cellTemps[0] ?? '-') + ' ' + tempUnit);
        setText('temp2', (cellTemps[1] ?? '-') + ' ' + tempUnit);
        setText('temp3', (cellTemps[2] ?? '-') + ' ' + tempUnit);
        setText('temp4', (cellTemps[3] ?? '-') + ' ' + tempUnit);
        setText('tempRaw', rawTemps.filter((v) => v != null).map((v) => v + ' ' + tempUnit).join(', '));
        setText('remainingAh', limits.remaining_ah ?? '-');
        setText('cellMin', fmt(cells.min_v || 0, 3) + ' V');
        setText('cellAvg', fmt(cells.avg_v || 0, 3) + ' V');
        setText('cellMax', fmt(cells.max_v || 0, 3) + ' V');
        setText('cellDelta', fmt(cells.delta_v || 0, 3) + ' V');
        const cellSlots = extractCellSlots(data);
        const cellCount = Number(cells.count || cellSlots.length || 0);
        setText('cellCount', cellCount > 0 ? String(cellCount) : '-');
        const soc = clamp(Number(pack.soc_pct || 0), 0, 100);
        byId('socBar').style.width = soc + '%';
        byId('socBar').className = 'fill ' + (soc < 20 ? 'low' : soc < 80 ? 'mid' : 'high');
        setText('socText', soc + '%');
        renderCells(data, Number(cells.min_v || 0), Number(cells.max_v || 0));
      } catch (e) {
        setText('pollState', 'offline');
        byId('pollState').className = 'pill offline';
      }
    }
    refresh();
    setInterval(refresh, 1000);
  </script>
</body>
</html>
)rawliteral";

extern const char DIAG_HTML[] PROGMEM = R"rawliteral(
<html><head><meta charset='utf-8'>__REFRESH_META__
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
  __UI_SHARED_CSS__
  body{__UI_THEME_BASE_VARS__padding:18px}
  __CUSTOM_CSS__
  #clickable{cursor:pointer;margin-bottom:.5em}
  .wrap{max-width:1100px;margin:0 auto}
  .hero{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;margin-bottom:18px}
  h1{margin:0;font-size:30px}
  .card{margin:0 0 16px 0;padding:16px}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px;}
  .cell{padding:10px;border-radius:12px;background:var(--panel2);border:1px solid var(--line);}
  .reg-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(9em,1fr));gap:12px;margin-top:12px}
  .can-grid{grid-template-columns:repeat(auto-fill,minmax(18.5em,1fr))}
  .reg-card{padding:12px;border-radius:12px;background:var(--panel2);border:1px solid var(--line)}
  .can-card{padding:12px;border-radius:12px;background:var(--panel2);border:1px solid var(--line)}
  .reg-line{line-height:1.35}
  .can-row{display:flex;justify-content:space-between;gap:12px;align-items:baseline;line-height:1.35;margin-top:4px}
  .can-k{color:var(--muted);font-size:12px;white-space:nowrap}
  .can-v{font-size:12px;text-align:right;word-break:break-word}
  .reg-addr,.reg-hex,.reg-dec{word-break:break-all}
  .reg-addr,.reg-hex{font-family:monospace;color:var(--muted);font-size:12px;display:inline-block;margin-right:1em;}
  .reg-name{font-size:14px;font-weight:700;margin:4px 0;color:var(--text)}
  .reg-dec{font-size:12px;color:var(--muted)}
  .flag-group{margin-top:8px;padding:10px;border:1px solid var(--line);border-radius:10px;background:#151515;}
  .flag-title{color:var(--muted);font-size:11px;letter-spacing:.05em;text-transform:uppercase;margin-bottom:6px;}
  .tag{display:inline-block;margin:0 6px 6px 0;padding:4px 8px;border-radius:999px;font-size:12px;border:1px solid transparent;}
  .tag-muted{background:#202020;color:var(--muted);border-color:var(--line);}
  .tag-status{background:#1f2b3f;color:#dbe8ff;border-color:#35537a;}
  .tag-warn{background:#3d3415;color:#fff2ba;border-color:#7f6a20;}
  .tag-protect{background:#3c1818;color:#ffd0d0;border-color:#7f2e2e;}
  .soft{color:var(--muted);}
  .log-wrap{margin-top:12px;padding:12px;border:1px solid var(--line);border-radius:12px;background:#141414;font-family:Consolas,Monaco,'Courier New',monospace;font-size:12px;line-height:1.45;height:360px;min-height:220px;max-height:75vh;resize:vertical;box-sizing:border-box}
  .log-wrap.live{overflow-y:auto;overflow-x:hidden;scrollbar-gutter:stable}
  .log-wrap.paused{overflow:auto}
  .log-head{display:flex;flex-direction:column;gap:10px}
  .log-head h2{margin:0}
  .log-controls{display:flex;align-items:center;justify-content:flex-start;gap:8px;flex-wrap:nowrap;min-width:0;overflow-x:auto;overflow-y:hidden;padding-bottom:2px}
  #logLiveState{min-width:11ch;text-align:center;justify-content:center;display:inline-flex}
  .log-line{display:flex;gap:10px;align-items:flex-start;padding:5px 0;border-bottom:1px solid rgba(255,255,255,0.05);word-break:break-word}
  .log-line:last-child{border-bottom:0}
  .log-time{flex:0 0 auto;min-width:18ch;color:var(--muted);font-size:11px;letter-spacing:.02em}
  .log-tag{flex:0 0 auto;min-width:58px;text-align:center;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:700;letter-spacing:.04em;border:1px solid transparent}
  .log-msg{flex:1 1 auto;white-space:pre-wrap}
  .log-tag-bms{background:#183047;color:#cfe7ff;border-color:#316590}
  .log-tag-can{background:#17392f;color:#c7f5e2;border-color:#2c7a68}
  .log-tag-cfg{background:#332b17;color:#ffe6ad;border-color:#816520}
  .log-tag-boot{background:#2f233e;color:#eadcff;border-color:#6e5a8f}
  .log-tag-wifi{background:#142f2d;color:#c8f0ec;border-color:#2a736f}
  .log-tag-http{background:#262626;color:#e4e4e4;border-color:#555}
  .log-tag-ota{background:#3b1f2a;color:#ffd7e5;border-color:#8a425f}
  .log-tag-ap{background:#2b2a16;color:#f4efb0;border-color:#77712e}
  .log-tag-stat{background:#1d2d1d;color:#d7f0d7;border-color:#4f7b4f}
  .log-tag-debug{background:#202020;color:#d2d2d2;border-color:#555}
  .log-tag-gen{background:#262626;color:#dcdcdc;border-color:#555}
  .log-line-bad .log-msg{color:var(--bad)}
  .log-line-warn .log-msg{color:var(--warn)}
  .log-line-soft .log-msg{color:#cfcfcf}
  .log-line-rx .log-msg{color:#7dd3fc}
  .log-line-tx .log-msg{color:#f9a8d4}
  .log-btn{appearance:none;border:1px solid var(--line);background:var(--panel2);color:var(--text);padding:6px 12px;border-radius:999px;font:inherit;font-size:12px;cursor:pointer}
  .log-btn:hover{border-color:var(--accent)}
  .log-follow.active{background:#1d2d1d;border-color:#4f7b4f;color:#d7f0d7}
  .log-filter{appearance:none;border:1px solid var(--line);background:#222;color:var(--muted);padding:5px 10px;border-radius:999px;font:inherit;font-size:11px;cursor:pointer;white-space:nowrap;flex:0 0 auto}
  .log-filter.active{color:var(--text);box-shadow:inset 0 0 0 1px rgba(255,255,255,0.06)}
  .log-filter-bms.active{background:#183047;border-color:#316590;color:#cfe7ff}
  .log-filter-can.active{background:#17392f;border-color:#2c7a68;color:#c7f5e2}
  .log-filter-stat.active{background:#1d2d1d;border-color:#4f7b4f;color:#d7f0d7}
  .log-filter-cfg.active{background:#332b17;border-color:#816520;color:#ffe6ad}
  .log-filter-boot.active{background:#2f233e;border-color:#6e5a8f;color:#eadcff}
  .log-filter-wifi.active{background:#142f2d;border-color:#2a736f;color:#c8f0ec}
  .log-filter-http.active{background:#262626;border-color:#555;color:#e4e4e4}
  .log-filter-ota.active{background:#3b1f2a;border-color:#8a425f;color:#ffd7e5}
  .log-filter-ap.active{background:#2b2a16;border-color:#77712e;color:#f4efb0}
  .log-filter-dbg.active{background:#202020;border-color:#555;color:#d2d2d2}
  .log-filter-gen.active{background:#262626;border-color:#555;color:#dcdcdc}
  .log-filter-toggle.active{background:#43381a;border-color:#8f7a33;color:#fff2ba}
  .log-filter:hover{border-color:var(--accent)}
  @media (max-width:560px){.hero{flex-direction:column;align-items:flex-start}}
</style></head><body data-refresh-seconds='__REFRESH_SECONDS__'>
<div class='wrap'>
  <div class='hero'>
    <div>
      <h1>Diagnostics</h1>
    </div>
    <div><a href='/'>Back to dashboard</a></div>
  </div>
  <div class='card'>
    <div id='clickable' class='soft'>Auto refresh: __REFRESH_STATE__ | click to __REFRESH_ACTION__</div>
    <div>CPU temp: __CPU_TEMP__</div>
    <div>Active ID: __ACTIVE_ID__ | Last tried: __LAST_TRIED__</div>
    <div>Status: __ONLINE__ | Errors: __ERRORS__</div>
    <div>Operating state: __OPERATING__</div>
    <div>Health: <span class='__HEALTH_CLASS__'>__HEALTH_TEXT__</span></div>
    <div>Status: __STATUS_HEX__ <span class='muted'>(__STATUS_TEXT__)</span></div>
    <div>Warning: __WARNING_HEX__ <span class='muted'>(__WARNING_TEXT__)</span></div>
    <div>Protect: __PROTECT_HEX__ <span class='muted'>(__PROTECT_TEXT__)</span></div>
    <div>0x001A: __CYCLES__</div>
    <div>Cell count: __CELL_COUNT__</div>
    <div>Temp sensors: __DEBUG_TEMP_SENSOR_COUNT__ | Raw: <span class='muted'>__DEBUG_TEMP_SENSORS__</span></div>
    <div>CRC rx: __CRC_RX__ | CRC calc: __CRC_CALC__</div>
  </div>
  <div id='liveLogHost'><div class='card'><div class='soft'>Loading live log...</div></div></div>
  <div class='card' style='margin-top:16px'>
    <h2 style="margin:0">Debug</h2>
    <div>Polls: __DEBUG_POLL_COUNT__ | Success: __DEBUG_SUCCESS_COUNT__ | Fail: __DEBUG_FAIL_COUNT__</div>
    <div>Last fail: <span class='tag tag-muted'>__DEBUG_LAST_FAIL__</span></div>
    <div>Last poll: __DEBUG_LAST_POLL_MS__ ms | Last frame len: __DEBUG_LAST_FRAME_LEN__</div>
  </div>
  __DIAG_BLOCKS__
</div>
<script>
  const refreshSeconds = Number(document.body.dataset.refreshSeconds || 0);
  const toggleRefresh = () => {
    const url = new URL(window.location.href);
    if (refreshSeconds > 0) {
      url.searchParams.delete('upd');
    } else {
      url.searchParams.set('upd', '1');
    }
    window.location.href = url.toString();
  };
  document.getElementById('clickable').addEventListener('click', (event) => {
    if (event.target.closest('a, button, input, textarea, select, label')) return;
    toggleRefresh();
  });

  const liveLogHost = document.getElementById('liveLogHost');
  const logStateKey = 'bms.logPaused';
  const logFollowKey = 'bms.logFollow';
  const logFiltersKey = 'bms.logFilters';
  const autoRefreshEnabled = new URL(window.location.href).searchParams.has('upd');
  const allLogTags = ['BMS', 'CAN', 'STAT', 'CFG', 'BOOT', 'WIFI', 'HTTP', 'OTA', 'AP', 'D', 'GEN'];
  const defaultFilterState = Object.fromEntries(allLogTags.map((tag) => [tag, true]));
  let logPaused = localStorage.getItem(logStateKey) === '1';
  let logFollow = localStorage.getItem(logFollowKey);
  logFollow = logFollow === null ? true : logFollow === '1';
  let logFilters = (() => {
    try {
      const parsed = JSON.parse(localStorage.getItem(logFiltersKey) || 'null');
      if (parsed && typeof parsed === 'object') {
        return { ...defaultFilterState, ...parsed };
      }
    } catch (e) {}
    return { ...defaultFilterState };
  })();
  let logSource = null;

  const logTagClassForName = (tag) => {
    const upper = String(tag || 'GEN').toUpperCase();
    if (upper === 'BMS') return 'log-tag-bms';
    if (upper === 'CAN') return 'log-tag-can';
    if (upper === 'CFG') return 'log-tag-cfg';
    if (upper === 'BOOT') return 'log-tag-boot';
    if (upper === 'WIFI') return 'log-tag-wifi';
    if (upper === 'HTTP') return 'log-tag-http';
    if (upper === 'OTA') return 'log-tag-ota';
    if (upper === 'AP') return 'log-tag-ap';
    if (upper === 'STAT') return 'log-tag-stat';
    if (upper === 'D') return 'log-tag-debug';
    return 'log-tag-gen';
  };

  const inferLogToneClass = (line) => {
    const lower = String(line || '').toLowerCase();
    const hasPositiveCounter = (key) => {
      const pos = lower.indexOf(key);
      if (pos < 0) return false;
      let idx = pos + key.length;
      while (idx < lower.length && lower[idx] === ' ') idx++;
      if (idx >= lower.length) return false;
      if (!/\d/.test(lower[idx])) return true;
      return lower[idx] !== '0';
    };
    if (lower.includes('error') || lower.includes('timeout') || lower.includes('offline') || lower.includes('bus-off') || lower.includes('failed') || hasPositiveCounter('err=') || hasPositiveCounter('fail=')) {
      return 'log-line-bad';
    }
    if (lower.includes('warn') || lower.includes('disabled') || lower.includes('fallback')) {
      return 'log-line-warn';
    }
    return 'log-line-soft';
  };

  const inferLogDirectionClass = (line) => {
    const lower = String(line || '').toLowerCase();
    const trimmed = lower.trimStart();
    if (trimmed.startsWith('rx ') || trimmed.startsWith('rx:')) return 'log-line-rx';
    if (trimmed.startsWith('tx ') || trimmed.startsWith('tx:')) return 'log-line-tx';
    return '';
  };

  const parseLogLine = (raw) => {
    const line = String(raw || '');
    let time = '';
    let payload = line;
    const sep = line.indexOf('|');
    if (sep > 0) {
      const prefix = line.slice(0, sep);
      if (/^\d+$/.test(prefix)) {
        time = formatElapsed(prefix);
        payload = line.slice(sep + 1);
      }
    }
    let tag = 'GEN';
    let message = payload;
    if (payload.startsWith('[')) {
      const end = payload.indexOf(']');
      if (end > 1) {
        tag = payload.slice(1, end);
        message = payload.slice(end + 1).trimStart();
      }
    }
    const el = document.createElement('div');
    el.className = 'log-line ' + inferLogToneClass(payload) + ' ' + inferLogDirectionClass(message || payload);
    el.dataset.tag = tag.toUpperCase();

    const timeEl = document.createElement('span');
    timeEl.className = 'log-time';
    timeEl.textContent = time || '';

    const tagEl = document.createElement('span');
    tagEl.className = 'log-tag ' + logTagClassForName(tag);
    tagEl.textContent = tag;

    const msgEl = document.createElement('span');
    msgEl.className = 'log-msg';
    msgEl.textContent = message;

    el.appendChild(timeEl);
    el.appendChild(tagEl);
    el.appendChild(msgEl);
    return el;
  };

  const formatElapsed = (value) => {
    const totalMs = Math.max(0, Math.floor(Number(value || 0)));
    const totalSeconds = Math.floor(totalMs / 1000);
    const millis = totalMs % 1000;
    const hours = Math.floor(totalSeconds / 3600);
    const rem = totalSeconds % 3600;
    const minutes = Math.floor(rem / 60);
    const seconds = rem % 60;
    return `+${String(hours).padStart(5, '0')}h ${String(minutes).padStart(2, '0')}m ${String(seconds).padStart(2, '0')}.${String(millis).padStart(3, '0')}s`;
  };

  const logDomLimit = Number(__LOG_DOM_LIMIT__ || 1000);
  const trimLogEntries = () => {
    if (!liveLogHost) return;
    const wrap = liveLogHost.querySelector('.log-wrap');
    if (!wrap) return;
    while (wrap.querySelectorAll('.log-line').length > logDomLimit) {
      const lastLine = wrap.querySelector('.log-line:last-of-type');
      if (!lastLine) break;
      lastLine.remove();
    }
  };

  const isAnyFilterEnabled = () => allLogTags.some((tag) => logFilters[tag] !== false);
  const isAllFilterEnabled = () => allLogTags.every((tag) => logFilters[tag] !== false);

  const updateFilterButtons = () => {
    if (!liveLogHost) return;
    const hideAllButton = liveLogHost.querySelector('#logHideAll');
    if (hideAllButton) {
      const anyVisible = isAnyFilterEnabled();
      const allVisible = isAllFilterEnabled();
      hideAllButton.textContent = anyVisible ? 'Hide all' : 'Show all';
      hideAllButton.className = 'log-filter log-filter-toggle' + (!allVisible ? ' active' : '');
    }
    liveLogHost.querySelectorAll('[data-filter-tag]').forEach((btn) => {
      const tag = (btn.dataset.filterTag || 'GEN').toUpperCase();
      const enabled = logFilters[tag] !== false;
      btn.className = 'log-filter ' + logTagClassForName(tag).replace('log-tag-', 'log-filter-') + (enabled ? ' active' : '');
    });
  };

  const applyLogFilters = () => {
    if (!liveLogHost) return;
    liveLogHost.querySelectorAll('.log-line').forEach((line) => {
      const tag = (line.dataset.tag || 'GEN').toUpperCase();
      line.style.display = logFilters[tag] !== false ? '' : 'none';
    });
  };

  const syncLiveScroll = (wrap, beforeScrollTop, beforeScrollHeight) => {
    if (logFollow) {
      wrap.scrollTop = 0;
      return;
    }
    const delta = wrap.scrollHeight - beforeScrollHeight;
    const nearTop = beforeScrollTop <= 24;
    wrap.scrollTop = nearTop ? 0 : Math.max(0, beforeScrollTop + delta);
  };

  const setLogUi = () => {
    const button = document.getElementById('logToggle');
    const followButton = document.getElementById('logFollowToggle');
    const state = document.getElementById('logLiveState');
    const logWrap = liveLogHost ? liveLogHost.querySelector('.log-wrap') : null;
    const controls = liveLogHost ? liveLogHost.querySelector('.log-controls') : null;
    if (button) {
      button.textContent = logPaused ? 'Resume' : 'Pause';
      button.style.display = autoRefreshEnabled ? 'none' : '';
    }
    if (followButton) {
      followButton.textContent = logFollow ? 'Follow live' : 'Browse';
      followButton.className = 'log-btn log-follow' + (logFollow ? ' active' : '');
      followButton.style.display = autoRefreshEnabled ? 'none' : '';
    }
    if (state) {
      state.style.display = autoRefreshEnabled ? 'none' : '';
      state.textContent = logPaused ? 'paused' : 'live';
      state.className = 'tag ' + (logPaused ? 'tag-warn' : 'tag-status');
    }
    if (controls) {
      controls.style.display = autoRefreshEnabled ? 'none' : '';
    }
    if (logWrap) {
      logWrap.classList.toggle('live', !logPaused);
      logWrap.classList.toggle('paused', logPaused);
    }
    updateFilterButtons();
    applyLogFilters();
  };

  const closeLogStream = () => {
    if (logSource) {
      logSource.close();
      logSource = null;
    }
  };

  const openLogStream = () => {
    if (autoRefreshEnabled || logPaused || !window.EventSource || logSource) return;
    closeLogStream();
    try {
      logSource = new EventSource('/log-stream');
      logSource.onopen = () => {
        const state = document.getElementById('logLiveState');
        if (state) {
          state.textContent = 'live';
          state.className = 'tag tag-status';
          state.style.display = autoRefreshEnabled ? 'none' : '';
        }
      };
      logSource.addEventListener('log', (event) => {
        if (logPaused || !liveLogHost) return;
        const wrap = liveLogHost.querySelector('.log-wrap');
        if (!wrap) return;
        const beforeScrollTop = wrap.scrollTop;
        const beforeScrollHeight = wrap.scrollHeight;
        const empty = wrap.querySelector('.soft');
        if (empty) empty.remove();
        wrap.insertBefore(parseLogLine(event.data), wrap.firstChild);
        trimLogEntries();
        syncLiveScroll(wrap, beforeScrollTop, beforeScrollHeight);
        applyLogFilters();
      });
      logSource.onerror = () => {
        if (autoRefreshEnabled || logPaused) return;
        const state = document.getElementById('logLiveState');
        if (state) {
          state.textContent = 'reconnecting';
          state.className = 'tag tag-warn';
          state.style.display = autoRefreshEnabled ? 'none' : '';
        }
      };
    } catch (err) {
      closeLogStream();
    }
  };

  if (liveLogHost) {
    liveLogHost.addEventListener('click', (event) => {
      const target = event.target.closest('button');
      if (!target) return;
      if (target.id === 'logToggle') {
        logPaused = !logPaused;
        localStorage.setItem(logStateKey, logPaused ? '1' : '0');
        setLogUi();
        if (logPaused) {
          closeLogStream();
        } else {
          openLogStream();
        }
        return;
      }
      if (target.id === 'logHideAll') {
        const shouldShowAll = !isAnyFilterEnabled();
        allLogTags.forEach((tag) => {
          logFilters[tag] = shouldShowAll;
        });
        localStorage.setItem(logFiltersKey, JSON.stringify(logFilters));
        setLogUi();
        return;
      }
      if (target.id === 'logFollowToggle') {
        logFollow = !logFollow;
        localStorage.setItem(logFollowKey, logFollow ? '1' : '0');
        setLogUi();
        if (logFollow && !logPaused) {
          const wrap = liveLogHost.querySelector('.log-wrap');
          if (wrap) {
            wrap.scrollTop = 0;
          }
        }
        return;
      }
      const tag = target.dataset.filterTag;
      if (!tag) return;
      const upper = tag.toUpperCase();
      logFilters[upper] = logFilters[upper] === false;
      localStorage.setItem(logFiltersKey, JSON.stringify(logFilters));
      setLogUi();
    });
  }

  const startLiveLog = () => {
    const mountLog = () => {
      setLogUi();
      trimLogEntries();
      if (autoRefreshEnabled) {
        return;
      }
      openLogStream();
    };
    if (!liveLogHost) {
      return;
    }
    fetch('/log', { cache: 'no-store' })
      .then((response) => response.text())
      .then((html) => {
        liveLogHost.innerHTML = html;
        mountLog();
      })
      .catch(() => {
        liveLogHost.innerHTML = "<div class='card'><div class='soft'>Failed to load live log.</div></div>";
      });
  };

  startLiveLog();
</script>
</body></html>
)rawliteral";

extern const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang='uk'>
<head>
  <meta charset='utf-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>BMS Setup</title>
  <style>
    __UI_SHARED_CSS__
    body{__UI_THEME_BASE_VARS__padding:18px}
    __CUSTOM_CSS__
    .wrap{max-width:1100px;margin:0 auto}
    .hero{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;margin-bottom:18px}
    h1{margin:0;font-size:30px}
    .sub{color:var(--muted);margin-top:6px}
    .card{padding:18px}
    .banner{margin-bottom:16px;padding:12px 14px;border:1px solid var(--line);border-radius:12px;background:rgba(255,255,255,0.03)}
    .grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}
    .section{grid-column:span 12;padding:16px;border:1px solid var(--line);border-radius:16px;background:rgba(255,255,255,0.02)}
    .section-title{margin:0 0 14px 0;font-size:14px;letter-spacing:.08em;text-transform:uppercase;color:var(--accent)}
    .section-grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}
    .field{grid-column:span 6;display:flex;flex-direction:column;gap:8px}
    .field.third{grid-column:span 4}
    .field.half{grid-column:span 6}
    .field.full{grid-column:span 12}
    .field.toggle{justify-content:flex-start}
    label{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted)}
    input[type=text],input[type=password],input[type=number],input[type=file],textarea{
      width:100%;padding:12px 14px;border-radius:12px;border:1px solid var(--line);
      background:var(--panel2);color:var(--text);font:inherit;outline:none;
    }
    textarea{min-height:160px;resize:vertical;font-family:monospace}
    .check{display:flex;align-items:center;gap:10px;padding:12px 14px;border-radius:12px;border:1px solid var(--line);background:var(--panel2);min-height:48px}
    .firmware-row{display:flex;gap:12px;align-items:flex-end;flex-wrap:wrap}
    .firmware-row .file-input{flex:1 1 320px}
    .firmware-row .upload-btn{white-space:nowrap}
    .actions{display:flex;gap:12px;flex-wrap:wrap;margin-top:18px}
    .footer-actions{display:flex;gap:12px;flex-wrap:wrap;justify-content:flex-start;margin-top:18px}
    .btn{appearance:none;border:1px solid var(--line);background:var(--accent);color:#0f1112;padding:12px 18px;border-radius:12px;font:inherit;font-weight:700;cursor:pointer}
    .btn.secondary{background:transparent;color:var(--text)}
    .note{font-size:12px;color:var(--muted);margin-top:4px}
    @media (max-width:900px){.field,.field.third,.field.half{grid-column:span 12}}
  </style>
</head>
<body>
  <div class='wrap'>
    <div class='hero'>
      <div>
        <h1>BMS Setup</h1>
      </div>
      __TOP_ACTION__
    </div>

    <div class='banner'>__STATUS__</div>

    <form method='post' action='/save'>
      <div class='card'>
        <div class='grid'>
          <section class='section'>
            <h2 class='section-title'>WiFi</h2>
            <div class='section-grid'>
              <div class='field third'>
                <label for='wifi_ssid'>Wi-Fi SSID</label>
                <input id='wifi_ssid' name='wifi_ssid' type='text' value='__WIFI_SSID__' autocomplete='off'>
              </div>
              <div class='field third'>
                <label for='wifi_password'>Wi-Fi password</label>
                <input id='wifi_password' name='wifi_password' type='password' value='' autocomplete='new-password' placeholder='leave blank to keep current'>
                <div class='note'>Empty password keeps the stored one.</div>
              </div>
              <div class='field third'>
                <label for='wifi_hidden'>Search hidden networks</label>
                <div class='check'>
                  <input id='wifi_hidden' name='wifi_hidden' type='checkbox' __WIFI_HIDDEN_CHECKED__>
                  <span class='note' style='margin:0'>Scan hidden SSIDs</span>
                </div>
              </div>
            </div>
          </section>

          <section class='section'>
            <h2 class='section-title'>BMS</h2>
            <div class='section-grid'>
              <div class='field third'>
                <label for='bms_de_pin'>BMS DE pin</label>
                <input id='bms_de_pin' name='bms_de_pin' type='number' min='0' max='39' value='__BMS_DE_PIN__'>
              </div>
              <div class='field third'>
                <label for='bms_rx_pin'>BMS RX pin</label>
                <input id='bms_rx_pin' name='bms_rx_pin' type='number' min='0' max='39' value='__BMS_RX_PIN__'>
              </div>
              <div class='field third'>
                <label for='bms_tx_pin'>BMS TX pin</label>
                <input id='bms_tx_pin' name='bms_tx_pin' type='number' min='0' max='39' value='__BMS_TX_PIN__'>
              </div>
            </div>
          </section>

          <section class='section'>
            <h2 class='section-title'>BMS ID</h2>
            <div class='section-grid'>
              <div class='field third'>
                <label for='preferred_bms_id'>Preferred BMS ID</label>
                <input id='preferred_bms_id' name='preferred_bms_id' type='number' min='1' max='16' value='__PREFERRED_BMS_ID__'>
              </div>
              <div class='field third'>
                <label for='bms_id_min'>BMS ID min</label>
                <input id='bms_id_min' name='bms_id_min' type='number' min='1' max='16' value='__BMS_ID_MIN__'>
              </div>
              <div class='field third'>
                <label for='bms_id_max'>BMS ID max</label>
                <input id='bms_id_max' name='bms_id_max' type='number' min='1' max='16' value='__BMS_ID_MAX__'>
              </div>
            </div>
          </section>

          <section class='section'>
            <h2 class='section-title'>CAN</h2>
            <div class='section-grid'>
              <div class='field third toggle'>
                <label for='enable_deye_can'>Enable Deye CAN</label>
                <div class='check'>
                  <input id='enable_deye_can' name='enable_deye_can' type='checkbox' __ENABLE_CAN_CHECKED__>
                  <span class='note' style='margin:0'>Enable CAN output to Deye</span>
                </div>
              </div>
              <div class='field third can-pin'>
                <label for='can_rx_pin'>CAN RX pin</label>
                <input id='can_rx_pin' name='can_rx_pin' type='number' min='0' max='39' value='__CAN_RX_PIN__'>
              </div>
              <div class='field third can-pin'>
                <label for='can_tx_pin'>CAN TX pin</label>
                <input id='can_tx_pin' name='can_tx_pin' type='number' min='0' max='39' value='__CAN_TX_PIN__'>
              </div>
            </div>
          </section>

          <section class='section'>
            <h2 class='section-title'>CSS</h2>
            <div class='section-grid'>
              <div class='field full'>
                <label for='custom_css'>Custom CSS</label>
                <textarea id='custom_css' name='custom_css' placeholder='Add extra CSS rules here...'>__CUSTOM_CSS_TEXT__</textarea>
              </div>
            </div>
          </section>

          <section class='section'>
            <h2 class='section-title'>Logs</h2>
            <div class='section-grid'>
              <div class='field third'>
                <label for='log_buffer_size'>Device log buffer</label>
                <input id='log_buffer_size' name='log_buffer_size' type='number' min='1' max='50' value='__LOG_BUFFER_SIZE__'>
                <div class='note'>How many log entries are kept in RAM on the device.</div>
              </div>
              <div class='field third'>
                <label for='log_dom_limit'>Browser log limit</label>
                <input id='log_dom_limit' name='log_dom_limit' type='number' min='1' max='5000' value='__LOG_DOM_LIMIT__'>
                <div class='note'>How many rendered rows the browser keeps before trimming old ones.</div>
              </div>
            </div>
          </section>
        </div>
        <div class='footer-actions'>
          <button class='btn' type='submit'>Save & reboot</button>
          <button class='btn secondary' type='submit' formaction='/reboot' formmethod='post'>Reboot</button>
          <button class='btn secondary' type='submit' formaction='/reset' formmethod='post'>Reset config</button>
        </div>
      </div>
    </form>

    <form method='post' action='/update' enctype='multipart/form-data' style='margin-top:16px'>
      <div class='card'>
        <div class='grid'>
          <section class='section'>
            <h2 class='section-title'>Firmware update</h2>
            <div class='section-grid'>
              <div class='field full'>
                <label for='firmware_file'>Binary file</label>
                <div class='firmware-row'>
                  <input class='file-input' id='firmware_file' name='firmware_file' type='file' accept='.bin,application/octet-stream'>
                  <button class='btn upload-btn' type='submit'>Upload firmware</button>
                </div>
                <div class='note'>Device will reboot automatically after a successful upload.</div>
              </div>
            </div>
          </section>
        </div>
      </div>
    </form>
  </div>
  <script>
    const canToggle = document.getElementById('enable_deye_can');
    const canPins = document.querySelectorAll('.can-pin');
    const updateCanPins = () => {
      const show = canToggle && canToggle.checked;
      canPins.forEach((el) => {
        el.style.display = show ? '' : 'none';
      });
    };
    if (canToggle) {
      canToggle.addEventListener('change', updateCanPins);
      updateCanPins();
    }
  </script>
</body>
</html>
)rawliteral";
