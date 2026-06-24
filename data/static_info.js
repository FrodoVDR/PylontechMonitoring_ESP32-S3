// Immer zurücksetzen wenn Seite geladen wird.
// "undefined"-Guard war falsch: hängende Flags aus vorherigem Seitenlauf
// ließen infoLoad() beim Rückkehren sofort abbrechen → INFO leer.
window.infoSaveInFlight = false;
window.infoLoadInFlight = false;
window.infoPolling = false;
window.infoAutoRefreshRequested = false;
if (window.infoPollTimer) { clearTimeout(window.infoPollTimer); }
window.infoPollTimer = null;
const INFO_SNAPSHOT_KEY = "info_last_snapshot_v1";

function sanitize(str){
    // Remove control chars, delimiter, special chars, excessive spaces, accents
    let s = (str || "")
        .replace(/[\x00-\x1F\x7F]/g, "")  // Control chars
        .replace(/\|/g, "")                 // Field delimiter
        .replace(/[^\w\s\-\.]/g, "")      // Keep only word chars, space, dash, dot
        .replace(/\s+/g, "")                // Remove spaces completely
        .replace(/^_+|_+$/g, "")             // Trim underscores
        .trim();
    return s.length > 0 ? s : "field";
}

function saveInfoSnapshot(payload) {
    try {
        if (!payload || !payload.headers || payload.headers.length === 0) return;
        const vals = Array.isArray(payload.values) ? payload.values : [];
        const hasNonEmptyRaw = vals.some(v => String(v || "").trim().length > 0);
        if (!hasNonEmptyRaw) return;
        const snap = {
            headers: payload.headers,
            values: payload.values || [],
            fields: payload.fields || [],
            config: payload.config || {},
            mqtt: payload.mqtt || {},
            cacheTimestamp: payload.cacheTimestamp || 0
        };
        localStorage.setItem(INFO_SNAPSHOT_KEY, JSON.stringify(snap));
    } catch (_) {}
}

function loadInfoSnapshot() {
    try {
        const raw = localStorage.getItem(INFO_SNAPSHOT_KEY);
        if (!raw) return null;
        const snap = JSON.parse(raw);
        if (!snap || !snap.headers || snap.headers.length === 0) return null;
        return snap;
    } catch (_) {
        return null;
    }
}

function renderCacheTs(elementId, ts) {
    const el = document.getElementById(elementId);
    if (!el) return;
    if (!ts) {
        el.innerText = "Cache: kein Eintrag (UART-Abruf läuft)";
        return;
    }
    const secs = Math.max(0, Math.floor((Date.now() - ts) / 1000));
    el.innerText = "Cache-Alter: " + secs + "s";
}

function renderInfoStatus(msg) {
    const el = document.getElementById("info_status_text");
    if (!el) return;
    el.innerText = "Status: " + (msg || "-");
}

function renderInfoSource(src) {
    const el = document.getElementById("info_source_text");
    if (!el) return;
    if (src === "cache") el.innerText = "Quelle: NVS-Cache";
    else if (src === "uart") el.innerText = "Quelle: UART (frisch)";
    else el.innerText = "Quelle: keine Daten";
}

function autodetect(name, raw) {

    let n = name.toLowerCase();
    let r = (raw || "").toLowerCase();

    if (n.includes("percent") || r.includes("%"))
        return { factor: "1", unit: "%", mqtt: true, send: true };

    if (n.includes("coulomb"))
        return { factor: "0.001", unit: "Ah", mqtt: true, send: true };

    if (isNaN(parseFloat(r)))
        return { factor: "text", unit: "", mqtt: false, send: false };

    return { factor: "1", unit: "", mqtt: false, send: false };
}

