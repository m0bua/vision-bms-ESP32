#include <Arduino.h>
#include "ui_theme.h"

const char UI_SHARED_CSS[] PROGMEM = R"rawliteral(
  *{box-sizing:border-box}
    body{margin:0;font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:var(--text)}
  .muted{color:var(--muted)}
  .ok{color:var(--ok)} .warn{color:var(--warn)} .bad{color:var(--bad)}
  a{color:var(--accent);text-decoration:none}
)rawliteral";

const char UI_SHARED_JS[] PROGMEM = R"rawliteral(
  const byId = (id) => document.getElementById(id);
  const hex16 = (n) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(4, '0');
  const fmt = (v, d = 0) => Number(v).toFixed(d);
  const clamp = (v, min, max) => Math.max(min, Math.min(max, v));
  const setText = (id, val) => {
    const el = byId(id);
    if (el) el.textContent = val;
  };
  function badgeForOnline(online) {
    return online ? { text: 'ONLINE', cls: 'ok' } : { text: 'OFFLINE', cls: 'bad' };
  }
  function deriveOperatingText(data) {
    if (!data.online) return 'Offline';
    const currentA = Number(data.current_a || 0);
    const soc = Number(data.soc || 0);
    const warning = Number(data.warning_raw || 0);
    const protect = Number(data.protect_raw || 0);
    const status = Number(data.status_raw || 0);
    if (warning === 2 && protect === 4096 && status === 0 && Math.abs(currentA) < 0.5 && soc >= 95) return 'Standby';
    if (currentA > 0.05) return 'Charging';
    if (currentA < -0.05) return 'Discharging';
    return data.operating_text || data.health_text || '-';
  }
  function extractCellSlots(data) {
    const cells = [];
    if (Array.isArray(data.cells) && data.cells.length) {
      data.cells.forEach((cell, index) => {
        if (typeof cell === 'number') {
          cells.push({ index: index + 1, mv: Number(cell || 0) });
          return;
        }
        if (cell && typeof cell === 'object') {
          if (cell.mv != null) {
            cells.push({ index: Number(cell.index || index + 1), mv: Number(cell.mv || 0) });
            return;
          }
          if (cell.v != null) {
            cells.push({ index: Number(cell.index || index + 1), mv: Number(cell.v || 0) * 1000 });
            return;
          }
        }
        cells.push({ index: index + 1, mv: 0 });
      });
    } else if (Array.isArray(data.cell_voltages_mv) && data.cell_voltages_mv.length) {
      data.cell_voltages_mv.forEach((mv, index) => cells.push({ index: index + 1, mv: Number(mv || 0) }));
    } else if (Array.isArray(data.raw_regs) && data.raw_regs.length >= 18) {
      data.raw_regs.slice(2, 18).forEach((reg, index) => cells.push({ index: index + 1, mv: Number(reg || 0) }));
    } else {
      for (let i = 1; i <= 16; i++) {
        const key = 'cell_' + i + '_mv';
        if (data[key] != null) cells.push({ index: i, mv: Number(data[key] || 0) });
      }
    }
    return cells;
  }
)rawliteral";

