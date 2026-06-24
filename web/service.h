#pragma once
#include <Arduino.h>

const char SERVICE_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Pylontech Service Center</title>
<style>
:root {
  --bg: #f3f6fb;
  --card: #ffffff;
  --text: #1f2937;
  --muted: #66758c;
  --line: #d9e2ef;
  --primary: #1565d8;
  --primary-dark: #0f4ea9;
  --danger: #bf2f2f;
  --danger-dark: #932424;
  --warning: #b47512;
  --warning-dark: #8f5c0e;
  --ok: #1e8b52;
  --shadow: 0 8px 22px rgba(17, 24, 39, 0.08);
}

* { box-sizing: border-box; }

body {
  margin: 0;
  padding: 18px;
  font-family: "Segoe UI", "Noto Sans", Arial, sans-serif;
  color: var(--text);
  background:
    radial-gradient(circle at 0% 0%, #eef4ff 0%, transparent 38%),
    radial-gradient(circle at 100% 0%, #e5f3ff 0%, transparent 34%),
    var(--bg);
}

.container {
  max-width: 1100px;
  margin: 0 auto;
}

.hero {
  background: linear-gradient(125deg, #0f1c35, #1c3b7d 62%, #1f5cc9);
  color: #fff;
  border-radius: 14px;
  padding: 18px 20px;
  box-shadow: 0 12px 28px rgba(15, 23, 42, 0.24);
  margin-bottom: 16px;
}

.hero h2 {
  margin: 0;
  font-size: 1.45rem;
}

.hero p {
  margin: 8px 0 0;
  color: #d9e7ff;
}

.badge {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  margin-top: 12px;
  border: 1px solid rgba(255, 255, 255, 0.3);
  border-radius: 999px;
  padding: 6px 10px;
  background: rgba(255, 255, 255, 0.14);
  font-size: 0.9rem;
}

.grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(310px, 1fr));
  gap: 14px;
}

.panel {
  background: var(--card);
  border: 1px solid var(--line);
  border-radius: 12px;
  box-shadow: var(--shadow);
  padding: 15px;
}

.panel h3 {
  margin: 0 0 10px;
  font-size: 1.04rem;
}

.panel p {
  margin: 0 0 10px;
  color: var(--muted);
  font-size: 0.92rem;
}

.panel.full {
  grid-column: 1 / -1;
}

.actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

label {
  display: block;
  margin-bottom: 6px;
  font-weight: 600;
  color: #324155;
}

input[type="file"],
input[type="text"],
input[type="number"] {
  width: 100%;
  border: 1px solid #c9d5e6;
  border-radius: 8px;
  padding: 9px 10px;
  background: #fff;
  color: var(--text);
  font: inherit;
}

input[type="checkbox"] {
  transform: translateY(1px);
}

button {
  border: none;
  border-radius: 8px;
  padding: 9px 14px;
  cursor: pointer;
  font: 600 0.92rem/1 "Segoe UI", "Noto Sans", Arial, sans-serif;
  color: #fff;
  background: linear-gradient(180deg, var(--primary), var(--primary-dark));
  transition: transform 0.08s ease, filter 0.14s ease;
}

button:hover { filter: brightness(0.97); }
button:active { transform: translateY(1px); }

.btn-secondary {
  color: #1a2f58;
  background: #e7effd;
}

.btn-warning {
  background: linear-gradient(180deg, var(--warning), var(--warning-dark));
}

.btn-danger {
  background: linear-gradient(180deg, var(--danger), var(--danger-dark));
}

progress {
  width: 100%;
  height: 12px;
  margin-top: 10px;
  display: none;
}

.status {
  margin-top: 8px;
  min-height: 20px;
  font-size: 0.9rem;
  color: var(--muted);
}

.status.ok { color: var(--ok); }
.status.err { color: var(--danger); }
.status.warn { color: var(--warning-dark); }

.table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.94rem;
}

.table td {
  border-bottom: 1px solid #edf1f7;
  padding: 8px 0;
  vertical-align: top;
}

.table td:first-child {
  width: 32%;
  color: #36465c;
  font-weight: 600;
}

.table small {
  color: var(--muted);
}

@media (max-width: 760px) {
  body { padding: 12px; }
  .hero { padding: 14px; }
}
</style>
</head>
<body>