function infoLoad() {
    if (window.infoLoadInFlight) return;
    window.infoLoadInFlight = true;
    fetch("/api/info/values")
        .then(r => r.json())
        .then(j => {

            if (!j || typeof j !== "object") j = {};

            // If backend returns empty headers, try local snapshot first.
            const hasHeaders = Array.isArray(j.headers) && j.headers.length > 0;
            if (!hasHeaders) {
                const snap = loadInfoSnapshot();
                if (snap) {
                    j = snap;
                    renderInfoStatus("INFO aus lokalem Snapshot geladen");
                    renderInfoSource("cache");
                }
            }

            renderCacheTs("cache_ts_info", j.cacheTimestamp || 0);
            renderInfoStatus(j.statusText || "-");
            renderInfoSource(j.dataSource || "none");

            // CONFIG oben
            const cfg = j.config || {};
            const mq = j.mqtt || {};
            document.getElementById("enable_info").checked = !!cfg.enableInfo;
            document.getElementById("topic_info").value = mq.topicInfo || "info";
            document.getElementById("interval_info").value = Math.max(1, Math.floor((cfg.intervalInfo || 3600000) / 1000));

            // Tabelle unten
            let container = document.getElementById("info_groups");

            if (!j.headers || j.headers.length === 0) {
                // Keep current table visible (if any) instead of replacing with empty page.
                if (!container.querySelector("table")) {
                    container.innerHTML = '<p style="color:#aaa">Keine Cache-Daten vorhanden. INFO wird automatisch abgerufen…</p>';
                }
                // Mirror the manual "Daten abrufen" path exactly once per page-open.
                if (!window.infoAutoRefreshRequested) {
                    window.infoAutoRefreshRequested = true;
                    fetch('/api/info/refresh', { method: 'POST' })
                        .finally(() => {
                            if (!window.infoPolling && !window.infoPollTimer) {
                                window.infoPollTimer = setTimeout(() => {
                                    window.infoPollTimer = null;
                                    pollInfo(40);
                                }, 500);
                            }
                        });
                } else if (!window.infoPolling && !window.infoPollTimer) {
                    window.infoPollTimer = setTimeout(() => {
                        window.infoPollTimer = null;
                        pollInfo(40);
                    }, 500);
                }
                return;
            }

            // If headers exist but all raw values are empty, keep current table and continue polling.
            const hasNonEmptyRaw = Array.isArray(j.values)
                && j.values.some(v => String(v || "").trim().length > 0);
            if (!hasNonEmptyRaw) {
                const snap = loadInfoSnapshot();
                if (snap) {
                    j = {
                        ...snap,
                        statusText: "INFO aus lokalem Snapshot (warte auf frische Rohdaten)",
                        dataSource: "cache"
                    };
                } else {
                    renderInfoStatus("INFO ohne Rohdaten empfangen, warte auf gueltigen Datensatz...");
                    if (!window.infoPolling && !window.infoPollTimer) {
                        window.infoPollTimer = setTimeout(() => {
                            window.infoPollTimer = null;
                            pollInfo(40);
                        }, 1000);
                    }
                    return;
                }
            }

            window.infoPolling = false;
            if (window.infoPollTimer) {
                clearTimeout(window.infoPollTimer);
                window.infoPollTimer = null;
            }
            saveInfoSnapshot(j);
            window.infoAutoRefreshRequested = false;

            container.innerHTML = "";

            // Eine einzige Tabelle erzeugen
            let table = document.createElement("table");
            table.className = "conn-table";

            table.innerHTML = `
                <thead>
                    <tr>
                        <th>Feldname</th>
                        <th>Anzeigename</th>
                        <th>Rohdaten</th>
                        <th>Faktor</th>
                        <th>Einheit</th>
                        <th>MQTT</th>
                        <th>Send</th>
                    </tr>
                </thead>
                <tbody></tbody>
            `;

            let tbody = table.querySelector("tbody");

            // gespeicherte Felder
            let saved = {};
            j.fields.forEach(f => saved[f.name] = f);

            // ALLE FELDER UNTEREINANDER
            for (let i = 0; i < j.headers.length; i++) {

                let name = j.headers[i];
                let raw  = j.values[i];

                let display = name;
                let factor  = "1";
                let unit    = "";
                let mqtt    = false;
                let send    = false;

                if (saved[name]) {
                    display = saved[name].display;
                    factor  = saved[name].factor;
                    unit    = saved[name].unit;
                    mqtt    = saved[name].sendMQTT;
                    send    = saved[name].sendPayload;
                }
                else {
                    // WICHTIG: autodetect NICHT verwenden (verursacht falsche Baselines)
                    // Alle neuen Felder mit Defaults, nicht mit autodetect mqtt/send!
                    factor = "1";
                    unit   = "";
                    mqtt   = false;   // IMMER false für neue Felder
                    send   = false;   // IMMER false für neue Felder
                }

                // WICHTIG: Display-Namen müssen HIER SCHON sanitiert sein für Baseline-Vergleich
                let displayClean = sanitize(display);

                let row = document.createElement("tr");
                row.dataset.fieldName = name;
                row.innerHTML = `
                    <td>${name}</td>
                    <td><input value="${displayClean}"></td>
                    <td>${raw}</td>
                    <td>
                        <select>
                            <option value="0.0001">0.0001</option>
                            <option value="0.001">0.001</option>
                            <option value="0.01">0.01</option>
                            <option value="0.1">0.1</option>
                            <option value="1">1</option>
                            <option value="10">10</option>
                            <option value="text">text</option>
                            <option value="date">date</option>
                        </select>
                    </td>
                    <td>
                        <select>
                            <option value=""></option>
                            <option value="V">V</option>
                            <option value="A">A</option>
                            <option value="Ah">Ah</option>
                            <option value="°C">°C</option>
                            <option value="%">%</option>
                            <option value="timestamp">timestamp</option>
                        </select>
                    </td>
                    <td><input type="checkbox" class="mqtt"></td>
                    <td><input type="checkbox" class="send"></td>
                `;

                // Werte setzen
                row.cells[3].querySelector("select").value = factor;
                row.cells[4].querySelector("select").value = unit;
                row.cells[5].querySelector("input").checked = mqtt;
                row.cells[6].querySelector("input").checked = send;

                // Baseline for delta-save (mit GEREINIIGTEM Display-Namen)
                row.dataset.baseDisplay = displayClean;
                row.dataset.baseFactor = factor;
                row.dataset.baseUnit = unit;
                row.dataset.baseMqtt = mqtt ? "1" : "0";
                row.dataset.baseSend = send ? "1" : "0";

                tbody.appendChild(row);
            }

            container.appendChild(table);
        })
        .finally(() => {
            window.infoLoadInFlight = false;
        });
}

