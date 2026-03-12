#!/usr/bin/env python3
"""
SmartHome captive portal — local test server
Proxies /ha/* to a real Home Assistant instance (REST API, self-signed TLS ok).
Falls back to mock data when HA is unreachable.

Usage:
    python3 server.py          # port 8080
    python3 server.py 9000
"""

import json, os, sys, time, random, threading, ssl, urllib.request, urllib.error
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, urlencode

PORT        = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
CONFIG_FILE = os.path.join(os.path.dirname(__file__), "config.json")

# ---------------------------------------------------------------------------
# Config helpers
# ---------------------------------------------------------------------------

def load_config():
    with open(CONFIG_FILE) as f:
        return json.load(f)

def save_config(data):
    with open(CONFIG_FILE, "w") as f:
        json.dump(data, f, indent=2)

# ---------------------------------------------------------------------------
# Home Assistant REST client
# ---------------------------------------------------------------------------

def _ssl_ctx():
    """SSL context that skips certificate verification (for self-signed HA certs)."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode    = ssl.CERT_NONE
    return ctx

_SSL = _ssl_ctx()

class HAClient:
    def __init__(self):
        self.base_url  = None
        self.token     = None
        self.connected = False
        self.error     = None
        self._states   = {}        # entity_id -> full state dict from HA
        self._lock     = threading.Lock()

    def configure(self, url, token):
        self.base_url = url.rstrip("/")
        self.token    = token
        self.connected = False
        self.error     = None

    def _headers(self):
        return {
            "Authorization": f"Bearer {self.token}",
            "Content-Type":  "application/json",
        }

    def _ctx(self):
        """Return SSL context only for https — http must get None (urlopen raises ValueError otherwise)."""
        return _SSL if self.base_url and self.base_url.startswith("https") else None

    def _get(self, path):
        req  = urllib.request.Request(
            self.base_url + path, headers=self._headers(), method="GET"
        )
        resp = urllib.request.urlopen(req, context=self._ctx(), timeout=10)
        return json.loads(resp.read())

    def _post(self, path, body=None):
        data = json.dumps(body or {}).encode()
        req  = urllib.request.Request(
            self.base_url + path, data=data, headers=self._headers(), method="POST"
        )
        resp = urllib.request.urlopen(req, context=self._ctx(), timeout=10)
        return json.loads(resp.read()) if resp.length != 0 else {}

    # ── Public API ───────────────────────────────────────────────────────

    def fetch_all_states(self):
        """Fetch all entity states from HA. Blocks — run in a thread."""
        try:
            states_list = self._get("/api/states")
            with self._lock:
                self._states   = {s["entity_id"]: s for s in states_list}
                self.connected = True
                self.error     = None
            push_log(f"I (ha_proxy): connected — {len(self._states)} entities loaded")
            return True
        except Exception as e:
            with self._lock:
                self.connected = False
                self.error     = str(e)
            push_log(f"E (ha_proxy): connection failed — {e}")
            return False

    def get_states(self):
        with self._lock:
            return dict(self._states)

    def get_entity(self, entity_id):
        with self._lock:
            return dict(self._states.get(entity_id, {}))

    def toggle(self, entity_id):
        """Call HA toggle service. Returns new state string or None on error."""
        domain = entity_id.split(".")[0] if "." in entity_id else "homeassistant"
        try:
            self._post(f"/api/services/{domain}/toggle",
                       {"entity_id": entity_id})
        except urllib.error.HTTPError:
            # Some domains don't have toggle — fall back to homeassistant.toggle
            try:
                self._post("/api/services/homeassistant/toggle",
                           {"entity_id": entity_id})
            except Exception as e:
                push_log(f"E (ha_proxy): toggle failed for {entity_id}: {e}")
                return None
        except Exception as e:
            push_log(f"E (ha_proxy): toggle failed for {entity_id}: {e}")
            return None

        # Re-fetch single entity state
        try:
            state_obj = self._get(f"/api/states/{entity_id}")
            new_state = state_obj.get("state", "unknown")
            with self._lock:
                self._states[entity_id] = state_obj
            push_log(f"I (ha_proxy): toggle {entity_id} -> {new_state}")
            return new_state
        except Exception as e:
            push_log(f"W (ha_proxy): state re-fetch failed: {e}")
            return None

    def status(self):
        with self._lock:
            return {
                "connected":    self.connected,
                "error":        self.error,
                "entity_count": len(self._states),
                "url":          self.base_url,
            }


# Global HA client
_ha = HAClient()

def _init_ha_from_config():
    cfg = load_config()
    ha  = cfg.get("ha", {})
    url   = ha.get("url", "")
    token = ha.get("token", "")
    if url and token:
        _ha.configure(url, token)
        threading.Thread(target=_ha.fetch_all_states, daemon=True).start()
    else:
        push_log("W (ha_proxy): no HA URL/token in config — using mock data")

# ---------------------------------------------------------------------------
# Mock entity fallback (used when HA is unreachable)
# ---------------------------------------------------------------------------

MOCK_STATES = {
    "light.living_room_ceiling":       {"state": "off",  "domain": "light",        "friendly_name": "Living Room Ceiling"},
    "light.kitchen":                   {"state": "on",   "domain": "light",        "friendly_name": "Kitchen Light"},
    "switch.coffee_maker":             {"state": "off",  "domain": "switch",       "friendly_name": "Coffee Maker"},
    "media_player.tv":                 {"state": "off",  "domain": "media_player", "friendly_name": "TV"},
    "climate.living_room":             {"state": "heat", "domain": "climate",      "friendly_name": "Living Room Thermostat"},
    "lock.front_door":                 {"state": "locked","domain": "lock",        "friendly_name": "Front Door"},
    "sensor.kukkasensori_temperature": {"state": "21.4", "domain": "sensor",       "friendly_name": "Indoor Temp"},
    "weather.forecast_koti":           {"state": "partlycloudy","domain":"weather","friendly_name": "Weather"},
    "cover.living_room_blinds":        {"state": "closed","domain": "cover",       "friendly_name": "Living Room Blinds"},
    "binary_sensor.motion_hallway":    {"state": "off",  "domain": "binary_sensor","friendly_name": "Hallway Motion"},
}
_mock_lock   = threading.Lock()
_mock_states = {k: dict(v) for k, v in MOCK_STATES.items()}

def mock_toggle(entity_id):
    with _mock_lock:
        ent = _mock_states.get(entity_id)
        if not ent:
            _mock_states[entity_id] = {"state": "on", "domain": entity_id.split(".")[0] if "." in entity_id else "unknown"}
            return "on"
        st = ent["state"]
        new = ("off"      if st in ("on","home","playing","heat","cool","auto") else
               "on"       if st in ("off","idle","standby") else
               "unlocked" if st == "locked" else
               "locked"   if st == "unlocked" else
               "open"     if st == "closed"  else
               "closed"   if st == "open"    else
               "off")
        ent["state"] = new
        return new

# ---------------------------------------------------------------------------
# Mock WiFi networks
# ---------------------------------------------------------------------------

MOCK_NETWORKS = [
    {"ssid": "DNA-WIFI-6117",  "rssi": -48},
    {"ssid": "Verkko-5G",      "rssi": -62},
    {"ssid": "HUAWEI-AP-3F2A", "rssi": -71},
    {"ssid": "iPhone",         "rssi": -75},
    {"ssid": "Taloyhtiö-WiFi", "rssi": -80},
]

# ---------------------------------------------------------------------------
# Log ring buffer
# ---------------------------------------------------------------------------

_log_lines = []
_log_lock  = threading.Lock()

def push_log(line):
    ts = time.strftime("%H:%M:%S")
    with _log_lock:
        _log_lines.append(f"[{ts}] {line}")
        if len(_log_lines) > 128:
            _log_lines.pop(0)

for _m in [
    "I (312) concept: Smart Home starting",
    "I (901) http_srv: HTTP server started",
]:
    push_log(_m)

def _log_sim():
    while True:
        time.sleep(6)
        t = int(time.monotonic() * 1000)
        push_log(random.choice([
            f"D ({t}): heap free {random.randint(180000,240000)} bytes",
            f"I ({t}): lvgl tick handler",
        ]))

threading.Thread(target=_log_sim, daemon=True).start()

# ---------------------------------------------------------------------------
# HTML + JS
# ---------------------------------------------------------------------------
# SAFETY RULES enforced here:
#   - No bare \n or \t inside JavaScript string literals within this
#     triple-quoted Python string.  Use \\n / \\t so Python outputs the
#     two-character escape sequence, not a literal whitespace character.
#   - Never call element.onclick — use element.getAttribute('onclick').
#   - Set membership uses .has(), not .includes().
# ---------------------------------------------------------------------------

INDEX_HTML = """\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartHome Setup</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0d1117;color:#e6edf3;font-family:system-ui,sans-serif;padding-bottom:48px}
  header{background:#161b22;border-bottom:1px solid #30363d;padding:14px 20px;
         display:flex;align-items:center;gap:12px;position:sticky;top:0;z-index:10}
  header h1{font-size:17px;color:#40c080;font-weight:600}
  #ha-badge{font-size:12px;padding:3px 10px;border-radius:20px;margin-left:auto;
            background:#1c2128;border:1px solid #30363d;color:#8b949e;
            transition:all .3s;cursor:default}
  #ha-badge.ok{border-color:#40c080;color:#40c080}
  #ha-badge.err{border-color:#e05050;color:#e05050}
  .tabs{display:flex;border-bottom:1px solid #30363d;background:#161b22;
        padding:0 16px;overflow-x:auto}
  .tab{padding:11px 16px;font-size:13px;color:#8b949e;cursor:pointer;
       border-bottom:2px solid transparent;white-space:nowrap;transition:color .15s}
  .tab:hover{color:#e6edf3}
  .tab.active{color:#40c080;border-bottom-color:#40c080}
  .page{display:none;max-width:640px;margin:0 auto;padding:20px 16px}
  .page.active{display:block}
  .card{background:#161b22;border:1px solid #30363d;border-radius:12px;
        padding:20px;margin-bottom:16px}
  .card h2{font-size:12px;font-weight:600;color:#8b949e;text-transform:uppercase;
           letter-spacing:.06em;margin-bottom:16px}
  label{display:block;font-size:13px;color:#8b949e;margin-bottom:4px;margin-top:12px}
  label:first-of-type{margin-top:0}
  input,textarea,select{width:100%;background:#0d1117;color:#e6edf3;border:1px solid #30363d;
    border-radius:6px;padding:9px 10px;font-size:14px;font-family:inherit;
    outline:none;transition:border-color .15s}
  input:focus,textarea:focus,select:focus{border-color:#40c080}
  select option{background:#0d1117}
  textarea{height:260px;resize:vertical;font-family:monospace;font-size:12px;line-height:1.5}
  .row{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap;align-items:center}
  button{padding:8px 16px;border:none;border-radius:6px;font-size:13px;font-weight:600;
    cursor:pointer;white-space:nowrap;line-height:1.4;transition:opacity .15s}
  button:disabled{opacity:.4;cursor:default}
  .btn-primary{background:#40c080;color:#0d1117}
  .btn-primary:not(:disabled):hover{background:#52d492}
  .btn-warn{background:#f0c040;color:#0d1117}
  .btn-warn:not(:disabled):hover{background:#f8d050}
  .btn-info{background:#40b0f0;color:#0d1117}
  .btn-info:not(:disabled):hover{background:#52c0ff}
  .btn-ghost{background:transparent;color:#8b949e;border:1px solid #30363d}
  .btn-ghost:not(:disabled):hover{color:#e6edf3;border-color:#8b949e}
  .btn-danger{background:transparent;color:#e05050;border:1px solid #e05050}
  .btn-danger:not(:disabled):hover{background:#e05050;color:#0d1117}
  .btn-sm{padding:5px 10px;font-size:12px}
  .status{font-size:13px;margin-top:8px;min-height:18px}
  .ok{color:#40c080}.err{color:#e05050}.inf{color:#40b0f0}.warn{color:#f0c040}
  /* WiFi */
  #networks{display:flex;flex-direction:column;gap:4px;margin-top:10px}
  .net-item{display:flex;align-items:center;justify-content:space-between;
    padding:8px 10px;background:#0d1117;border:1px solid #30363d;
    border-radius:6px;cursor:pointer;transition:border-color .15s}
  .net-item:hover{border-color:#40b0f0}
  .signal{display:inline-flex;gap:2px;align-items:flex-end}
  .signal span{width:4px;border-radius:1px}
  /* Dashboard tiles */
  .tile-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));
    gap:12px;margin-bottom:8px}
  .tile{background:#161b22;border:2px solid #30363d;border-radius:14px;
    padding:16px 12px;cursor:pointer;text-align:center;user-select:none;
    min-height:110px;display:flex;flex-direction:column;
    align-items:center;justify-content:center;gap:8px;transition:all .2s}
  .tile:hover{border-color:#555;transform:translateY(-1px)}
  .tile.on{border-color:var(--accent)}
  .tile-icon{font-size:24px;color:#3a4048;transition:color .2s}
  .tile.on .tile-icon{color:var(--accent)}
  .tile-label{font-size:13px;color:#8b949e;font-weight:500}
  .tile.on .tile-label{color:#e6edf3}
  .tile-state{font-size:11px;color:#444c56}
  .tile.on .tile-state{color:#8b949e}
  /* Entity list */
  .entity-list{display:flex;flex-direction:column;gap:6px;margin-top:12px;
    max-height:420px;overflow-y:auto}
  .entity-row{display:flex;align-items:center;gap:8px;padding:8px 10px;
    background:#0d1117;border:1px solid #30363d;border-radius:8px}
  .entity-id{font-family:monospace;font-size:12px;color:#8b949e;flex:1;
    min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .entity-name{font-size:11px;color:#444c56;margin-top:2px}
  .entity-state{font-size:12px;font-weight:600;padding:2px 8px;border-radius:10px;
    background:#1c2128;white-space:nowrap;flex-shrink:0}
  .s-on {color:#40c080;border:1px solid #40c08040}
  .s-off{color:#444c56;border:1px solid #30363d}
  .s-oth{color:#f0c040;border:1px solid #f0c04040}
  .entity-actions{display:flex;gap:4px;flex-shrink:0}
  /* Add tile form */
  .form-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  .form-grid .full{grid-column:1/-1}
  .color-dot{width:16px;height:16px;border-radius:4px;
    display:inline-block;vertical-align:middle;margin-left:6px;border:1px solid #30363d}
  /* Logs */
  #logbox{background:#0d1117;border:1px solid #30363d;border-radius:6px;
    height:240px;overflow-y:auto;padding:8px 10px;font-size:11px;
    font-family:monospace;white-space:pre-wrap;margin-bottom:10px}
  .log-I{color:#8b949e}.log-W{color:#f0c040}.log-E{color:#e05050}.log-D{color:#3a4048}
  /* Spinner */
  .spin{display:inline-block;width:11px;height:11px;border:2px solid #30363d;
    border-top-color:#40c080;border-radius:50%;animation:sp .6s linear infinite;
    margin-right:5px;vertical-align:middle}
  @keyframes sp{to{transform:rotate(360deg)}}
  .src-badge{font-size:10px;padding:1px 6px;border-radius:8px;margin-left:6px;
    vertical-align:middle;background:#1c2128;color:#8b949e;border:1px solid #30363d}
  .src-badge.live{color:#40c080;border-color:#40c08060}
</style>
</head>
<body>

<header>
  <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="#40c080" stroke-width="1.8">
    <path d="M3 9.5L12 3l9 6.5V20a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V9.5z"/>
    <polyline points="9 21 9 12 15 12 15 21"/>
  </svg>
  <h1>SmartHome Setup</h1>
  <span id="ha-badge" title="">HA: connecting…</span>
</header>

<div class="tabs">
  <div class="tab active"  onclick="showTab('wifi')">WiFi</div>
  <div class="tab"         onclick="showTab('ha')">Home Assistant</div>
  <div class="tab"         onclick="showTab('dashboard')">Dashboard</div>
  <div class="tab"         onclick="showTab('entities')">Entity Explorer</div>
  <div class="tab"         onclick="showTab('config')">JSON Config</div>
  <div class="tab"         onclick="showTab('logs')">Logs</div>
</div>

<!-- WiFi -->
<div id="tab-wifi" class="page active">
  <div class="card">
    <h2>WiFi Network</h2>
    <div class="row" style="margin-top:0;margin-bottom:10px">
      <button class="btn-info" id="btn-scan" onclick="scanWifi()">Scan networks</button>
    </div>
    <div id="networks"></div>
    <label>Network name (SSID)</label>
    <input id="ssid" placeholder="e.g. MyHomeWiFi" autocomplete="off" spellcheck="false">
    <label>Password</label>
    <input id="pass" type="password" placeholder="Leave empty for open networks">
    <div class="row">
      <button class="btn-warn" id="btn-wifi" onclick="saveWifi()">Save &amp; Reboot</button>
    </div>
    <div id="wifi-status" class="status"></div>
  </div>
</div>

<!-- Home Assistant -->
<div id="tab-ha" class="page">
  <div class="card">
    <h2>Home Assistant Connection</h2>
    <label>URL</label>
    <input id="haurl" placeholder="https://192.168.1.10:8123" autocomplete="off" spellcheck="false">
    <label>Long-lived access token</label>
    <input id="hatoken" type="password" placeholder="Paste your HA token here">
    <div class="row">
      <button class="btn-primary" id="btn-ha"        onclick="saveHA()">Save HA Settings</button>
      <button class="btn-ghost"   id="btn-reload-ha" onclick="reloadHA()">Reload from HA</button>
      <button class="btn-danger"  id="btn-disconnect" onclick="disconnectHA()">Disconnect</button>
    </div>
    <div id="ha-status" class="status"></div>
  </div>
</div>

<!-- Dashboard preview -->
<div id="tab-dashboard" class="page">
  <div class="card">
    <h2>Pages
      <span style="font-weight:400;text-transform:none;font-size:11px;color:#444c56">
        — manage dashboard pages</span>
    </h2>
    <div id="page-tabs" style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px"></div>
    <div class="row" style="margin-top:0">
      <button class="btn-primary btn-sm" id="btn-add-page" onclick="addPage()">+ Add Page</button>
      <button class="btn-ghost btn-sm"   id="btn-del-page" onclick="deletePage()">Delete Page</button>
      <input id="page-name-edit" placeholder="Page name" style="flex:1;margin:0">
      <button class="btn-ghost btn-sm" onclick="renamePage()">Rename</button>
    </div>
  </div>
  <div class="card">
    <h2>Dashboard Preview
      <span style="font-weight:400;text-transform:none;font-size:11px;color:#444c56">
        — click tiles to toggle</span>
    </h2>
    <div id="tile-grid" class="tile-grid"></div>
    <div id="preview-status" class="status"></div>
  </div>
  <div class="card">
    <h2>Add New Tile <span id="add-tile-page-label" style="font-size:12px;font-weight:400;color:#8b949e"></span></h2>
    <div class="form-grid">
      <div>
        <label>Entity ID</label>
        <input id="new-entity" placeholder="light.living_room" autocomplete="off"
               spellcheck="false" oninput="onNewEntityInput(this.value)">
      </div>
      <div>
        <label>Label</label>
        <input id="new-label" placeholder="Living Room Light">
      </div>
      <div>
        <label>Icon</label>
        <select id="new-icon">
          <option value="power">power</option><option value="home">home</option>
          <option value="bell">bell</option><option value="settings">settings</option>
          <option value="play">play</option><option value="audio">audio</option>
          <option value="wifi">wifi</option><option value="loop">loop</option>
          <option value="ok">ok</option><option value="gps">gps</option>
          <option value="edit">edit</option><option value="image">image</option>
          <option value="list">list</option>
        </select>
      </div>
      <div>
        <label>Accent colour
          <span id="color-dot" class="color-dot" style="background:#40c080"></span>
        </label>
        <input id="new-color" value="40c080" placeholder="hex e.g. f0c040"
               maxlength="6" oninput="updateDot(this.value)">
      </div>
      <div class="full">
        <span id="new-entity-info" class="status inf"></span>
      </div>
    </div>
    <div class="row">
      <button class="btn-primary" id="btn-add-tile" onclick="addTile()">
        Add to Page</button>
    </div>
    <div id="add-tile-status" class="status"></div>
  </div>
</div>

<!-- Entity Explorer -->
<div id="tab-entities" class="page">
  <div class="card">
    <h2>Entity Explorer
      <span id="entity-source" class="src-badge"></span>
    </h2>
    <div class="row" style="margin-top:0;margin-bottom:0">
      <input id="entity-filter" placeholder="Filter by entity ID or name…"
             oninput="renderEntityList()" style="flex:1;margin-bottom:0">
      <button class="btn-info btn-sm" id="btn-reload-entities" onclick="reloadEntities()">
        Reload</button>
    </div>
    <div id="entity-list" class="entity-list"></div>
  </div>
</div>

<!-- JSON Config -->
<div id="tab-config" class="page">
  <div class="card">
    <h2>Dashboard Config (JSON)</h2>
    <textarea id="cfg" spellcheck="false"></textarea>
    <div class="row">
      <button class="btn-primary" id="btn-cfg"  onclick="saveConfig()">
        Save &amp; Reload UI</button>
      <button class="btn-ghost"                 onclick="loadConfig()">
        Discard changes</button>
    </div>
    <div id="cfg-status" class="status"></div>
  </div>
</div>

<!-- Logs -->
<div id="tab-logs" class="page">
  <div class="card">
    <h2>Live Logs</h2>
    <div id="logbox"></div>
    <div class="row">
      <button class="btn-ghost" onclick="clearLogs()">Clear</button>
    </div>
  </div>
</div>

<script>
// ── Tab navigation ─────────────────────────────────────────────────────────
function showTab(name) {
  document.querySelectorAll('.tab').forEach(function(t) {
    var fn = t.getAttribute('onclick') || '';
    t.classList.toggle('active', fn.indexOf("'" + name + "'") !== -1);
  });
  document.querySelectorAll('.page').forEach(function(p) {
    p.classList.remove('active');
  });
  var el = document.getElementById('tab-' + name);
  if (el) el.classList.add('active');
  if (name === 'dashboard') loadDashboard();
  if (name === 'entities')  loadAndRenderEntities();
  if (name === 'config')    loadConfig();
}

// ── Helpers ────────────────────────────────────────────────────────────────
function setStatus(id, msg, cls) {
  var el = document.getElementById(id);
  if (!el) return;
  el.textContent = msg;
  el.className = 'status ' + (cls || '');
}
function setLoading(id, on) {
  var btn = document.getElementById(id);
  if (!btn) return;
  if (on) { btn._lbl = btn.innerHTML; btn.innerHTML = '<span class="spin"></span>Working…'; btn.disabled = true; }
  else    { if (btn._lbl) btn.innerHTML = btn._lbl; btn.disabled = false; }
}
function esc(s) {
  return String(s)
    .replace(/&/g,'&amp;').replace(/</g,'&lt;')
    .replace(/>/g,'&gt;').replace(/"/g,'&quot;')
    .replace(/'/g,'&#39;');
}
function bars(rssi) {
  var n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  var h = [6,9,13,17];
  return '<span class="signal">' +
    h.map(function(px,i){
      return '<span style="height:'+px+'px;background:'+(i<n?'#40b0f0':'#30363d')+'"></span>';
    }).join('') + '</span>';
}

// ── HA status badge ────────────────────────────────────────────────────────
function refreshHABadge() {
  fetch('/ha/status').then(function(r){ return r.json(); }).then(function(s) {
    var b = document.getElementById('ha-badge');
    if (s.connected) {
      b.textContent = 'HA: ' + s.entity_count + ' entities';
      b.className = 'ok';
      b.title = s.url || '';
    } else {
      b.textContent = s.error ? 'HA: error' : 'HA: mock';
      b.className = s.error ? 'err' : '';
      b.title = s.error || 'No HA connection — using mock data';
    }
  }).catch(function(){});
}

// ── Config ─────────────────────────────────────────────────────────────────
var _cfg = null;
function loadConfig() {
  return fetch('/config').then(function(r){ return r.json(); }).then(function(j) {
    _cfg = j;
    document.getElementById('cfg').value = JSON.stringify(j, null, 2);
    if (j.ha) {
      if (j.ha.url)   document.getElementById('haurl').value   = j.ha.url;
      if (j.ha.token) document.getElementById('hatoken').value = j.ha.token;
    }
    return j;
  }).catch(function(e){ setStatus('cfg-status','Load failed: '+e,'err'); });
}
async function saveConfig() {
  var txt = document.getElementById('cfg').value, parsed;
  try { parsed = JSON.parse(txt); } catch(e) {
    setStatus('cfg-status','Invalid JSON: '+e.message,'err'); return;
  }
  setLoading('btn-cfg', true);
  try {
    var r = await fetch('/config',{method:'POST',
      headers:{'Content-Type':'application/json'},body:JSON.stringify(parsed)});
    if (r.ok) { _cfg = parsed; setStatus('cfg-status','Saved!','ok'); }
    else       setStatus('cfg-status','Error '+r.status,'err');
  } catch(e) { setStatus('cfg-status','Failed: '+e,'err'); }
  finally    { setLoading('btn-cfg', false); }
}

// ── HA settings ────────────────────────────────────────────────────────────
async function saveHA() {
  var url = document.getElementById('haurl').value.trim();
  var tok = document.getElementById('hatoken').value.trim();
  if (!url) { setStatus('ha-status','Enter HA URL','warn'); return; }
  if (!tok) { setStatus('ha-status','Enter token','warn'); return; }
  setLoading('btn-ha', true);
  try {
    var cfg = await (await fetch('/config')).json();
    cfg.ha = { url: url, token: tok };
    var r = await fetch('/config',{method:'POST',
      headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});
    if (r.ok) { setStatus('ha-status','Saved — reconnecting to HA…','ok'); refreshHABadge(); }
    else       setStatus('ha-status','Error '+r.status,'err');
  } catch(e) { setStatus('ha-status','Failed: '+e,'err'); }
  finally    { setLoading('btn-ha', false); }
}
async function reloadHA() {
  setLoading('btn-reload-ha', true);
  setStatus('ha-status','','');
  try {
    var r = await fetch('/ha/reload', {method:'POST'});
    var j = await r.json();
    setStatus('ha-status', j.ok ? 'Reloaded '+j.entity_count+' entities' : j.error, j.ok?'ok':'err');
    refreshHABadge();
  } catch(e) { setStatus('ha-status','Failed: '+e,'err'); }
  finally    { setLoading('btn-reload-ha', false); }
}
async function disconnectHA() {
  if (!confirm('Clear HA URL and token from config and disconnect?')) return;
  setLoading('btn-disconnect', true);
  try {
    var r = await fetch('/ha/disconnect', {method:'POST'});
    var j = await r.json();
    if (j.ok) {
      document.getElementById('haurl').value   = '';
      document.getElementById('hatoken').value = '';
      setStatus('ha-status', 'Disconnected — HA config cleared', 'warn');
      refreshHABadge();
    } else {
      setStatus('ha-status', 'Error: ' + j.error, 'err');
    }
  } catch(e) { setStatus('ha-status','Failed: '+e,'err'); }
  finally    { setLoading('btn-disconnect', false); }
}

// ── WiFi ───────────────────────────────────────────────────────────────────
async function scanWifi() {
  setLoading('btn-scan', true);
  document.getElementById('networks').innerHTML = '';
  try {
    var nets = await (await fetch('/wifi/scan')).json();
    var el   = document.getElementById('networks');
    el.innerHTML = nets.length
      ? nets.map(function(n){
          return '<div class="net-item" data-ssid="'+esc(n.ssid)+'">'+
            '<span style="font-size:13px">'+esc(n.ssid)+'</span>'+
            '<span style="font-size:12px;color:#8b949e">'+n.rssi+'dBm '+bars(n.rssi)+'</span>'+
          '</div>';
        }).join('')
      : '<span style="color:#8b949e;font-size:13px">No networks found</span>';
    el.querySelectorAll('.net-item').forEach(function(item) {
      item.addEventListener('click', function() {
        document.getElementById('ssid').value = this.dataset.ssid;
        document.getElementById('pass').focus();
      });
    });
  } catch(e) { setStatus('wifi-status','Scan failed: '+e,'err'); }
  finally    { setLoading('btn-scan', false); }
}
async function saveWifi() {
  var ssid = document.getElementById('ssid').value.trim();
  var pass = document.getElementById('pass').value;
  if (!ssid) { setStatus('wifi-status','Enter SSID','warn'); return; }
  if (!confirm('Save WiFi credentials and reboot device?')) return;
  setLoading('btn-wifi', true);
  try {
    await fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:ssid,password:pass})});
    setStatus('wifi-status','Saved — device rebooting…','ok');
  } catch(e) { setStatus('wifi-status','Failed: '+e,'err'); setLoading('btn-wifi',false); }
}

// ── Dashboard preview ──────────────────────────────────────────────────────
var _tileStates = {};
var _dashPage   = 0;     /* currently selected page index in admin portal */
var ICONS = {power:'⏻',home:'⌂',bell:'🔔',settings:'⚙',play:'▶',audio:'♪',
             wifi:'📶',loop:'↺',ok:'✓',gps:'◎',edit:'✏',image:'🖼',list:'☰'};
function isOn(s){ return ['on','home','open','playing','heat','cool','auto','unlocked'].indexOf(s) !== -1; }

/* Page tabs for admin portal */
function renderPageTabs(cfg) {
  var tabs = document.getElementById('page-tabs');
  if (!tabs) return;
  var pages = cfg.pages || [];
  tabs.innerHTML = pages.map(function(p, i) {
    var active = i === _dashPage ? 'background:#40c080;color:#0d1117;' : '';
    return '<button style="padding:6px 14px;border-radius:8px;border:1px solid #30363d;' +
           'background:#1c2128;color:#8b949e;cursor:pointer;font-size:13px;' + active +
           '" data-pi="' + i + '">' + esc(p.name||'Page '+(i+1)) + '</button>';
  }).join('');
  tabs.querySelectorAll('button').forEach(function(b) {
    b.addEventListener('click', function() {
      _dashPage = parseInt(this.dataset.pi);
      renderPageTabs(cfg);
      renderTiles(cfg);
      var lbl = document.getElementById('add-tile-page-label');
      if (lbl) lbl.textContent = '→ ' + (cfg.pages[_dashPage]||{}).name || '';
      var ni = document.getElementById('page-name-edit');
      if (ni) ni.value = (cfg.pages[_dashPage]||{}).name || '';
    });
  });
  /* Sync name edit field */
  var ni = document.getElementById('page-name-edit');
  if (ni && !ni.value) ni.value = (pages[_dashPage]||{}).name || '';
}

async function loadDashboard() {
  var cfg = _cfg || await loadConfig();
  if (!cfg) return;
  var st = await (await fetch('/ha/states')).json();
  _tileStates = {};
  for (var k in st) _tileStates[k] = st[k].state !== undefined ? st[k].state : st[k];
  if (_dashPage >= (cfg.pages||[]).length) _dashPage = 0;
  renderPageTabs(cfg);
  renderTiles(cfg);
}
function renderTiles(cfg) {
  var grid  = document.getElementById('tile-grid');
  var page  = (cfg.pages || [])[_dashPage] || {};
  var tiles = page.tiles || [];
  var lbl   = document.getElementById('add-tile-page-label');
  if (lbl) lbl.textContent = page.name ? '→ ' + page.name : '';
  if (!tiles.length) {
    grid.innerHTML = '<span style="color:#8b949e;font-size:13px">No tiles on this page.</span>';
    return;
  }
  grid.innerHTML = tiles.map(function(t, idx) {
    var accent = '#' + (t.color || '888888');
    var state  = _tileStates[t.entity] || 'unknown';
    var on     = isOn(state);
    var icon   = ICONS[t.icon] || '⚙';
    return (
      '<div class="tile'+(on?' on':'')+'" style="--accent:'+accent+'" data-entity="'+esc(t.entity)+'">' +
      '<div class="tile-icon">'+icon+'</div>' +
      '<div class="tile-label">'+esc(t.label||t.entity)+'</div>' +
      '<div class="tile-state">'+esc(state)+'</div>' +
      '<div style="position:absolute;top:4px;right:4px;display:flex;gap:4px">' +
        (idx>0 ? '<button class="tile-reorder" data-dir="-1" data-idx="'+idx+'" title="Move left" style="background:none;border:none;color:#8b949e;cursor:pointer;font-size:14px">&#8592;</button>' : '') +
        (idx<tiles.length-1 ? '<button class="tile-reorder" data-dir="1" data-idx="'+idx+'" title="Move right" style="background:none;border:none;color:#8b949e;cursor:pointer;font-size:14px">&#8594;</button>' : '') +
        '<button class="tile-del" data-idx="'+idx+'" title="Remove" style="background:none;border:none;color:#8b949e;cursor:pointer;font-size:14px">&#215;</button>' +
      '</div>' +
      '</div>'
    );
  }).join('');
  grid.querySelectorAll('.tile').forEach(function(el) {
    el.addEventListener('click', function(e) {
      if (e.target.closest('button')) return;
      clickTile(this.dataset.entity, this);
    });
  });
  grid.querySelectorAll('.tile-del').forEach(function(btn) {
    btn.addEventListener('click', function(e) {
      e.stopPropagation();
      deleteTile(parseInt(this.dataset.idx));
    });
  });
  grid.querySelectorAll('.tile-reorder').forEach(function(btn) {
    btn.addEventListener('click', function(e) {
      e.stopPropagation();
      reorderTile(parseInt(this.dataset.idx), parseInt(this.dataset.dir));
    });
  });
}
async function clickTile(eid, el) {
  var wasOn = el.classList.contains('on');
  el.classList.toggle('on', !wasOn);
  try {
    var r = await fetch('/ha/toggle',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({entity_id:eid})});
    var d = await r.json();
    _tileStates[eid] = d.new_state;
    el.classList.toggle('on', isOn(d.new_state));
    el.querySelector('.tile-state').textContent = d.new_state;
    setStatus('preview-status', eid + ' \u2192 ' + d.new_state, 'inf');
  } catch(e) {
    el.classList.toggle('on', wasOn);
    setStatus('preview-status','Toggle failed: '+e,'err');
  }
}

/* ── Page CRUD ── */
async function addPage() {
  var cfg = _cfg || await (await fetch('/config')).json();
  if (!cfg.pages) cfg.pages = [];
  cfg.pages.push({name:'New Page',tiles:[]});
  _dashPage = cfg.pages.length - 1;
  await saveCfg(cfg);
  renderPageTabs(cfg);
  renderTiles(cfg);
}
async function deletePage() {
  var cfg = _cfg || await (await fetch('/config')).json();
  if (!cfg.pages || cfg.pages.length <= 1) {
    setStatus('preview-status','Cannot delete the last page','warn'); return;
  }
  if (!confirm('Delete page "' + (cfg.pages[_dashPage]||{}).name + '"?')) return;
  cfg.pages.splice(_dashPage, 1);
  _dashPage = Math.max(0, _dashPage - 1);
  await saveCfg(cfg);
  renderPageTabs(cfg);
  renderTiles(cfg);
}
async function renamePage() {
  var name = document.getElementById('page-name-edit').value.trim();
  if (!name) return;
  var cfg = _cfg || await (await fetch('/config')).json();
  if (cfg.pages && cfg.pages[_dashPage]) cfg.pages[_dashPage].name = name;
  await saveCfg(cfg);
  renderPageTabs(cfg);
}

/* ── Tile CRUD ── */
async function deleteTile(idx) {
  var cfg = _cfg || await (await fetch('/config')).json();
  var page = (cfg.pages||[])[_dashPage];
  if (!page || !page.tiles) return;
  var lbl = (page.tiles[idx]||{}).label || 'tile';
  if (!confirm('Remove "' + lbl + '"?')) return;
  page.tiles.splice(idx, 1);
  await saveCfg(cfg);
  renderTiles(cfg);
}
async function reorderTile(idx, dir) {
  var cfg = _cfg || await (await fetch('/config')).json();
  var tiles = ((cfg.pages||[])[_dashPage]||{}).tiles;
  if (!tiles) return;
  var newIdx = idx + dir;
  if (newIdx < 0 || newIdx >= tiles.length) return;
  var tmp = tiles[idx]; tiles[idx] = tiles[newIdx]; tiles[newIdx] = tmp;
  await saveCfg(cfg);
  renderTiles(cfg);
}
async function saveCfg(cfg) {
  _cfg = cfg;
  await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(cfg)});
}

// ── Add tile form ──────────────────────────────────────────────────────────
function updateDot(v) {
  var c = v.replace(/[^0-9a-fA-F]/g,'');
  document.getElementById('color-dot').style.background = c.length===6 ? '#'+c : '#444';
}
async function onNewEntityInput(eid) {
  var info = document.getElementById('new-entity-info');
  if (!eid || eid.indexOf('.') === -1) { info.textContent=''; return; }
  try {
    var st = await (await fetch('/ha/states')).json();
    var e  = st[eid];
    if (e) {
      var name = (e.attributes && e.attributes.friendly_name) || e.friendly_name || '';
      info.textContent = 'state: ' + (e.state || e) + (name ? '  |  ' + name : '');
      info.className   = 'status ok';
      var lbl = document.getElementById('new-label');
      if (!lbl.value && name && name !== eid) lbl.value = name;
    } else {
      info.textContent = 'Not found in HA — will be created as new entity';
      info.className   = 'status warn';
    }
  } catch(e) { info.textContent = ''; }
}
async function addTile() {
  var entity = document.getElementById('new-entity').value.trim();
  var label  = document.getElementById('new-label').value.trim() || entity;
  var icon   = document.getElementById('new-icon').value;
  var color  = document.getElementById('new-color').value.trim().replace('#','');
  if (!entity)         { setStatus('add-tile-status','Enter entity ID','warn'); return; }
  if (color.length!==6){ setStatus('add-tile-status','Color must be 6 hex chars','err'); return; }
  setLoading('btn-add-tile', true);
  try {
    var cfg = await (await fetch('/config')).json();
    if (!cfg.pages || !cfg.pages.length) cfg.pages = [{name:'Home',tiles:[]}];
    var page = cfg.pages[_dashPage] || cfg.pages[0];
    if (!page.tiles) page.tiles = [];
    if (page.tiles.some(function(t){ return t.entity === entity; })) {
      setStatus('add-tile-status', entity + ' already on this page','warn'); return;
    }
    page.tiles.push({label:label,icon:icon,color:color,entity:entity});
    await saveCfg(cfg);
    renderTiles(cfg);
    setStatus('add-tile-status','Added "'+label+'" to '+page.name+'!','ok');
    document.getElementById('new-entity').value = '';
    document.getElementById('new-label').value  = '';
    document.getElementById('new-entity-info').textContent = '';
  } catch(e) { setStatus('add-tile-status','Failed: '+e,'err'); }
  finally    { setLoading('btn-add-tile', false); }
}

// ── Entity Explorer ────────────────────────────────────────────────────────
var _entities  = {};
var _entSource = 'mock';
var TOGGLEABLE = new Set(['light','switch','media_player','lock','cover',
                          'automation','scene','input_boolean','fan','input_switch']);

async function loadAndRenderEntities() {
  setLoading('btn-reload-entities', true);
  try {
    var r  = await fetch('/ha/states');
    var st = await r.json();
    _entities = st;
    var src = r.headers.get('X-Source') || 'mock';
    _entSource = src;
    document.getElementById('entity-source').textContent = src;
    document.getElementById('entity-source').className =
      'src-badge' + (src === 'live' ? ' live' : '');
  } catch(e) { /* keep old */ }
  renderEntityList();
  setLoading('btn-reload-entities', false);
}
async function reloadEntities() {
  _entities = {};
  await loadAndRenderEntities();
}
function renderEntityList() {
  var filter = (document.getElementById('entity-filter').value || '').toLowerCase();
  var list   = document.getElementById('entity-list');
  var keys   = Object.keys(_entities).filter(function(eid) {
    var e = _entities[eid];
    var name = (e.attributes && e.attributes.friendly_name) || e.friendly_name || '';
    return !filter || eid.toLowerCase().indexOf(filter) !== -1
                   || name.toLowerCase().indexOf(filter) !== -1;
  }).sort();
  if (!keys.length) {
    list.innerHTML = '<span style="color:#8b949e;font-size:13px;display:block;padding:8px 0">'
                   + (Object.keys(_entities).length ? 'No match.' : 'Loading…') + '</span>';
    return;
  }
  list.innerHTML = keys.map(function(eid) {
    var e      = _entities[eid];
    var state  = e.state !== undefined ? e.state : String(e);
    var name   = (e.attributes && e.attributes.friendly_name) || e.friendly_name || '';
    var domain = eid.split('.')[0];
    var cls    = isOn(state) ? 's-on' : state === 'off' || state === 'locked' ? 's-off' : 's-oth';
    var canTog = TOGGLEABLE.has(domain);
    return '<div class="entity-row" data-entity="'+esc(eid)+'" data-name="'+esc(name||eid)+'">'
      + '<div style="flex:1;min-width:0">'
      +   '<div class="entity-id" title="'+esc(eid)+'">'+esc(eid)+'</div>'
      +   (name && name!==eid ? '<div class="entity-name">'+esc(name)+'</div>' : '')
      + '</div>'
      + '<span class="entity-state '+cls+'">'+esc(state)+'</span>'
      + '<div class="entity-actions">'
      + (canTog ? '<button class="btn-ghost btn-sm" data-action="toggle">Toggle</button>' : '')
      + '<button class="btn-info btn-sm" data-action="add">+&nbsp;Tile</button>'
      + '</div>'
      + '</div>';
  }).join('');
  list.querySelectorAll('.entity-row').forEach(function(row) {
    var eid  = row.dataset.entity;
    var name = row.dataset.name;
    var tog  = row.querySelector('[data-action="toggle"]');
    var add  = row.querySelector('[data-action="add"]');
    if (tog) tog.addEventListener('click', function() { explorerToggle(eid, this); });
    if (add) add.addEventListener('click', function() { sendToBuilder(eid, name); });
  });
}
async function explorerToggle(eid, btn) {
  btn.disabled = true; btn.textContent = '…';
  try {
    var r = await fetch('/ha/toggle',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({entity_id:eid})});
    var d = await r.json();
    if (_entities[eid]) {
      if (_entities[eid].state !== undefined) _entities[eid].state = d.new_state;
      else _entities[eid] = d.new_state;
    }
    _tileStates[eid] = d.new_state;
    var row   = btn.closest('.entity-row');
    var badge = row && row.querySelector('.entity-state');
    if (badge) {
      badge.textContent = d.new_state;
      badge.className = 'entity-state ' + (isOn(d.new_state)?'s-on':d.new_state==='off'?'s-off':'s-oth');
    }
    var tile = document.querySelector('.tile[data-entity="'+eid+'"]');
    if (tile) {
      tile.classList.toggle('on', isOn(d.new_state));
      tile.querySelector('.tile-state').textContent = d.new_state;
    }
  } catch(e) { alert('Toggle failed: '+e); }
  finally { btn.disabled=false; btn.textContent='Toggle'; }
}
function sendToBuilder(eid, name) {
  showTab('dashboard');
  document.getElementById('new-entity').value = eid;
  document.getElementById('new-label').value  = name !== eid ? name : '';
  onNewEntityInput(eid);
  document.getElementById('new-entity').focus();
}

// ── Logs ───────────────────────────────────────────────────────────────────
var _logCount = 0, _autoScroll = true;
async function pollLogs() {
  try {
    var body  = await (await fetch('/logs')).text();
    var lines = body.trim().split('\\n').filter(Boolean);
    if (lines.length === _logCount) return;
    _logCount = lines.length;
    var el = document.getElementById('logbox');
    el.innerHTML = lines.map(function(l) {
      var c = l.indexOf(' W ')!==-1?'log-W':l.indexOf(' E ')!==-1?'log-E':
              l.indexOf(' D ')!==-1?'log-D':'log-I';
      return '<div class="'+c+'">'+esc(l)+'</div>';
    }).join('');
    if (_autoScroll) el.scrollTop = el.scrollHeight;
  } catch(e) {}
}
function clearLogs() { document.getElementById('logbox').innerHTML=''; _logCount=0; }
document.getElementById('logbox').addEventListener('scroll', function() {
  _autoScroll = this.scrollTop + this.clientHeight >= this.scrollHeight - 20;
});

// ── Init ───────────────────────────────────────────────────────────────────
loadConfig();
pollLogs();
refreshHABadge();
setInterval(pollLogs, 2500);
setInterval(refreshHABadge, 8000);
</script>
</body>
</html>
"""

# ---------------------------------------------------------------------------
# Device simulator HTML
# Mirrors the LVGL ui_manager layout pixel-for-pixel on an 800×1280 canvas.
# Same safety rules as INDEX_HTML apply (see comments above INDEX_HTML).
# ---------------------------------------------------------------------------

DEVICE_HTML = """\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Device Simulator</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%;background:#111;overflow:hidden;font-family:system-ui,sans-serif}
#toolbar{display:flex;align-items:center;gap:10px;padding:8px 16px;
  background:#161b22;border-bottom:1px solid #30363d;font-size:13px;color:#8b949e}
#toolbar a{color:#40b0f0;text-decoration:none}
#src-badge{margin-left:auto;font-size:11px;padding:2px 9px;border-radius:12px;
  border:1px solid #30363d;color:#8b949e;transition:all .3s}
#src-badge.live{border-color:#40c080;color:#40c080}
#src-badge.mock{border-color:#f0c040;color:#f0c040}
#wrap{display:flex;align-items:flex-start;justify-content:center;
  height:calc(100vh - 41px);overflow:hidden;padding:12px}
#screen{width:800px;height:1280px;background:#0d1117;position:relative;overflow:hidden;
  border:1px solid #30363d;border-radius:6px;box-shadow:0 0 60px rgba(0,0,0,.9);
  transform-origin:top center;flex-shrink:0}
/* ── Fixed header elements ── */
.w-in{position:absolute;top:32px;left:24px;color:#40b0f0;font-size:16px;line-height:1.5;text-align:center}
.w-out{position:absolute;top:32px;right:24px;color:#8b949e;font-size:16px;line-height:1.5;text-align:center}
.s-title{position:absolute;top:30px;left:0;right:0;text-align:center;color:#e6edf3;font-size:22px;font-weight:600}
#clock{position:absolute;top:58px;left:0;right:0;text-align:center;color:#8b949e;font-size:13px;letter-spacing:.08em}
.s-sub{position:absolute;top:76px;left:0;right:0;text-align:center;color:#8b949e;font-size:16px}
.s-div{position:absolute;top:106px;left:calc(50% - 340px);width:680px;height:1px;background:#30363d}
/* ── Tile grid ── */
#grid{position:absolute;top:316px;left:calc(50% - 360px);width:720px;height:740px;
  display:flex;flex-wrap:wrap;justify-content:space-evenly;align-content:center;gap:20px}
.tile{width:320px;height:320px;background:#2a2f38;border:1px solid #30363d;border-radius:20px;
  display:flex;flex-direction:column;align-items:center;justify-content:center;
  cursor:pointer;position:relative;transition:background .15s,border-color .15s;
  user-select:none;-webkit-tap-highlight-color:transparent;overflow:hidden}
.tile .ico{font-size:42px;line-height:1;
  position:absolute;top:50%;transform:translateY(calc(-50% - 28px))}
.tile .lbl{position:absolute;top:50%;transform:translateY(calc(-50% + 28px));
  font-size:20px;text-align:center;padding:0 12px;line-height:1.2}
.tile .hint{position:absolute;bottom:16px;left:0;right:0;text-align:center;
  font-size:11px;color:#8b949e;opacity:0.6}
/* Sensor tile: big value display */
.tile.sensor .sval{position:absolute;top:50%;transform:translateY(-50%);
  font-size:52px;font-weight:700;color:#e6edf3;text-align:center;width:100%;line-height:1}
.tile.sensor .sunit{position:absolute;top:50%;transform:translateY(calc(-50% + 44px));
  font-size:18px;color:#8b949e;text-align:center;width:100%}
.tile.sensor .lbl{font-size:16px;top:50%;transform:translateY(calc(-50% + 80px))}
/* Scene tile: star-burst tint on tap */
.tile.scene .ico{color:#f0c040}
.tile.scene:active{background:#302800 !important}
/* Badge: notification dot in top-right corner */
.badge{position:absolute;top:12px;right:14px;width:16px;height:16px;
  border-radius:50%;border:2px solid #0d1117;z-index:2}
.badge.alert{background:#e05050}
.badge.ok   {background:#40c080}
/* ── Page dots ── */
#page-dots{position:absolute;bottom:68px;left:0;right:0;
  display:flex;justify-content:center;gap:12px}
.dot{width:12px;height:12px;border-radius:50%;background:#2a2f38;
  border:2px solid #30363d;cursor:pointer;transition:all .2s}
.dot.active{background:#40c080;border-color:#40c080}
/* ── Footer ── */
#footer{position:absolute;bottom:28px;left:0;right:0;text-align:center;font-size:14px;color:#f0c040}
/* ── Detail modal (centered floating card) ── */
#overlay{position:absolute;inset:0;background:rgba(0,0,0,.65);z-index:8;
  opacity:0;pointer-events:none;transition:opacity .25s}
#overlay.show{opacity:1;pointer-events:auto}
#detail{
  position:absolute;
  top:50%;left:50%;
  width:660px;max-height:820px;
  background:#161b22;
  border-radius:28px;
  box-shadow:0 32px 100px rgba(0,0,0,.85);
  border:1px solid #30363d;
  z-index:9;
  transform:translate(-50%,-50%) scale(.88);
  opacity:0;pointer-events:none;
  transition:transform .22s ease-out,opacity .18s;
  overflow:hidden;
}
#detail.open{transform:translate(-50%,-50%) scale(1);opacity:1;pointer-events:auto}
.d-header{display:flex;align-items:center;padding:20px 24px 16px;
  border-bottom:1px solid #30363d}
.d-name{font-size:20px;font-weight:600;color:#e6edf3;flex:1;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis}
.d-state{font-size:13px;color:#8b949e;margin-right:12px;flex-shrink:0}
.d-close{background:none;border:none;color:#8b949e;font-size:28px;cursor:pointer;
  line-height:1;padding:0 2px;flex-shrink:0}
.d-close:hover{color:#e6edf3}
.d-body{padding:20px 24px;overflow-y:auto;max-height:calc(820px - 78px)}
/* Detail sections */
.d-sect{margin-bottom:24px}
.d-sect-title{font-size:11px;letter-spacing:.1em;text-transform:uppercase;
  color:#8b949e;margin-bottom:10px}
/* Brightness slider */
.bright-row{display:flex;align-items:center;gap:16px}
.bright-row input[type=range]{flex:1;height:8px;border-radius:4px;
  accent-color:#f0c040;cursor:pointer}
.bright-val{width:44px;text-align:right;color:#e6edf3;font-size:18px;font-weight:600}
/* Color presets */
.color-swatches{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:12px}
.swatch{width:64px;height:44px;border-radius:10px;border:2px solid transparent;
  cursor:pointer;display:flex;align-items:center;justify-content:center;
  font-size:18px;transition:transform .1s,border-color .1s}
.swatch:active{transform:scale(.93)}
.swatch.sel{border-color:#e6edf3}
.color-pick-row{display:flex;align-items:center;gap:12px}
.color-pick-row input[type=color]{width:64px;height:44px;border:none;
  border-radius:10px;cursor:pointer;padding:2px;background:none}
.color-pick-row span{color:#8b949e;font-size:13px}
/* Schedule */
.sched-list{display:flex;flex-direction:column;gap:8px;margin-bottom:12px}
.sched-item{display:flex;align-items:center;gap:10px;background:#1c2128;
  border-radius:10px;padding:10px 14px}
.sched-time{font-size:18px;font-weight:600;color:#e6edf3;width:54px}
.sched-bri{font-size:14px;color:#f0c040;width:40px}
.sched-col{width:24px;height:24px;border-radius:6px;flex-shrink:0}
.sched-del{margin-left:auto;background:none;border:none;color:#8b949e;
  font-size:20px;cursor:pointer;line-height:1}
.sched-del:hover{color:#e05050}
.sched-add{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.sched-add input{background:#0d1117;border:1px solid #30363d;border-radius:8px;
  color:#e6edf3;padding:8px 10px;font-size:15px}
.sched-add input[type=time]{width:112px}
.sched-add input[type=number]{width:72px}
.sched-add input[type=color]{width:52px;height:36px;padding:2px;border-radius:8px;cursor:pointer}
.btn-add-sched{background:#40c080;border:none;border-radius:8px;
  color:#0d1117;font-size:14px;font-weight:600;padding:8px 16px;cursor:pointer}
.btn-add-sched:hover{background:#50d090}
/* Climate / Media controls */
.temp-row{display:flex;align-items:center;justify-content:center;gap:24px}
.temp-btn{width:80px;height:80px;border-radius:50%;background:#1c2128;border:2px solid #30363d;
  color:#e6edf3;font-size:36px;cursor:pointer;display:flex;align-items:center;justify-content:center}
.temp-btn:active{background:#30363d}
.temp-display{font-size:56px;font-weight:600;color:#e6edf3;min-width:140px;text-align:center}
.mode-row{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}
.mode-btn{flex:1;min-width:100px;padding:14px 8px;border-radius:12px;border:2px solid #30363d;
  background:#1c2128;color:#8b949e;font-size:15px;cursor:pointer;text-align:center}
.mode-btn.active{border-color:#40c080;color:#40c080}
.vol-row{display:flex;align-items:center;gap:16px}
.vol-row input[type=range]{flex:1;height:8px;accent-color:#40b0f0;cursor:pointer}
.media-btns{display:flex;justify-content:center;gap:20px;margin-top:8px}
.media-btn{width:80px;height:80px;border-radius:50%;background:#1c2128;border:2px solid #30363d;
  color:#e6edf3;font-size:30px;cursor:pointer;display:flex;align-items:center;justify-content:center}
.media-btn:active{background:#30363d}
/* Cover slider */
.cover-row{display:flex;align-items:center;gap:16px}
.cover-row input[type=range]{flex:1;height:8px;accent-color:#8b949e;cursor:pointer}
</style>
</head>
<body>
<div id="toolbar">
  <a href="/">&#8592; Admin</a>
  <span style="color:#30363d">|</span>
  <span>Device Simulator</span>
  <span style="margin-left:4px;color:#555;font-size:11px">800&#215;1280</span>
  <span id="src-badge">&#8212;</span>
</div>
<div id="wrap"><div id="screen">
  <div class="w-in"  id="w-in">In<br>--.-&#176;C</div>
  <div class="w-out" id="w-out">Out<br>--.-&#176;C<br>---</div>
  <div class="s-title">Smart Home</div>
  <div id="clock">--:--</div>
  <div class="s-sub" id="page-name">&#8212;</div>
  <div class="s-div"></div>
  <div id="grid"></div>
  <div id="page-dots"></div>
  <div id="footer">&#9473;&#9473; Connecting... &#9473;&#9473;</div>
  <div id="overlay"></div>
  <div id="detail">
    <div class="d-header">
      <span class="d-name" id="d-name">&#8212;</span>
      <span class="d-state" id="d-state"></span>
      <button class="d-close" id="d-close">&#215;</button>
    </div>
    <div class="d-body" id="d-body"></div>
  </div>
</div></div>

<script>
(function(){
  var ICON_MAP = {
    power:"&#9211;",play:"&#9654;",home:"&#8962;",bell:"&#128276;",
    wifi:"&#8776;",settings:"&#9881;",charge:"&#9889;",loop:"&#8635;",
    ok:"&#10003;",edit:"&#9998;",list:"&#9776;",gps:"&#9685;",
    image:"&#128444;",audio:"&#9834;"
  };
  var ON_STATES = new Set(["on","home","open","playing","heat","cool","auto","unlocked"]);
  var COND_MAP = {
    "sunny":"Sunny","clear-night":"Clear","partlycloudy":"Part.cloudy",
    "cloudy":"Cloudy","rainy":"Rainy","pouring":"Pouring","snowy":"Snowy",
    "snowy-rainy":"Sleet","windy":"Windy","windy-variant":"Windy",
    "fog":"Foggy","hail":"Hail","lightning":"Storm","lightning-rainy":"Thunderstorm"
  };
  var COLOR_PRESETS = [
    {name:"Warm",   hex:"#ffd080", label:"&#9728;"},
    {name:"Cool",   hex:"#ddeeff", label:"&#10052;"},
    {name:"Red",    hex:"#ff4040", label:"&#9679;"},
    {name:"Orange", hex:"#ff8020", label:"&#9679;"},
    {name:"Green",  hex:"#40e080", label:"&#9679;"},
    {name:"Blue",   hex:"#4080ff", label:"&#9679;"},
    {name:"Purple", hex:"#c040ff", label:"&#9679;"},
    {name:"White",  hex:"#ffffff", label:"&#9675;"}
  ];

  var _cfg      = null;
  var _states   = {};
  var _tiles    = [];     /* [{el, entityId, accent, tileCfg}] */
  var _curPage  = 0;
  var _detailEid = "";
  var _detailTileCfg = null;

  /* ── Scale canvas to viewport ── */
  function fitScreen() {
    var wrap = document.getElementById("wrap");
    var scr  = document.getElementById("screen");
    var scale = Math.min((wrap.clientHeight - 24) / 1280,
                         (wrap.clientWidth  - 24) / 800);
    scr.style.transform = "scale(" + scale + ")";
    wrap.style.height   = Math.round(1280 * scale + 24) + "px";
  }

  /* ── Page management ── */
  function goPage(idx) {
    if (!_cfg) return;
    var pages = _cfg.pages || [];
    _curPage = Math.max(0, Math.min(pages.length - 1, idx));
    buildGrid(_cfg);
  }

  function buildDots(count) {
    var el = document.getElementById("page-dots");
    el.innerHTML = "";
    for (var i = 0; i < count; i++) {
      var d = document.createElement("div");
      d.className = "dot" + (i === _curPage ? " active" : "");
      d.dataset.page = String(i);
      d.addEventListener("click", function() { goPage(parseInt(this.dataset.page)); });
      el.appendChild(d);
    }
  }

  /* ── Build tile grid for current page ── */
  function buildGrid(cfg) {
    _cfg   = cfg;
    _tiles = [];
    var grid  = document.getElementById("grid");
    grid.innerHTML = "";
    var pages = cfg.pages || [];
    var page  = pages[_curPage] || {};
    document.getElementById("page-name").textContent = page.name || "";

    var wx = page.weather || {};
    buildGrid._inEid  = wx.indoor_entity  || "";
    buildGrid._outEid = wx.outdoor_entity || "";

    buildDots(pages.length);

    (page.tiles || []).forEach(function(t) {
      var accent = "#" + (t.color || "888888");
      var eid    = t.entity || "";
      var domain = eid.split(".")[0] || "";
      var isSensor = (domain === "sensor" || domain === "binary_sensor" || domain === "input_number");
      var isScene  = (domain === "scene" || domain === "script" || domain === "automation");
      var div = document.createElement("div");
      div.dataset.eid = eid;

      if (isSensor) {
        /* Sensor tile: large value display, no toggle */
        div.className = "tile sensor";
        div.innerHTML = (
          "<span class=\\"sval\\" id=\\"sv-" + esc(eid) + "\\">--</span>" +
          "<span class=\\"sunit\\" id=\\"su-" + esc(eid) + "\\"></span>" +
          "<span class=\\"lbl\\">" + esc(t.label || eid) + "</span>"
        );
        div.style.borderColor = accent;
        div.querySelector(".lbl").style.color = accent;
        attachTile(div, t, accent);  /* long-press still opens info */

      } else if (isScene) {
        /* Scene tile: single tap triggers, no long-press detail */
        div.className = "tile scene";
        div.innerHTML = (
          "<span class=\\"ico\\">&#10024;</span>" +
          "<span class=\\"lbl\\">" + esc(t.label || eid) + "</span>"
        );
        div.querySelector(".ico").style.color = accent;
        div.style.borderColor = accent;
        div.addEventListener("click", function() { triggerScene(eid, div, accent); });

      } else {
        /* Standard toggleable tile */
        div.className = "tile";
        div.innerHTML = (
          "<span class=\\"ico\\">" + (ICON_MAP[t.icon] || "&#9881;") + "</span>" +
          "<span class=\\"lbl\\">" + esc(t.label || "?") + "</span>" +
          "<span class=\\"hint\\">hold</span>"
        );
        div.querySelector(".ico").style.color = accent;
        attachTile(div, t, accent);
      }

      /* Alert badge for door/lock/motion sensors */
      var BADGE_DOMAINS = new Set(["binary_sensor","lock","alarm_control_panel"]);
      if (BADGE_DOMAINS.has(domain)) {
        var badge = document.createElement("div");
        badge.className = "badge";
        badge.id = "badge-" + eid.replace(/\\./g,"-");
        div.appendChild(badge);
      }

      grid.appendChild(div);
      _tiles.push({el: div, entityId: eid, accent: accent, tileCfg: t,
                   isSensor: isSensor, isScene: isScene});
    });

    applyStates(_states);
  }

  function esc(s) {
    return String(s)
      .replace(/&/g,"&amp;").replace(/</g,"&lt;")
      .replace(/>/g,"&gt;").replace(/"/g,"&quot;");
  }

  /* ── Long-press vs tap ── */
  function attachTile(el, tileCfg, accent) {
    var timer = null;
    var moved = false;
    var sx = 0, sy = 0;

    function onStart(cx, cy) {
      moved = false; sx = cx; sy = cy;
      timer = setTimeout(function() {
        if (!moved) openDetail(el.dataset.eid, tileCfg, accent);
      }, 550);
    }
    function onMove(cx, cy) {
      if (Math.abs(cx - sx) > 8 || Math.abs(cy - sy) > 8) { moved = true; clearTimeout(timer); }
    }
    function onEnd() {
      if (!moved) {
        clearTimeout(timer);
        tapTile(el, accent);
      }
    }
    el.addEventListener("touchstart", function(e){
      var t = e.touches[0]; onStart(t.clientX, t.clientY);
    }, {passive:true});
    el.addEventListener("touchmove",  function(e){
      var t = e.touches[0]; onMove(t.clientX, t.clientY);
    }, {passive:true});
    el.addEventListener("touchend",   onEnd);
    el.addEventListener("mousedown",  function(e){ onStart(e.clientX, e.clientY); });
    el.addEventListener("mousemove",  function(e){ if(e.buttons) onMove(e.clientX, e.clientY); });
    el.addEventListener("mouseup",    onEnd);
    el.addEventListener("mouseleave", function(){ clearTimeout(timer); });
  }

  /* Swipe between pages on the screen background */
  (function(){
    var sx = 0, sy = 0;
    var scr = document.getElementById("screen");
    scr.addEventListener("touchstart", function(e){
      sx = e.touches[0].clientX; sy = e.touches[0].clientY;
    }, {passive:true});
    scr.addEventListener("touchend", function(e){
      var dx = e.changedTouches[0].clientX - sx;
      var dy = e.changedTouches[0].clientY - sy;
      if (Math.abs(dx) > Math.abs(dy) * 1.5 && Math.abs(dx) > 50) {
        goPage(_curPage + (dx < 0 ? 1 : -1));
      }
    });
  })();

  /* ── Apply entity states to tiles ── */
  function applyStates(states) {
    _states = states;
    _tiles.forEach(function(ts) {
      if (!ts.entityId) return;
      var ent = states[ts.entityId] || {};
      var s   = ent.state || "off";
      var on  = ON_STATES.has(s);
      var attr = ent.attributes || {};

      if (ts.isSensor) {
        /* Sensor tile: update value display */
        var valEl  = document.getElementById("sv-" + ts.entityId.replace(/\\./g,"-"));
        var unitEl = document.getElementById("su-" + ts.entityId.replace(/\\./g,"-"));
        if (valEl) {
          /* Format: truncate long decimals, show integer if >100 */
          var num = parseFloat(s);
          valEl.textContent = isNaN(num) ? s : (Math.abs(num) >= 100 ? Math.round(num) : num.toFixed(1));
        }
        if (unitEl) {
          unitEl.textContent = attr.unit_of_measurement || "";
        }
        /* Binary sensor: colour tile */
        if (ts.entityId.startsWith("binary_sensor.")) {
          ts.el.style.background  = on ? ts.accent : "#2a2f38";
          ts.el.style.borderColor = on ? ts.accent : "#30363d";
        }

      } else if (!ts.isScene) {
        /* Standard tile */
        ts.el.style.background  = on ? ts.accent : "#2a2f38";
        ts.el.style.borderColor = on ? ts.accent : "#30363d";
        ts.el.querySelector(".lbl").style.color = on ? "#0d1117" : "#e6edf3";
        ts.el.querySelector(".ico").style.color = on ? "#0d1117" : ts.accent;

        /* Brightness hint on light tiles */
        var hint = ts.el.querySelector(".hint");
        if (hint && ts.entityId.startsWith("light.")) {
          var bpct = attr.brightness ? Math.round(attr.brightness / 2.55) : null;
          hint.textContent = on && bpct != null ? bpct + "%" : "hold";
        }
      }

      /* Badge update (door/lock/binary_sensor) */
      var badge = document.getElementById("badge-" + ts.entityId.replace(/\\./g,"-"));
      if (badge) {
        var alertState = (s === "on" || s === "unlocked" || s === "open" || s === "triggered");
        badge.className = "badge " + (alertState ? "alert" : "ok");
        badge.style.display = (s === "off" || s === "locked" || s === "closed") ? "none" : "";
      }
    });

    var inEid = buildGrid._inEid || "";
    if (inEid && states[inEid]) {
      document.getElementById("w-in").innerHTML =
        "In<br>" + parseFloat(states[inEid].state).toFixed(1) + "&#176;C";
    }
    var outEid = buildGrid._outEid || "";
    if (outEid && states[outEid]) {
      var o = states[outEid];
      var oT = (o.attributes && o.attributes.temperature != null)
               ? parseFloat(o.attributes.temperature).toFixed(1) + "&#176;C" : "--.-&#176;C";
      document.getElementById("w-out").innerHTML =
        "Out<br>" + oT + "<br>" + (COND_MAP[o.state] || o.state || "---");
    }
  }

  /* ── Scene trigger (single tap, no toggle) ── */
  function triggerScene(eid, el, accent) {
    el.style.background = accent;
    setTimeout(function() { el.style.background = "#2a2f38"; }, 500);
    fetch("/ha/toggle",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({entity_id:eid})}).catch(function(){});
  }

  /* ── Tap: quick toggle ── */
  function tapTile(el, accent) {
    var eid = el.dataset.eid;
    if (!eid) return;
    /* Sensor tiles: don't toggle, open detail on short tap */
    if (el.classList.contains("sensor")) { openDetail(eid, _tiles.find(function(t){return t.el===el;})||{tileCfg:{label:eid}}, accent); return; }
    var curOn = ON_STATES.has((_states[eid] || {}).state || "off");
    el.style.background  = curOn ? "#2a2f38" : accent;
    el.style.borderColor = curOn ? "#30363d" : accent;
    el.querySelector(".lbl").style.color = curOn ? "#e6edf3" : "#0d1117";
    el.querySelector(".ico").style.color = curOn ? accent : "#0d1117";
    fetch("/ha/toggle",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({entity_id:eid})})
    .then(function(r){return r.json();})
    .then(function(d){
      if (d.new_state && _states[eid])
        _states[eid] = Object.assign({},_states[eid],{state:d.new_state});
      applyStates(_states);
    }).catch(function(){});
  }

  /* ── Detail drawer ── */
  function openDetail(eid, tileCfg, accent) {
    _detailEid = eid;
    _detailTileCfg = tileCfg;
    var domain = eid.split(".")[0] || "";
    var ent    = _states[eid] || {};
    var name   = (ent.attributes && ent.attributes.friendly_name) || tileCfg.label || eid;
    document.getElementById("d-name").textContent  = name;
    document.getElementById("d-state").textContent = ent.state || "";
    document.getElementById("d-body").innerHTML    = buildDetailBody(domain, eid, ent, tileCfg, accent);
    bindDetailEvents(domain, eid, ent, tileCfg);
    document.getElementById("detail").classList.add("open");
    document.getElementById("overlay").classList.add("show");
  }

  function closeDetail() {
    document.getElementById("detail").classList.remove("open");
    document.getElementById("overlay").classList.remove("show");
    _detailEid = "";
  }

  document.getElementById("d-close").addEventListener("click", closeDetail);
  document.getElementById("overlay").addEventListener("click", closeDetail);

  function buildDetailBody(domain, eid, ent, tileCfg, accent) {
    var attr = ent.attributes || {};
    if (domain === "light")        return buildLightDetail(eid, ent, attr, tileCfg);
    if (domain === "climate")      return buildClimateDetail(eid, ent, attr);
    if (domain === "media_player") return buildMediaDetail(eid, ent, attr);
    if (domain === "cover")        return buildCoverDetail(eid, ent, attr);
    return buildGenericDetail(eid, ent, accent);
  }

  /* ---- LIGHT ---- */
  function buildLightDetail(eid, ent, attr, tileCfg) {
    var bri   = attr.brightness ? Math.round(attr.brightness / 2.55) : 80;
    var scheds = (tileCfg.schedules || []);
    var schedHtml = scheds.map(function(s, i) {
      var colStyle = s.color_hex ? "background:#" + s.color_hex : "background:#888";
      return ("<div class=\\"sched-item\\">" +
        "<span class=\\"sched-time\\">" + esc(s.time) + "</span>" +
        "<span class=\\"sched-bri\\">" + s.brightness_pct + "%</span>" +
        "<div class=\\"sched-col\\" style=\\"" + colStyle + "\\"></div>" +
        "<button class=\\"sched-del\\" data-idx=\\"" + i + "\\">&#215;</button>" +
      "</div>");
    }).join("");
    var swatchHtml = COLOR_PRESETS.map(function(c) {
      return ("<div class=\\"swatch\\" data-hex=\\"" + c.hex + "\\" " +
        "style=\\"background:" + c.hex + "\\" title=\\"" + c.name + "\\">" +
        c.label + "</div>");
    }).join("");
    return (
      "<div class=\\"d-sect\\">" +
        "<div class=\\"d-sect-title\\">Brightness</div>" +
        "<div class=\\"bright-row\\">" +
          "<input type=\\"range\\" id=\\"bri-slider\\" min=\\"1\\" max=\\"100\\" value=\\"" + bri + "\\">" +
          "<span class=\\"bright-val\\" id=\\"bri-val\\">" + bri + "%</span>" +
        "</div>" +
      "</div>" +
      "<div class=\\"d-sect\\">" +
        "<div class=\\"d-sect-title\\">Color</div>" +
        "<div class=\\"color-swatches\\">" + swatchHtml + "</div>" +
        "<div class=\\"color-pick-row\\">" +
          "<input type=\\"color\\" id=\\"color-custom\\" value=\\"#ffffff\\">" +
          "<span>Custom color</span>" +
        "</div>" +
      "</div>" +
      "<div class=\\"d-sect\\">" +
        "<div class=\\"d-sect-title\\">Schedule</div>" +
        "<div class=\\"sched-list\\" id=\\"sched-list\\">" + schedHtml + "</div>" +
        "<div class=\\"sched-add\\">" +
          "<input type=\\"time\\" id=\\"sched-time\\" value=\\"07:00\\">" +
          "<input type=\\"number\\" id=\\"sched-bri\\" min=\\"1\\" max=\\"100\\" value=\\"80\\" placeholder=\\"%\\">" +
          "<input type=\\"color\\" id=\\"sched-color\\" value=\\"#ffd080\\">" +
          "<button class=\\"btn-add-sched\\" id=\\"btn-add-sched\\">+ Add</button>" +
        "</div>" +
      "</div>"
    );
  }

  /* ---- CLIMATE ---- */
  function buildClimateDetail(eid, ent, attr) {
    var setpt = attr.temperature || 21;
    var mode  = ent.state || "off";
    var modes = ["heat","cool","auto","off"];
    var modeHtml = modes.map(function(m) {
      return ("<div class=\\"mode-btn" + (m===mode?" active":"") + "\\" data-mode=\\"" + m + "\\">" +
        {heat:"&#128293; Heat",cool:"&#10052; Cool",auto:"&#9851; Auto",off:"&#9679; Off"}[m] +
      "</div>");
    }).join("");
    return (
      "<div class=\\"d-sect\\">" +
        "<div class=\\"d-sect-title\\">Temperature setpoint</div>" +
        "<div class=\\"temp-row\\">" +
          "<button class=\\"temp-btn\\" id=\\"temp-dn\\">&#8722;</button>" +
          "<div class=\\"temp-display\\" id=\\"temp-val\\">" + setpt + "&#176;</div>" +
          "<button class=\\"temp-btn\\" id=\\"temp-up\\">+</button>" +
        "</div>" +
        "<div class=\\"mode-row\\" id=\\"mode-row\\">" + modeHtml + "</div>" +
      "</div>"
    );
  }

  /* ---- MEDIA PLAYER ---- */
  function buildMediaDetail(eid, ent, attr) {
    var vol = attr.volume_level != null ? Math.round(attr.volume_level * 100) : 50;
    return (
      "<div class=\\"d-sect\\">" +
        "<div class=\\"d-sect-title\\">Playback</div>" +
        "<div class=\\"media-btns\\">" +
          "<button class=\\"media-btn\\" id=\\"med-prev\\">&#9198;</button>" +
          "<button class=\\"media-btn\\" id=\\"med-play\\">" +
            (ent.state==="playing" ? "&#9646;&#9646;" : "&#9654;") + "</button>" +
          "<button class=\\"media-btn\\" id=\\"med-next\\">&#9197;</button>" +
        "</div>" +
      "</div>" +
      "<div class=\\"d-sect\\">" +
        "<div class=\\"d-sect-title\\">Volume</div>" +
        "<div class=\\"vol-row\\">" +
          "<input type=\\"range\\" id=\\"vol-slider\\" min=\\"0\\" max=\\"100\\" value=\\"" + vol + "\\">" +
          "<span class=\\"bright-val\\" id=\\"vol-val\\">" + vol + "%</span>" +
        "</div>" +
      "</div>"
    );
  }

  /* ---- COVER ---- */
  function buildCoverDetail(eid, ent, attr) {
    var pos = attr.current_position != null ? attr.current_position : 0;
    return (
      "<div class=\\"d-sect\\">" +
        "<div class=\\"d-sect-title\\">Position</div>" +
        "<div class=\\"cover-row\\">" +
          "<input type=\\"range\\" id=\\"cover-pos\\" min=\\"0\\" max=\\"100\\" value=\\"" + pos + "\\">" +
          "<span class=\\"bright-val\\" id=\\"cover-val\\">" + pos + "%</span>" +
        "</div>" +
        "<div class=\\"mode-row\\">" +
          "<button class=\\"mode-btn\\" id=\\"cov-open\\">&#9650; Open</button>" +
          "<button class=\\"mode-btn\\" id=\\"cov-stop\\">&#9646;&#9646; Stop</button>" +
          "<button class=\\"mode-btn\\" id=\\"cov-close\\">&#9660; Close</button>" +
        "</div>" +
      "</div>"
    );
  }

  /* ---- GENERIC ---- */
  function buildGenericDetail(eid, ent, accent) {
    return (
      "<div class=\\"d-sect\\">" +
        "<div class=\\"d-sect-title\\">State: " + esc(ent.state||"unknown") + "</div>" +
        "<div style=\\"margin-top:24px\\">" +
          "<button class=\\"btn-add-sched\\" style=\\"width:100%;font-size:18px;padding:18px;\\"" +
          " id=\\"gen-toggle\\">Toggle</button>" +
        "</div>" +
      "</div>"
    );
  }

  /* ── Bind detail events (called after innerHTML is set) ── */
  function bindDetailEvents(domain, eid, ent, tileCfg) {
    if (domain === "light")        bindLightEvents(eid, tileCfg);
    if (domain === "climate")      bindClimateEvents(eid, ent);
    if (domain === "media_player") bindMediaEvents(eid);
    if (domain === "cover")        bindCoverEvents(eid);
    if (domain !== "light" && domain !== "climate" &&
        domain !== "media_player" && domain !== "cover") {
      var gb = document.getElementById("gen-toggle");
      if (gb) gb.addEventListener("click", function() {
        fetch("/ha/toggle",{method:"POST",headers:{"Content-Type":"application/json"},
          body:JSON.stringify({entity_id:eid})}).then(function(){closeDetail();});
      });
    }
  }

  function bindLightEvents(eid, tileCfg) {
    var slider = document.getElementById("bri-slider");
    var valEl  = document.getElementById("bri-val");
    if (slider) {
      slider.addEventListener("input", function() {
        valEl.textContent = this.value + "%";
      });
      slider.addEventListener("change", function() {
        setLight(eid, {brightness_pct: parseInt(this.value)});
      });
    }
    document.querySelectorAll(".swatch").forEach(function(sw) {
      sw.addEventListener("click", function() {
        document.querySelectorAll(".swatch").forEach(function(x){x.classList.remove("sel");});
        this.classList.add("sel");
        var hs = hexToHs(this.dataset.hex);
        setLight(eid, {hs_color: hs});
      });
    });
    var cc = document.getElementById("color-custom");
    if (cc) cc.addEventListener("change", function() {
      var hs = hexToHs(this.value);
      setLight(eid, {hs_color: hs});
    });
    /* Schedules */
    document.querySelectorAll(".sched-del").forEach(function(btn) {
      btn.addEventListener("click", function() {
        var idx = parseInt(this.dataset.idx);
        var scheds = (tileCfg.schedules || []).slice();
        scheds.splice(idx, 1);
        saveTileSchedules(eid, scheds);
      });
    });
    var addBtn = document.getElementById("btn-add-sched");
    if (addBtn) addBtn.addEventListener("click", function() {
      var t   = document.getElementById("sched-time").value;
      var b   = parseInt(document.getElementById("sched-bri").value);
      var col = document.getElementById("sched-color").value.replace("#","");
      if (!t || isNaN(b)) return;
      var scheds = (tileCfg.schedules || []).slice();
      scheds.push({time: t, brightness_pct: b, color_hex: col});
      scheds.sort(function(a,b){ return a.time < b.time ? -1 : 1; });
      saveTileSchedules(eid, scheds);
    });
  }

  function bindClimateEvents(eid, ent) {
    var setpt = [(ent.attributes||{}).temperature || 21];
    var valEl = document.getElementById("temp-val");
    var upd = function(delta) {
      setpt[0] = Math.round((setpt[0] + delta) * 2) / 2;
      valEl.innerHTML = setpt[0] + "&#176;";
      fetch("/ha/toggle",{method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify({entity_id:eid})}).catch(function(){});
    };
    var dn = document.getElementById("temp-dn");
    var up = document.getElementById("temp-up");
    if (dn) dn.addEventListener("click", function(){upd(-0.5);});
    if (up) up.addEventListener("click", function(){upd(0.5);});
    document.querySelectorAll(".mode-btn").forEach(function(btn) {
      btn.addEventListener("click", function() {
        document.querySelectorAll(".mode-btn").forEach(function(b){b.classList.remove("active");});
        this.classList.add("active");
      });
    });
  }

  function bindMediaEvents(eid) {
    var slider = document.getElementById("vol-slider");
    var valEl  = document.getElementById("vol-val");
    if (slider) slider.addEventListener("input", function(){valEl.textContent=this.value+"%";});
    var pp = document.getElementById("med-play");
    if (pp) pp.addEventListener("click", function(){
      fetch("/ha/toggle",{method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify({entity_id:eid})}).catch(function(){});
    });
  }

  function bindCoverEvents(eid) {
    var slider = document.getElementById("cover-pos");
    var valEl  = document.getElementById("cover-val");
    if (slider) slider.addEventListener("input",function(){valEl.textContent=this.value+"%";});
    var open  = document.getElementById("cov-open");
    var stop  = document.getElementById("cov-stop");
    var close = document.getElementById("cov-close");
    function callCover(svc) {
      fetch("/ha/toggle",{method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify({entity_id:eid})}).catch(function(){});
    }
    if (open)  open.addEventListener("click",  function(){callCover("open_cover");});
    if (stop)  stop.addEventListener("click",  function(){callCover("stop_cover");});
    if (close) close.addEventListener("click", function(){callCover("close_cover");});
  }

  /* ── HA light control helper ── */
  function setLight(eid, params) {
    params.entity_id = eid;
    fetch("/ha/light",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify(params)}).catch(function(){});
  }

  /* ── Color conversion hex → [h, s] ── */
  function hexToHs(hex) {
    var r = parseInt(hex.slice(1,3),16)/255;
    var g = parseInt(hex.slice(3,5),16)/255;
    var b = parseInt(hex.slice(5,7),16)/255;
    var max=Math.max(r,g,b), min=Math.min(r,g,b), d=max-min, h=0, s=0;
    if (d > 0) {
      s = d / max;
      if      (max===r) h=(g-b)/d+(g<b?6:0);
      else if (max===g) h=(b-r)/d+2;
      else              h=(r-g)/d+4;
      h /= 6;
    }
    return [Math.round(h*360), Math.round(s*100)];
  }

  /* ── Save schedules to config.json ── */
  function saveTileSchedules(eid, scheds) {
    fetch("/config")
    .then(function(r){return r.json();})
    .then(function(cfg){
      var pages = cfg.pages || [];
      pages.forEach(function(page) {
        (page.tiles || []).forEach(function(t) {
          if (t.entity === eid) t.schedules = scheds;
        });
      });
      return fetch("/config",{method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify(cfg)});
    })
    .then(function(){
      /* Update local tileCfg ref and re-open detail */
      if (_detailTileCfg) _detailTileCfg.schedules = scheds;
      openDetail(eid, _detailTileCfg, "#" + (_detailTileCfg.color||"888888"));
    }).catch(function(){});
  }

  /* ── Polling ── */
  function pollConfig() {
    fetch("/config")
    .then(function(r){return r.json();})
    .then(function(cfg){
      var sig = JSON.stringify(cfg.pages);
      if (pollConfig._last !== sig) {
        pollConfig._last = sig;
        if (_curPage >= (cfg.pages||[]).length) _curPage = 0;
        buildGrid(cfg);
      }
    }).catch(function(){});
  }

  function pollStates() {
    fetch("/ha/states")
    .then(function(r){
      var src = r.headers.get("X-Source") || "mock";
      var badge = document.getElementById("src-badge");
      badge.textContent = src==="live" ? "HA live" : "mock";
      badge.className   = src==="live" ? "live"    : "mock";
      return r.json();
    })
    .then(function(st){ applyStates(st); })
    .catch(function(){});
  }

  function pollFooter() {
    fetch("/ha/status")
    .then(function(r){return r.json();})
    .then(function(s){
      var el = document.getElementById("footer");
      if (s.connected) {
        el.textContent = "\\u2248 HA  |  " + (s.entity_count||0) + " entities";
        el.style.color = "#40c080";
      } else {
        el.textContent = "\\u26a0 HA disconnected";
        el.style.color = "#e05050";
      }
    }).catch(function(){});
  }

  /* ── Clock ── */
  function updateClock() {
    var now = new Date();
    var h = String(now.getHours()).padStart(2,"0");
    var m = String(now.getMinutes()).padStart(2,"0");
    var s = String(now.getSeconds()).padStart(2,"0");
    var el = document.getElementById("clock");
    if (el) el.textContent = h + ":" + m + ":" + s;
  }
  updateClock();
  setInterval(updateClock, 1000);

  window.addEventListener("resize", fitScreen);
  fitScreen();
  pollConfig();
  setTimeout(pollStates,  400);
  setTimeout(pollFooter,  600);
  setInterval(pollConfig,  3000);
  setInterval(pollStates,  2000);
  setInterval(pollFooter, 10000);
})();
</script>
</body>
</html>
"""

# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

CAPTIVE_URLS = {
    "/generate_204", "/hotspot-detect.html", "/library/test/success.html",
    "/success.txt",  "/connecttest.txt",     "/ncsi.txt",
    "/redirect",     "/canonical.html",      "/chat",
}

class ReuseAddrServer(HTTPServer):
    """Avoids 'Address already in use' after quick restarts."""
    allow_reuse_address = True


class Handler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        print(f"  \033[90m{self.address_string()} {fmt % args}\033[0m")

    def _json(self, data, code=200, extra_headers=None):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type",  "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _html(self, html, code=200):
        body = html.encode()
        self.send_response(code)
        self.send_header("Content-Type",  "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, loc):
        self.send_response(302)
        self.send_header("Location",       loc)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _body(self):
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n) if n else b""

    def _ha_states_response(self):
        """Return HA states dict formatted for the UI."""
        if _ha.connected:
            states  = _ha.get_states()
            source  = "live"
        else:
            # Build mock-compatible dict
            states  = {k: {"state": v["state"],
                           "attributes": {"friendly_name": v.get("friendly_name", k)},
                           "friendly_name": v.get("friendly_name", k)}
                       for k, v in _mock_states.items()}
            source  = "mock"
        return states, source

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin",  "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        path = urlparse(self.path).path

        if path in ("/", "/index.html"):
            self._html(INDEX_HTML); return

        if path == "/device":
            self._html(DEVICE_HTML); return

        if path == "/config":
            self._json(load_config()); return

        if path == "/wifi/scan":
            push_log("I (wifi): scan started")
            time.sleep(0.3)
            push_log(f"I (wifi): scan complete, {len(MOCK_NETWORKS)} networks")
            self._json(MOCK_NETWORKS); return

        if path == "/logs":
            with _log_lock:
                body = "\n".join(_log_lines)
            b = body.encode()
            self.send_response(200)
            self.send_header("Content-Type",  "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(b); return

        if path == "/ha/states":
            states, src = self._ha_states_response()
            self._json(states, extra_headers={"X-Source": src}); return

        if path == "/ha/status":
            self._json(_ha.status()); return

        if path == "/ha/entity":
            from urllib.parse import parse_qs
            eid = parse_qs(urlparse(self.path).query).get("id", [""])[0]
            if not eid:
                self._json({"error": "missing id"}, 400); return
            if _ha.connected:
                ent = _ha.get_entity(eid)
                if ent:
                    self._json(ent); return
            with _mock_lock:
                mock = dict(_mock_states.get(eid, {}))
            self._json({"entity_id": eid, "state": mock.get("state", "unknown"),
                        "attributes": mock.get("attributes", {})}); return

        if path in CAPTIVE_URLS:
            push_log(f"I (http): captive probe {path}")
            self._redirect(f"http://localhost:{PORT}/"); return

        print(f"  \033[33m[404] {path}\033[0m")
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        path = urlparse(self.path).path
        raw  = self._body()

        if path == "/config":
            try:   data = json.loads(raw)
            except json.JSONDecodeError as e:
                self._json({"error": str(e)}, 400); return
            save_config(data)
            push_log("I (http_srv): config saved via HTTP")
            # Reconfigure HA client if HA settings changed
            ha = data.get("ha", {})
            if ha.get("url") and ha.get("token"):
                _ha.configure(ha["url"], ha["token"])
                threading.Thread(target=_ha.fetch_all_states, daemon=True).start()
            self._json({"ok": True}); return

        if path == "/wifi":
            try:   data = json.loads(raw)
            except json.JSONDecodeError:
                self._json({"error": "invalid json"}, 400); return
            ssid = data.get("ssid", "")
            push_log(f"I (wifi_manager): saved SSID '{ssid}', rebooting in 1s…")
            push_log("W (wifi_manager): [MOCK] reboot skipped in local test mode")
            self._json({"ok": True}); return

        if path == "/ha/toggle":
            try:   data = json.loads(raw)
            except json.JSONDecodeError:
                self._json({"error": "invalid json"}, 400); return
            eid = data.get("entity_id", "")
            if not eid:
                self._json({"error": "missing entity_id"}, 400); return

            if _ha.connected:
                new_state = _ha.toggle(eid)
                if new_state is None:
                    # HA toggle failed — fall through to mock
                    new_state = mock_toggle(eid)
                    push_log(f"W (ha_proxy): real toggle failed, used mock for {eid}")
            else:
                new_state = mock_toggle(eid)
                push_log(f"I (ha_mock): toggle {eid} -> {new_state}")

            self._json({"entity_id": eid, "new_state": new_state}); return

        if path == "/ha/reload":
            if _ha.base_url and _ha.token:
                ok = _ha.fetch_all_states()
                s  = _ha.status()
                self._json({"ok": ok, "entity_count": s["entity_count"],
                            "error": s["error"]}); return
            else:
                self._json({"ok": False, "error": "No HA URL/token configured"}); return

        if path == "/ha/light":
            try:   data = json.loads(raw)
            except json.JSONDecodeError:
                self._json({"error": "invalid json"}, 400); return
            eid = data.get("entity_id", "")
            if not eid:
                self._json({"error": "missing entity_id"}, 400); return
            svc_data = {"entity_id": eid}
            if "brightness_pct" in data:
                svc_data["brightness_pct"] = max(0, min(100, int(data["brightness_pct"])))
            if "rgb_color"  in data: svc_data["rgb_color"]  = data["rgb_color"]
            if "hs_color"   in data: svc_data["hs_color"]   = data["hs_color"]
            if "color_temp" in data: svc_data["color_temp"] = data["color_temp"]
            try:
                if _ha.connected:
                    _ha._post("/api/services/light/turn_on", svc_data)
                    push_log(f"I (ha_proxy): light.turn_on {eid} {svc_data}")
                else:
                    push_log(f"I (ha_mock): light.turn_on {eid} {svc_data}")
                self._json({"ok": True}); return
            except Exception as e:
                push_log(f"E (ha_proxy): light.turn_on failed: {e}")
                self._json({"error": str(e)}, 500); return

        if path == "/ha/disconnect":
            cfg = load_config()
            cfg["ha"] = {"url": "", "token": ""}
            save_config(cfg)
            _ha.base_url   = None
            _ha.token      = None
            _ha.connected  = False
            _ha.error      = None
            with _ha._lock:
                _ha._states = {}
            push_log("W (ha_proxy): disconnected — HA config cleared")
            self._json({"ok": True}); return

        self._json({"error": "not found"}, 404)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    _init_ha_from_config()

    server = ReuseAddrServer(("", PORT), Handler)
    print(f"\n  \033[32m✓ SmartHome portal\033[0m  http://localhost:{PORT}")
    print(f"  Config: {CONFIG_FILE}")
    print(f"  Ctrl-C to stop\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n  Stopped.")
