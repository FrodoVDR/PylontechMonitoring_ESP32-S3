#pragma once
#include <Arduino.h>

const char FILEMANAGER_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Pylontech File Manager</title>
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

.panel {
    background: var(--card);
    border: 1px solid var(--line);
    border-radius: 12px;
    box-shadow: var(--shadow);
    padding: 15px;
}

.upload-row {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    align-items: center;
}

input[type="file"] {
    flex: 1 1 260px;
    border: 1px solid #c9d5e6;
    border-radius: 8px;
    padding: 8px 10px;
    background: #fff;
    color: var(--text);
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

.table-wrap {
    margin-top: 14px;
    overflow-x: auto;
}

table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.94rem;
}

th, td {
    border-bottom: 1px solid #edf1f7;
    padding: 10px 8px;
    text-align: left;
}

th {
    color: #44556f;
    font-weight: 700;
}

td:first-child {
    word-break: break-word;
}

.size {
    color: var(--muted);
    white-space: nowrap;
}

.actions {
    display: flex;
    gap: 10px;
    white-space: nowrap;
}

.actions a {
    text-decoration: none;
    font-weight: 600;
}

.actions a.download { color: var(--primary-dark); }
.actions a.delete { color: var(--danger-dark); }

.empty {
    color: var(--muted);
    padding: 12px 0;
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
        <h2>Dateimanager</h2>
        <p>Dateien auf SPIFFS hochladen, herunterladen und loeschen.</p>
    </div>

    <div class="panel">
        <div class="upload-row">
            <input type="file" id="file" multiple>
            <button onclick="upload()">Upload starten</button>
            <button class="btn-secondary" onclick="load()">Aktualisieren</button>
        </div>

        <progress id="prog" value="0" max="100"></progress>
        <div id="progtext" class="status"></div>

        <div class="table-wrap">
            <table id="tbl"></table>
        </div>
    </div>
</div>

<script>
function fmtBytes(bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
        return (bytes / (1024 * 1024)).toFixed(2) + " MB";
}

function setStatus(text, cls) {
        let el = document.getElementById("progtext");
        el.className = "status" + (cls ? " " + cls : "");
        el.innerText = text || "";
}

function load() {
    fetch('/fm/list')
    .then(r => r.json())
    .then(list => {
                if (!list.length) {
                        document.getElementById("tbl").innerHTML =
                            "<tr><td class='empty'>Keine Dateien vorhanden.</td></tr>";
                        return;
                }

                let html = "<tr><th>Name</th><th>Groesse</th><th>Aktion</th></tr>";
        list.forEach(f => {
            html += `<tr>
                <td>${f.name}</td>
                                <td class="size">${fmtBytes(f.size)} (${f.size} B)</td>
                                <td class="actions">
                                        <a class="download" href="/fm/download?file=${encodeURIComponent(f.name)}" download="${f.name}">Download</a>
                                        <a class="delete" href="#" onclick="del('${f.name}')">Loeschen</a>
                </td>
            </tr>`;
        });
        document.getElementById("tbl").innerHTML = html;
        })
        .catch(() => setStatus("Dateiliste konnte nicht geladen werden.", "err"));
}

async function upload() {
    let files = document.getElementById("file").files;
        if (!files.length) {
                setStatus("Bitte mindestens eine Datei auswaehlen.", "err");
                return;
        }

    let prog = document.getElementById("prog");

        prog.style.display = "block";
    prog.value = 0;
        setStatus("Upload gestartet...", "");

    for (let i = 0; i < files.length; i++) {
        let f = files[i];

        let fd = new FormData();
        fd.append("file", f, f.name);

        await new Promise((resolve, reject) => {
            let xhr = new XMLHttpRequest();
            xhr.open("POST", "/fm/upload");

            xhr.upload.onprogress = function(e) {
                if (e.lengthComputable) {
                    let p = Math.round((e.loaded / e.total) * 100);
                    prog.value = p;
                    setStatus(`Datei ${i+1}/${files.length}: ${p}%`, "");
                }
            };

            xhr.onload = function() {
                if (xhr.status === 200) resolve();
                else reject(new Error("Upload fehlgeschlagen"));
            };

            xhr.onerror = reject;
            xhr.send(fd);
        });
    }

    prog.value = 100;
    setStatus("Upload abgeschlossen.", "ok");
    load();
}

function del(name) {
    if (!confirm("Datei wirklich loeschen?\n" + name)) return;
    fetch("/fm/delete?file=" + encodeURIComponent(name))
    .then(() => {
        setStatus("Datei geloescht: " + name, "ok");
        load();
    })
    .catch(() => setStatus("Loeschen fehlgeschlagen.", "err"));
}

load();
</script>

</body>
</html>
)rawliteral";