if (!window.__infoChangeHandlerBound) {
window.__infoChangeHandlerBound = true;
document.addEventListener("change", function(e) {

    let target = e.target;

    // MQTT deaktiviert → SEND deaktivieren
    if (target.classList.contains("mqtt")) {
        let row = target.closest("tr");
        if (!row) return;

        let sendBox = row.querySelector(".send");
        if (!sendBox) return;

        if (!target.checked) {
            sendBox.checked = false;
        }
    }

    // SEND aktiviert → MQTT aktivieren
    if (target.classList.contains("send")) {
        let row = target.closest("tr");
        if (!row) return;

        let mqttBox = row.querySelector(".mqtt");
        if (!mqttBox) return;

        if (target.checked) {
            mqttBox.checked = true;
        }
    }
});
}

infoLoad();

function saveInfoSettings() {
    if (window.infoSaveInFlight) return;

    const rows = document.querySelectorAll('#info_groups table tbody tr');
    if (!rows || rows.length === 0) {
        alert('Noch keine INFO-Tabelle geladen. Bitte zuerst "Daten abrufen".');
        return;
    }

    let data = {
        config: {
            enableInfo: document.getElementById("enable_info").checked,
            intervalInfo: parseInt(document.getElementById("interval_info").value) * 1000
        },
        mqtt: {
            topicInfo: document.getElementById("topic_info").value
        },
        fields: []
    };

    rows.forEach(row => {

        let name = row.dataset.fieldName || row.cells[0].textContent || "";
        if (!name) return;

        // Display-Name
        let dispInput = row.cells[1].querySelector("input");
        let dispRaw   = dispInput.value || name;

        // Sanitizen: Speichere korrigierte Version ins Input
        let dispClean = sanitize(dispRaw);
        dispInput.value = dispClean;
        // WICHTIG: Baseline NICHT hier aktualisieren! Das wird nach erfolgreichem Speichern gemacht.

        // Faktor
        let factor = row.cells[3].querySelector("select").value;

        // Einheit
        let unit = row.cells[4].querySelector("select").value;

        // MQTT
        let mqtt = row.cells[5].querySelector("input").checked;

        // Send
        let send = row.cells[6].querySelector("input").checked;

        // Robust statt Delta: alle Zeilen senden, damit keine Haken durch Baseline-Drift verloren gehen.
        data.fields.push({
            name: name,
            display: dispClean,
            factor: factor,
            unit: unit,
            sendMQTT: mqtt,
            sendPayload: send
        });
    });

    window.infoSaveInFlight = true;
    renderInfoStatus("Speichern laeuft...");
    fetch('/api/info/set', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify(data)
    })
    .then(async (resp) => {
        if (!resp.ok) {
            const msg = await resp.text();
            throw new Error(msg || ('HTTP ' + resp.status));
        }
        renderInfoStatus("Speichern erfolgreich");
        alert('Gespeichert');
        // Nach erfolgreichem Speichern: Reload aller Daten und Baselines
        infoLoad();
    })
    .catch((err) => {
        renderInfoStatus("Speichern fehlgeschlagen");
        alert('Speichern fehlgeschlagen: ' + (err && err.message ? err.message : 'unbekannter Fehler'));
    })
    .finally(() => {
        window.infoSaveInFlight = false;
    });
}

function refreshInfo() {
    let container = document.getElementById("info_groups");
    container.innerHTML = '<p style="color:#aaa">Daten werden abgerufen…</p>';
    fetch('/api/info/refresh', { method: 'POST' })
    .then(() => pollInfo(40));
}

function pollInfo(retries) {
    if (window.infoPolling && retries === 40) return;
    window.infoPolling = true;

    if (retries <= 0) {
        window.infoPolling = false;
        renderInfoStatus("Timeout: kein konsistenter INFO-Datensatz empfangen");
        document.getElementById("info_groups").innerHTML =
            '<p style="color:orange">Keine Antwort auf <b>info</b>-Befehl. Bitte in der Battery Console \'info 1\' manuell testen.</p>';
        return;
    }
    fetch("/api/info/values")
        .then(r => r.json())
        .then(j => {
            if (j.headers && j.headers.length > 0) {
                window.infoPolling = false;
                infoLoad();
            } else {
                window.infoPollTimer = setTimeout(() => {
                    window.infoPollTimer = null;
                    pollInfo(retries - 1);
                }, 2000);
            }
        })
        .catch(() => {
            window.infoPollTimer = setTimeout(() => {
                window.infoPollTimer = null;
                pollInfo(retries - 1);
            }, 2000);
        });
}