const char INDEX_HTML[] PROGMEM = R"rawliteral(
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
    @media (max-width:1100px){.grid-cells{grid-template-columns:repeat(4,minmax(0,1fr))}}
    @media (max-width:900px){.half{grid-column:span 12}.stats{grid-template-columns:repeat(2,minmax(0,1fr))}.grid-cells{grid-template-columns:repeat(3,minmax(0,1fr))}}
    @media (max-width:650px){.grid-cells{grid-template-columns:repeat(2,minmax(0,1fr))}}
    @media (max-width:560px){.hero{flex-direction:column;align-items:flex-start}.stats{grid-template-columns:1fr}.grid-cells{grid-template-columns:1fr}}
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
            <div class="label">Status</div>
            <div class="value" id="statusText">-</div>
            <div class="muted" style="margin-top:4px">Substatus: <strong id="substatusText">-</strong></div>
          </div>
          <div style="min-width:220px;flex:1">
            <div class="label">SOC</div>
            <div class="bar"><div class="fill" id="socBar" style="width:0%"></div></div>
            <div class="muted" id="socText" style="margin-top:6px">-</div>
          </div>
        </div>
        <div class="row" style="margin-top:12px">
          <div><span class="muted">Voltage:</span> <strong id="packV">-</strong></div>
          <div><span class="muted">Current:</span> <strong id="currentA">-</strong></div>
          <div><span class="muted">SOH:</span> <strong id="soh">-</strong></div>
        </div>
        <div class="row" style="margin-top:12px">
          <div><span class="muted">Temp 1:</span> <strong id="temp1">-</strong></div>
          <div><span class="muted">Temp 2:</span> <strong id="temp2">-</strong></div>
          <div><span class="muted">Temp 3:</span> <strong id="temp3">-</strong></div>
        </div>
        <div class="row" style="margin-top:12px">
          <div><span class="muted">Remaining Ah:</span> <strong id="remainingAh">-</strong></div>
        </div>
        <div class="row">
          <div><span class="muted">BMS ID:</span> <strong id="slaveId">-</strong></div>
          <div><span class="muted">last tried:</span> <strong id="scanId">-</strong></div>
        </div>
        <div style="margin-top:12px">
          <div class="label">Cell summary</div>
          <div class="row" style="margin-top:10px">
            <div><span class="muted">Cell count:</span> <strong id="cellCount">-</strong></div>
            <div><span class="muted">State:</span> <strong id="healthText">-</strong></div>
          </div>
          <div class="row" style="margin-top:8px">
            <div><span class="muted">Min:</span> <strong id="cellMin">-</strong></div>
            <div><span class="muted">Avg:</span> <strong id="cellAvg">-</strong></div>
            <div><span class="muted">Max:</span> <strong id="cellMax">-</strong></div>
            <div><span class="muted">Delta:</span> <strong id="cellDelta">-</strong></div>
          </div>
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
            <a href="/ha">HA</a>
            <a href="/json">JSON</a>
            <a href="/diag">Diagnostic</a>
          </div>
        </div>
      </div>
    </div>
  </div>
  <script>