<div class="container">
  <div class="hero">
    <h2>Service Center</h2>
    <p>Firmware-Update, Wartung, Backup und Diagnose.</p>
    <div class="badge">Firmware: <b id="fwver">...</b></div>
  </div>

  <div class="grid">
    <section class="panel">
      <h3>OTA Update</h3>
      <p>Bitte eine gueltige .bin Firmwaredatei auswaehlen.</p>
      <input type="file" id="fw" accept=".bin">
      <div class="actions" style="margin-top:10px;">
        <button onclick="upload()">Firmware flashen</button>
      </div>
      <progress id="prog" value="0" max="100"></progress>
      <div id="status" class="status"></div>
    </section>

    <section class="panel">
      <h3>System Aktionen</h3>
      <p>Aktionen mit Neustart oder Ruecksetzen.</p>
      <div class="actions">
        <button class="btn-secondary" onclick="restartESP()">Restart ESP</button>
        <button class="btn-warning" onclick="wifiReset()">WiFi Reset</button>
        <button class="btn-danger" onclick="factoryReset()">Factory Reset</button>
      </div>
    </section>

    <section class="panel">
      <h3>Backup und Restore</h3>
      <p>Einstellungen als JSON sichern oder wiederherstellen.</p>
      <div class="actions" style="margin-bottom:10px;">
        <button class="btn-secondary" onclick="backupSettings()">Backup herunterladen</button>
      </div>
      <label for="restore_file">Backup-Datei waehlen</label>
      <input type="file" id="restore_file" accept=".json">
      <div class="actions" style="margin-top:10px;">
        <button onclick="restoreSettings()">Wiederherstellen</button>
      </div>
      <div id="restore_status" class="status"></div>
    </section>

    <section class="panel">
      <h3>MQTT Datenabruf</h3>
      <p>Manuellen Abruf fuer Diagnose starten.</p>
      <div class="actions">
        <button class="btn-secondary" onclick="triggerRefresh('pwr')">PWR</button>
        <button class="btn-secondary" onclick="triggerRefresh('bat')">BAT</button>
        <button class="btn-secondary" onclick="triggerRefresh('stat')">STAT</button>
        <button class="btn-secondary" onclick="triggerRefresh('info')">INFO</button>
      </div>
      <div id="refresh_status" class="status"></div>
    </section>

    <section class="panel">
      <h3>Debug Log in SPIFFS</h3>
      <label>
        <input type="checkbox" id="logfile_toggle" onchange="setLogfileEnabled(this.checked)">
        Log in Datei schreiben
      </label>
      <p>Datei: <b id="logfile_path">/debug.log</b></p>
      <div class="actions">
        <button class="btn-secondary" onclick="clearLogfile()">Logdatei leeren</button>
      </div>
      <div id="logfile_status" class="status"></div>
    </section>

    <section class="panel">
      <h3>Syslog (UDP)</h3>
      <label>
        <input type="checkbox" id="syslog_enabled">
        Syslog aktivieren
      </label>
      <div style="margin-top:10px;">
        <label for="syslog_server">Server/IP</label>
        <input type="text" id="syslog_server" placeholder="192.168.88.10">
      </div>
      <div style="margin-top:10px;">
        <label for="syslog_port">Port</label>
        <input type="number" id="syslog_port" min="1" max="65535" value="514">
      </div>
      <div class="actions" style="margin-top:10px;">
        <button onclick="saveSyslogConfig()">Syslog speichern</button>
      </div>
      <div id="syslog_status" class="status"></div>
    </section>

    <section class="panel full">
      <h3>Speicher Uebersicht</h3>
      <table class="table">
        <tr>
          <td>SPIFFS Gesamt</td>
          <td>
            <span id="spiffs_total">...</span><br>
            <small id="spiffs_total_pct"></small>
          </td>
        </tr>
        <tr>
          <td>SPIFFS Belegt</td>
          <td>
            <span id="spiffs_used">...</span><br>
            <small id="spiffs_used_pct"></small>
          </td>
        </tr>
        <tr>
          <td>SPIFFS Frei</td>
          <td>
            <span id="spiffs_free">...</span><br>
            <small id="spiffs_free_pct"></small>
          </td>
        </tr>
        <tr>
          <td>NVS Gesamt</td>
          <td>
            <span id="nvs_total">...</span><br>
            <small id="nvs_total_pct"></small>
          </td>
        </tr>
        <tr>
          <td>NVS Belegt</td>
          <td>
            <span id="nvs_used">...</span><br>
            <small id="nvs_used_pct"></small>
          </td>
        </tr>
        <tr>
          <td>NVS Frei</td>
          <td>
            <span id="nvs_free">...</span><br>
            <small id="nvs_free_pct"></small>
          </td>
        </tr>
      </table>
    </section>
  </div>
