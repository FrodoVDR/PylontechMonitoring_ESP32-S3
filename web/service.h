#pragma once
#include <Arduino.h>

const char SERVICE_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>ESP32 OTA Update</title>
<style>
body { font-family: sans-serif; padding: 20px; }
button { padding: 8px 16px; margin-top: 10px; }
#status { margin-top: 15px; font-weight: bold; }
progress { width: 300px; display:none; }
</style>
</head>
<body>

<h2>ESP32 OTA Update</h2>
<p>Aktuelle alte Firmwareversion: <b id="fwver">...</b></p>

<p>Bitte eine <b>.bin</b> Firmware auswählen:</p>

<input type="file" id="fw">
<br><br>
<button onclick="upload()">Flashen</button>

<br><br>
<progress id="prog" value="0" max="100"></progress>
<div id="status"></div>

<h3>System Aktionen</h3>

<button onclick="restartESP()">Restart ESP</button>
<br><br>

<button onclick="wifiReset()">WiFi Reset</button>
<br><br>

<button onclick="factoryReset()">Factory Reset</button>
<br><br>

<h3>Speicherinformationen</h3>
<table>
<tr>
  <td>SPIFFS Gesamt:</td>
  <td>
    <span id="spiffs_total">...</span><br>
  </td>
</tr>
<tr>
  <td>SPIFFS Belegt:</td>
  <td>
    <span id="spiffs_used">...</span><br>
    <small id="spiffs_used_pct"></small>
  </td>
</tr>
<tr>
  <td>SPIFFS Frei:</td>
  <td>
    <span id="spiffs_free">...</span><br>
    <small id="spiffs_free_pct"></small>
  </td>
</tr>

<tr>
  <td>NVS Gesamt:</td>
  <td>
    <span id="nvs_total">...</span><br>
  </td>
</tr>
<tr>
  <td>NVS Belegt:</td>
  <td>
    <span id="nvs_used">...</span><br>
    <small id="nvs_used_pct"></small>
  </td>
</tr>
<tr>
  <td>NVS Frei:</td>
  <td>
    <span id="nvs_free">...</span><br>
    <small id="nvs_free_pct"></small>
  </td>
</tr>
</table>



<script>
// ------------------------------------------------------------
// Firmwareversion vom ESP laden
// ------------------------------------------------------------
fetch("/api/version")
    .then(r => r.text())
    .then(v => {
        document.getElementById("fwver").innerText = v;
    });

// ------------------------------------------------------------
// OTA Upload (multipart/form-data!)
// ------------------------------------------------------------
function upload() {
    let file = document.getElementById("fw").files[0];
    if (!file) {
        alert("Bitte eine .bin Datei auswählen");
        return;
    }

    let prog = document.getElementById("prog");
    let status = document.getElementById("status");

    prog.style.display = "block";
    prog.value = 0;
    status.innerText = "Upload läuft...";

    // Multipart-FormData erzeugen
    let formData = new FormData();
    formData.append("fw", file);

    let xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/ota");

    xhr.upload.onprogress = function(e) {
        if (e.lengthComputable) {
            let p = Math.round((e.loaded / e.total) * 100);
            prog.value = p;
            status.innerText = "Upload: " + p + "%";
        }
    };

    xhr.onload = function() {
        if (xhr.status == 200) {
            status.innerText = "Update erfolgreich! ESP startet neu...";
            prog.value = 100;

            setTimeout(() => {
                location.reload();
            }, 5000);
        } else {
            status.innerText = "Fehler: " + xhr.responseText;
        }
    };

    xhr.send(formData);
}
// ---------------------------------------------
// ESP Neustart
// ---------------------------------------------
function restartESP() {
    if (!confirm("ESP wird neu gestartet.")) return;

    fetch("/api/restart", { method: "POST" })
        .then(() => {
            alert("Restart wird durchgeführt");
        });
}

// ---------------------------------------------
// WiFi Reset
// ---------------------------------------------
function wifiReset() {
    if (!confirm("WiFi-Einstellungen werden gelöscht. Fortfahren?")) return;

    fetch("/api/wifireset", { method: "POST" })
        .then(() => {
            alert("WiFi Reset durchgeführt. ESP startet neu.");
        });
}

// ---------------------------------------------
// Factory Reset
// ---------------------------------------------
function factoryReset() {
    if (!confirm("ALLE Nutzerdaten werden gelöscht!\nFactory Reset durchführen?")) return;

    fetch("/api/factoryreset", { method: "POST" })
        .then(() => {
            alert("Factory Reset durchgeführt. ESP startet neu.");
        });
}
// -------------------------------------------------------------
// Speicherinfo laden
// -------------------------------------------------------------
fetch("/api/storageinfo")
  .then(r => r.json())
  .then(info => {

    // --- SPIFFS ---
    document.getElementById("spiffs_total").innerText = info.spiffs_total + " Bytes";
    document.getElementById("spiffs_used").innerText  = info.spiffs_used  + " Bytes";
    document.getElementById("spiffs_free").innerText  = info.spiffs_free  + " Bytes";

    let spiffs_total = info.spiffs_total;
    let spiffs_used  = info.spiffs_used;
    let spiffs_free  = info.spiffs_free;

    document.getElementById("spiffs_total_pct").innerText =
        Math.round((spiffs_total / spiffs_total) * 100) + "% von 100%";

    document.getElementById("spiffs_used_pct").innerText =
        Math.round((spiffs_used / spiffs_total) * 100) + "% belegt";

    document.getElementById("spiffs_free_pct").innerText =
        Math.round((spiffs_free / spiffs_total) * 100) + "% frei";

    // --- NVS ---
    document.getElementById("nvs_total").innerText = info.nvs_total + " Einträge";
    document.getElementById("nvs_used").innerText  = info.nvs_used  + " Einträge";
    document.getElementById("nvs_free").innerText  = info.nvs_free  + " Einträge";

    let nvs_total = info.nvs_total;
    let nvs_used  = info.nvs_used;
    let nvs_free  = info.nvs_free;

    document.getElementById("nvs_total_pct").innerText =
        Math.round((nvs_total / nvs_total) * 100) + "% von 100%";

    document.getElementById("nvs_used_pct").innerText =
        Math.round((nvs_used / nvs_total) * 100) + "% belegt";

    document.getElementById("nvs_free_pct").innerText =
        Math.round((nvs_free / nvs_total) * 100) + "% frei";
  });


</script>


</body>
</html>
)rawliteral";
