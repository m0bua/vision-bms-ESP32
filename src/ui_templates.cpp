#include <Arduino.h>
#include "ui_theme.h"

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
    body{__UI_THEME_BASE_VARS____UI_THEME_CUSTOM_VARS__}
    .wrap{max-width:1280px;margin:0 auto;padding:20px}
    .hero{display:flex;justify-content:space-between;align-items:flex-end;gap:16px;margin-bottom:18px}
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
    @media (max-width:560px){.hero{flex-direction:column;align-items:flex-start}.stats{grid-template-columns:1fr}}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <div>
        <h1>BMS Dashboard</h1>
        <div class="sub">Vision V-LFP48100</div>
      </div>
      <div class="pill" id="pollState">loading</div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="stats">
          <div class="stat"><div class="label">Wi-Fi</div><div class="value" id="wifiState">-</div><div class="muted" id="wifiIp">-</div></div>
          <div class="stat"><div class="label">Uptime</div><div class="value" id="uptime">-</div><div class="muted" id="lastUpdate">-</div></div>
          <div class="stat"><div class="label">Errors</div><div class="value" id="errors">-</div><div class="muted" id="crc">-</div></div>
        </div>
      </div>

      <div class="card half">
        <div class="row">
          <div>
            <div class="value" id="statusText" style="margin-right:30px">-</div>
          </div>
          <div style="min-width:220px;flex:1">
            <div class="label" style="text-align:right">SOC: <strong id="socText">-</strong></div>
            <div class="bar"><div class="fill" id="socBar" style="width:0%"></div></div>
            <div class="label" style="text-align:right"><strong id="remainingAh">-</strong>Ah</div>
          </div>
        </div>
        <div class="row" style="margin-top:12px;font-size:1.5em;">
          <div>
            <div class="muted">Status: <strong id="healthText">-</strong></div>
          </div>
        </div>
        <div class="row" style="margin-top:12px;font-size:1.3em;">
          <div><span class="muted">Voltage:</span> <strong id="packV">-</strong></div>
          <div><span class="muted">Current:</span> <strong id="currentA">-</strong></div>
        </div>
        <div class="row" style="margin-top:12px">
          <div><span class="muted">SOH:</span> <strong id="soh">-</strong></div>
          <div><span class="muted">Cell count:</span> <strong id="cellCount">-</strong></div>
        </div>
        <div class="row" style="margin-top:12px">
          <div><span class="muted">PCB:</span> <strong id="pcbTemp">-</strong></div>
          <div><span class="muted">Cell min:</span> <strong id="cellMinTemp">-</strong></div>
          <div><span class="muted">Cell max:</span> <strong id="cellMaxTemp">-</strong></div>
        </div>
        <div class="row" style="margin-top:8px">
          <div><span class="muted">Min:</span> <strong id="cellMin">-</strong></div>
          <div><span class="muted">Avg:</span> <strong id="cellAvg">-</strong></div>
          <div><span class="muted">Max:</span> <strong id="cellMax">-</strong></div>
          <div><span class="muted">Delta:</span> <strong id="cellDelta">-</strong></div>
        </div>
      </div>

      <div class="card half">
        <div class="label">Cells</div>
        <div class="grid-cells" id="cellGrid"></div>
      </div>

      <div class="card">
        <div class="row" style="justify-content:space-between">
          <div class="label">Links</div>
          <div class="row">
            <a href="/json">JSON</a>
            <a href="/diag">Diagnostic</a>
          </div>
        </div>
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
        const crc = diagnostics.crc || {};
        setText('pollState', 'live');
        byId('pollState').className = 'pill live';
        setText('wifiState', system.wifi.connected ? 'connected' : 'disconnected');
        byId('wifiState').className = 'value ' + (system.wifi.connected ? 'ok' : 'bad');
        setText('wifiIp', 'IP: ' + system.wifi.ip);
        setText('uptime', formatUptime(system.uptime_ms / 1000));
        setText('lastUpdate', system.last_update_ms ? ('last update: ' + Math.floor((system.uptime_ms - system.last_update_ms) / 1000) + ' s ago') : 'no data yet');
        setText('errors', system.errors);
        setText('crc', 'CRC rx ' + hex16(crc.received || 0) + ' / calc ' + hex16(crc.calculated || 0));
        const s = badgeForOnline(status.online);
        setText('statusText', s.text);
        byId('statusText').className = 'value ' + s.cls;
        setText('healthText', status.text || '-');
        setText('packV', fmt(pack.voltage_v || 0, 2) + ' V');
        setText('currentA', fmt(pack.current_a || 0, 2) + ' A');
        setText('soh', (pack.soh_pct ?? '-') + '%');
        setText('pcbTemp', (temperatures.pcb_c ?? '-') + ' C');
        setText('cellMinTemp', (temperatures.cell_min_c ?? '-') + ' C');
        setText('cellMaxTemp', (temperatures.cell_max_c ?? '-') + ' C');
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
  body{__UI_THEME_BASE_VARS____UI_THEME_CUSTOM_VARS__padding:18px}
  #clickable{cursor:pointer;margin-bottom:.5em}
  .card{max-width:1200px;margin:0 auto 16px auto;padding:16px}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px;}
  .cell{padding:10px;border-radius:12px;background:var(--panel2);border:1px solid var(--line);}
  .reg-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(9em,1fr));gap:12px;margin-top:12px}
  .reg-card{padding:12px;border-radius:12px;background:var(--panel2);border:1px solid var(--line)}
  .reg-line{line-height:1.35}
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
</style></head><body data-refresh-seconds='__REFRESH_SECONDS__'>
<div class='card'>
  <h1>Diagnostics</h1>
  <div><a href='/'>Back to dashboard</a></div><br>
  <div id='clickable' class='soft'>Auto refresh: __REFRESH_STATE__ | click to __REFRESH_ACTION__</div>
  <div>Active ID: __ACTIVE_ID__ | Last tried: __LAST_TRIED__</div>
  <div>Status: __ONLINE__ | Errors: __ERRORS__</div>
  <div>Operating state: __OPERATING__</div>
  <div>Health: <span class='__HEALTH_CLASS__'>__HEALTH_TEXT__</span></div>
  <div>Status: __STATUS_HEX__ <span class='muted'>(__STATUS_TEXT__)</span></div>
  <div>Warning: __WARNING_HEX__ <span class='muted'>(__WARNING_TEXT__)</span></div>
  <div>Protect: __PROTECT_HEX__ <span class='muted'>(__PROTECT_TEXT__)</span></div>
  <div>Cycles: __CYCLES__</div>
  <div>Cell count: __CELL_COUNT__</div>
  <div>CRC rx: __CRC_RX__ | CRC calc: __CRC_CALC__</div>
  <div style='margin-top:12px;padding-top:12px;border-top:1px solid var(--line)'>
    <div class='flag-title'>Debug</div>
    <div>Polls: __DEBUG_POLL_COUNT__ | Success: __DEBUG_SUCCESS_COUNT__ | Fail: __DEBUG_FAIL_COUNT__</div>
    <div>Last fail: <span class='tag tag-muted'>__DEBUG_LAST_FAIL__</span></div>
    <div>Last poll: __DEBUG_LAST_POLL_MS__ ms | Last frame len: __DEBUG_LAST_FRAME_LEN__</div>
    <div>CAN init: __DEBUG_CAN_INIT__ | CAN start: __DEBUG_CAN_START__ | CAN state: __DEBUG_CAN_STATUS__</div>
    <div>CAN TWAI state: __DEBUG_CAN_STATE__ | TX err: __DEBUG_CAN_TX_ERR__ | RX err: __DEBUG_CAN_RX_ERR__</div>
    <div>CAN bus err: __DEBUG_CAN_BUS_ERR__ | Arb lost: __DEBUG_CAN_ARB_LOST__ | Queue: __DEBUG_CAN_QUEUE__</div>
    <div>CAN tx: __DEBUG_CAN_TX__ | CAN tx fail: __DEBUG_CAN_TX_FAIL__</div>
    <div style='margin-top:6px;font-family:monospace;word-break:break-all'>Last frame: __DEBUG_LAST_FRAME__</div>
  </div>
</div>
__RAW_REGS__
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
</script>
</body></html>
)rawliteral";
