// Immer zurücksetzen beim Seitenladen.
window.statSaveInFlight = false;

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

function renderStatStatus(msg) {
    const el = document.getElementById("stat_status_text");
    if (!el) return;
    el.innerText = "Status: " + (msg || "-");
}

function renderStatSource(src) {
    const el = document.getElementById("stat_source_text");
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

function statLoad() {
    fetch("/api/stat/values")
        .then(r => r.json())
        .then(j => {

            renderCacheTs("cache_ts_stat", j.cacheTimestamp || 0);
            renderStatStatus(j.statusText || "-");
            renderStatSource(j.dataSource || "none");

            // CONFIG oben
            document.getElementById("enable_stat").checked = j.config.enableStat;
            document.getElementById("topic_stat").value = j.mqtt.topicStat;
            document.getElementById("interval_stat").value = j.config.intervalStat / 1000;

            // Tabelle unten
            let container = document.getElementById("stat_groups");
            container.innerHTML = "";

            if (!j.headers || j.headers.length === 0) {
                container.innerHTML = '<p style="color:#aaa">Keine Cache-Daten vorhanden. STAT wird automatisch abgerufen…</p>';
                setTimeout(() => pollStat(40), 500);
                return;
            }

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
        });
}

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

statLoad();

function saveStatSettings() {
    if (window.statSaveInFlight) return;

    const rows = document.querySelectorAll('#stat_groups table tbody tr');
    if (!rows || rows.length === 0) {
        alert('Noch keine STAT-Tabelle geladen. Bitte zuerst "Daten abrufen".');
        return;
    }

    let data = {
        config: {
            enableStat: document.getElementById("enable_stat").checked,
            intervalStat: parseInt(document.getElementById("interval_stat").value) * 1000
        },
        mqtt: {
            topicStat: document.getElementById("topic_stat").value
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


        // Faktor
        let factor = row.cells[3].querySelector("select").value;

        // Einheit
        let unit = row.cells[4].querySelector("select").value;

        // MQTT
        let mqtt = row.cells[5].querySelector("input").checked;

        // Send
        let send = row.cells[6].querySelector("input").checked;

        // Mini-Filter: nur relevante Zeilen senden, um Payload klein zu halten.
        // Relevanz:
        // 1) aktuell aktiv (MQTT oder Send)
        // 2) zuvor aktiv (damit Deaktivierung gespeichert wird)
        // 3) in Display/Faktor/Einheit geaendert
        const baseDisplay = row.dataset.baseDisplay || name;
        const baseFactor  = row.dataset.baseFactor || "1";
        const baseUnit    = row.dataset.baseUnit || "";
        const baseMqtt    = row.dataset.baseMqtt === "1";
        const baseSend    = row.dataset.baseSend === "1";

        const changedMeta = (
            dispClean !== baseDisplay ||
            factor !== baseFactor ||
            unit !== baseUnit
        );

        const relevant = (
            mqtt || send || baseMqtt || baseSend || changedMeta
        );

        if (!relevant) return;

        data.fields.push({
            name: name,
            display: dispClean,
            factor: factor,
            unit: unit,
            sendMQTT: mqtt,
            sendPayload: send
        });
    });

    window.statSaveInFlight = true;
    renderStatStatus("Speichern laeuft...");
    fetch('/api/stat/set', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify(data)
    })
    .then(async (resp) => {
        if (!resp.ok) {
            const msg = await resp.text();
            throw new Error(msg || ('HTTP ' + resp.status));
        }
        renderStatStatus("Speichern erfolgreich");
        alert('Gespeichert');
        // Nach erfolgreichem Speichern: Reload aller Daten und Baselines
        statLoad();
    })
    .catch((err) => {
        renderStatStatus("Speichern fehlgeschlagen");
        alert('Speichern fehlgeschlagen: ' + (err && err.message ? err.message : 'unbekannter Fehler'));
    })
    .finally(() => {
        window.statSaveInFlight = false;
    });
}

function refreshStat() {
    let container = document.getElementById("stat_groups");
    container.innerHTML = '<p style="color:#aaa">Daten werden abgerufen…</p>';
    fetch('/api/stat/refresh', { method: 'POST' })
    .then(() => pollStat(40));
}

function pollStat(retries) {
    if (retries <= 0) {
        document.getElementById("stat_groups").innerHTML =
            '<p style="color:orange">Keine Antwort auf <b>stat</b>-Befehl. Bitte in der Battery Console \'stat 1\' manuell testen, um zu prüfen ob das Gerät diesen Befehl unterstützt.</p>';
        return;
    }
    fetch("/api/stat/values")
        .then(r => r.json())
        .then(j => {
            if (j.headers && j.headers.length > 0) {
                statLoad();
            } else {
                setTimeout(() => pollStat(retries - 1), 2000);
            }
        })
        .catch(() => setTimeout(() => pollStat(retries - 1), 2000));
}