__UI_SHARED_JS__
    function renderCells(data, minMv, maxMv) {
      const grid = byId('cellGrid');
      grid.innerHTML = '';
      const slots = extractCellSlots(data);
      slots.forEach((slot) => {
        if (!slot.mv) return;
        const v = (slot.mv / 1000).toFixed(3);
        const el = document.createElement('div');
        el.className = 'cell';
        if (slot.mv === minMv) el.classList.add('low');
        if (slot.mv === maxMv) el.classList.add('high');
        el.innerHTML = '<div class="a">Cell ' + slot.index + '</div>' +
                       '<div class="v">' + v + ' V</div>';
        grid.appendChild(el);
      });
    }
    async function refresh() {
      try {
        const res = await fetch('/json', { cache: 'no-store' });
        const data = await res.json();
        setText('pollState', 'live');
        byId('pollState').className = 'pill live';
        setText('wifiState', data.wifi.connected ? 'connected' : 'disconnected');
        byId('wifiState').className = 'value ' + (data.wifi.connected ? 'ok' : 'bad');
        setText('wifiIp', 'IP: ' + data.wifi.ip);
        setText('uptime', Math.floor(data.uptime_ms / 1000) + ' s');
        setText('lastUpdate', data.last_update_ms ? ('last update: ' + Math.floor((data.uptime_ms - data.last_update_ms) / 1000) + ' s ago') : 'no data yet');
        setText('slaveId', data.active_slave_id || '-');
        setText('scanId', 'last tried: ' + (data.last_tried_slave_id || '-'));
        setText('errors', data.errors);
        setText('crc', 'CRC rx ' + hex16(data.crc_received || 0) + ' / calc ' + hex16(data.crc_calculated || 0));
        setText('mode', data.mode_label || data.mode || '-');
        setText('canEnabled', 'CAN: ' + (data.can_enabled ? 'enabled' : 'disabled'));
        const s = badgeForOnline(data.online);
        setText('statusText', s.text);
        byId('statusText').className = 'value ' + s.cls;
        setText('substatusText', data.substatus || '-');
        setText('packV', fmt(data.pack_v || 0, 2) + ' V');
        setText('currentA', fmt(data.current_a || 0, 2) + ' A');
        setText('soh', (data.soh ?? '-') + '%');
        setText('temp1', (data.temp_1_c ?? '-') + ' C');
        setText('temp2', (data.temp_2_c ?? '-') + ' C');
        setText('temp3', (data.temp_3_c ?? '-') + ' C');
        setText('remainingAh', data.remaining_ah ?? '-');
        setText('cellMin', fmt(data.cell_min_v || 0, 3) + ' V');
        setText('cellAvg', fmt(data.cell_avg_v || 0, 3) + ' V');
        setText('cellMax', fmt(data.cell_max_v || 0, 3) + ' V');
        setText('cellDelta', fmt(data.cell_delta_v || 0, 3) + ' V');
        setText('healthText', deriveOperatingText(data));
        const mvList = extractCellSlots(data);
        const cellCount = Number(data.cell_count || mvList.filter((slot) => slot.mv).length || 0);
        setText('cellCount', cellCount > 0 ? String(cellCount) : '-');
        const soc = clamp(Number(data.soc || 0), 0, 100);
        byId('socBar').style.width = soc + '%';
        byId('socBar').className = 'fill ' + (soc < 20 ? 'low' : soc < 80 ? 'mid' : 'high');
        setText('socText', soc + '%');
        renderCells(data, Math.round((data.cell_min_v || 0) * 1000), Math.round((data.cell_max_v || 0) * 1000));
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

const char DIAG_HTML[] PROGMEM = R"rawliteral(
<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='1'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
  __UI_SHARED_CSS__
  body{__UI_THEME_BASE_VARS____UI_THEME_CUSTOM_VARS__padding:18px}
  .card{max-width:1200px;margin:0 auto 16px auto;padding:16px}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px;}
  .cell{padding:10px;border-radius:12px;background:var(--panel2);border:1px solid var(--line);}
  .flag-group{margin-top:8px;padding:10px;border:1px solid var(--line);border-radius:10px;background:#151515;}
  .flag-title{color:var(--muted);font-size:11px;letter-spacing:.05em;text-transform:uppercase;margin-bottom:6px;}
  .tag{display:inline-block;margin:0 6px 6px 0;padding:4px 8px;border-radius:999px;font-size:12px;border:1px solid transparent;}
  .tag-muted{background:#202020;color:var(--muted);border-color:var(--line);}
  .tag-status{background:#1f2b3f;color:#dbe8ff;border-color:#35537a;}
  .tag-warn{background:#3d3415;color:#fff2ba;border-color:#7f6a20;}
  .tag-protect{background:#3c1818;color:#ffd0d0;border-color:#7f2e2e;}
  .soft{color:var(--muted);}
</style></head><body>
<div class='card'><h1>Diagnostics</h1>
<div>Active ID: __ACTIVE_ID__ | Last tried: __LAST_TRIED__</div>
<div>Status: __ONLINE__ | Errors: __ERRORS__</div>
<div>Operating state: __OPERATING__</div>
<div>Health: <span class='__HEALTH_CLASS__'>__HEALTH_TEXT__</span></div>
<div>Status: __STATUS_HEX__ <span class='muted'>(__STATUS_TEXT__)</span>__STATUS_CHIPS__</div>
<div>Warning: __WARNING_HEX__ <span class='muted'>(__WARNING_TEXT__)</span>__WARNING_CHIPS__</div>
<div>Protect: __PROTECT_HEX__ <span class='muted'>(__PROTECT_TEXT__)</span>__PROTECT_CHIPS__</div>
<div>Cycles: __CYCLES__</div>
<div>Cell count: __CELL_COUNT__</div>
<div>CRC rx: __CRC_RX__ | CRC calc: __CRC_CALC__</div>
<div><a href='/'>Back to dashboard</a></div>
</div>
__RAW_REGS__
</body></html>
)rawliteral";