</div>

<script>
function setStatus(id, text, cls) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  el.className = "status" + (cls ? " " + cls : "");
}

function triggerRefresh(type) {
  setStatus("refresh_status", type.toUpperCase() + " wird abgerufen...", "warn");
  fetch("/api/" + type + "/refresh", { method: "POST" })
    .then(r => r.text())
    .then(t => setStatus("refresh_status", t, "ok"))
    .catch(() => setStatus("refresh_status", "Fehler bei " + type.toUpperCase(), "err"));
}

function loadLogfileStatus() {
  fetch("/api/logfile/status?t=" + Date.now(), { cache: "no-store" })
    .then(r => r.json())
    .then(j => {
      document.getElementById("logfile_toggle").checked = !!j.enabled;
      document.getElementById("logfile_path").innerText = j.path || "/debug.log";
      setStatus("logfile_status", j.enabled ? "Logdatei aktiv" : "Logdatei inaktiv", j.enabled ? "ok" : "warn");
    })
    .catch(() => setStatus("logfile_status", "Status konnte nicht geladen werden", "err"));
}

function setLogfileEnabled(enable) {
  fetch("/api/logfile/enable?enable=" + (enable ? "1" : "0"), { method: "POST" })
    .then(r => r.text())
    .then(t => {
      setStatus("logfile_status", t, "ok");
      loadLogfileStatus();
    })
    .catch(() => {
      setStatus("logfile_status", "Umschalten fehlgeschlagen", "err");
      loadLogfileStatus();
    });
}

function clearLogfile() {
  fetch("/api/logfile/clear", { method: "POST" })
    .then(r => r.text())
    .then(t => setStatus("logfile_status", t, "ok"))
    .catch(() => setStatus("logfile_status", "Leeren fehlgeschlagen", "err"));
}

loadLogfileStatus();

function loadSyslogConfig() {
  fetch("/api/log/level?t=" + Date.now(), { cache: "no-store" })
    .then(r => r.json())
    .then(j => {
      document.getElementById("syslog_enabled").checked = !!j.syslogEnabled;
      document.getElementById("syslog_server").value = j.syslogServer || "";
      document.getElementById("syslog_port").value = (j.syslogPort || 514);
      setStatus("syslog_status", "Syslog-Konfiguration geladen", "ok");
    })
    .catch(() => setStatus("syslog_status", "Syslog-Konfiguration konnte nicht geladen werden", "err"));
}

function saveSyslogConfig() {
  const enabled = document.getElementById("syslog_enabled").checked;
  const server = document.getElementById("syslog_server").value.trim();
  const port = parseInt(document.getElementById("syslog_port").value || "514", 10);

  if (enabled && !server) {
    setStatus("syslog_status", "Bitte Syslog-Server eintragen", "warn");
    return;
  }
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    setStatus("syslog_status", "Ungueltiger Port (1..65535)", "warn");
    return;
  }

  fetch("/api/log/level", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      syslogEnabled: enabled,
      syslogServer: server,
      syslogPort: port
    })
  })
    .then(r => r.text())
    .then(t => {
      setStatus("syslog_status", t, "ok");
      loadSyslogConfig();
    })
    .catch(() => setStatus("syslog_status", "Speichern fehlgeschlagen", "err"));
}

loadSyslogConfig();

function loadFirmwareVersion() {
  fetch("/api/version?t=" + Date.now(), { cache: "no-store" })
    .then(r => r.text())
    .then(v => {
      document.getElementById("fwver").innerText = v;
    })
    .catch(() => {
      document.getElementById("fwver").innerText = "Fehler";
    });
}

loadFirmwareVersion();
setInterval(loadFirmwareVersion, 30000);

function upload() {
  const file = document.getElementById("fw").files[0];
  if (!file) {
    alert("Bitte eine .bin Datei auswaehlen");
    return;
  }

  const prog = document.getElementById("prog");
  prog.style.display = "block";
  prog.value = 0;
  setStatus("status", "Upload laeuft...", "warn");

  const formData = new FormData();
  formData.append("fw", file);

  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/api/ota");

  xhr.upload.onprogress = function(e) {
    if (e.lengthComputable) {
      const p = Math.round((e.loaded / e.total) * 100);
      prog.value = p;
      setStatus("status", "Upload: " + p + "%", "warn");
    }
  };

  xhr.onload = function() {
    if (xhr.status === 200) {
      prog.value = 100;
      setStatus("status", "Update erfolgreich. ESP startet neu...", "ok");
      setTimeout(() => location.reload(), 5000);
    } else {
      setStatus("status", "Fehler: " + xhr.responseText, "err");
    }
  };

  xhr.send(formData);
}

function restartESP() {
  if (!confirm("ESP wird neu gestartet.")) return;
  fetch("/api/restart", { method: "POST" }).then(() => {
    alert("Restart wird durchgefuehrt");
  });
}

function wifiReset() {
  if (!confirm("WiFi-Einstellungen werden geloescht. Fortfahren?")) return;
  fetch("/api/wifireset", { method: "POST" }).then(() => {
    alert("WiFi Reset durchgefuehrt. ESP startet neu.");
  });
}

function factoryReset() {
  if (!confirm("ALLE Nutzerdaten werden geloescht!\nFactory Reset durchfuehren?")) return;
  fetch("/api/factoryreset", { method: "POST" }).then(() => {
    alert("Factory Reset durchgefuehrt. ESP startet neu.");
  });
}

function backupSettings() {
  fetch("/api/backup")
    .then(r => r.blob())
    .then(blob => {
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = "pylontech_backup.json";
      a.click();
    });
}

function restoreSettings() {
  const file = document.getElementById("restore_file").files[0];
  if (!file) {
    alert("Bitte eine JSON-Backup-Datei auswaehlen.");
    return;
  }

  const reader = new FileReader();
  reader.onload = function(e) {
    setStatus("restore_status", "Wird wiederhergestellt...", "warn");
    fetch("/api/restore", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: e.target.result
    })
      .then(r => r.text())
      .then(t => setStatus("restore_status", t, "ok"))
      .catch(() => setStatus("restore_status", "Fehler beim Wiederherstellen.", "err"));
  };
  reader.readAsText(file);
}

fetch("/api/storageinfo")
  .then(r => r.json())
  .then(info => {
    document.getElementById("spiffs_total").innerText = info.spiffs_total + " Bytes";
    document.getElementById("spiffs_used").innerText = info.spiffs_used + " Bytes";
    document.getElementById("spiffs_free").innerText = info.spiffs_free + " Bytes";

    const spiffs_total = info.spiffs_total;
    const spiffs_used = info.spiffs_used;
    const spiffs_free = info.spiffs_free;

    document.getElementById("spiffs_total_pct").innerText =
      Math.round((spiffs_total / spiffs_total) * 100) + "%";

    document.getElementById("spiffs_used_pct").innerText =
      Math.round((spiffs_used / spiffs_total) * 100) + "% belegt";

    document.getElementById("spiffs_free_pct").innerText =
      Math.round((spiffs_free / spiffs_total) * 100) + "% frei";

    if (info.nvs_total !== undefined) {
      document.getElementById("nvs_total").innerText = info.nvs_total + " Eintraege";
      document.getElementById("nvs_used").innerText = info.nvs_used + " Eintraege";
      document.getElementById("nvs_free").innerText = info.nvs_free + " Eintraege";

      const nvs_total = info.nvs_total;
      const nvs_used = info.nvs_used;
      const nvs_free = info.nvs_free;

      document.getElementById("nvs_total_pct").innerText = "100%";

      document.getElementById("nvs_used_pct").innerText =
        nvs_total > 0 ? Math.round((nvs_used / nvs_total) * 100) + "% belegt" : "";

      document.getElementById("nvs_free_pct").innerText =
        nvs_total > 0 ? Math.round((nvs_free / nvs_total) * 100) + "% frei" : "";
    } else {
      document.getElementById("nvs_total").innerText = "nicht verfuegbar";
      document.getElementById("nvs_used").innerText = "nicht verfuegbar";
      document.getElementById("nvs_free").innerText = "nicht verfuegbar";
    }
  });
</script>

</body>
</html>
)rawliteral";
