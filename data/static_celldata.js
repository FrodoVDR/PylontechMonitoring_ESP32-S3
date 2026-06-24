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

function renderBatStatus(msg) {
    const el = document.getElementById("bat_status_text");
    if (!el) return;
    el.innerText = "Status: " + (msg || "-");
}

function renderBatSource(src) {
    const el = document.getElementById("bat_source_text");
    if (!el) return;
    if (src === "cache") el.innerText = "Quelle: NVS-Cache";
    else if (src === "uart") el.innerText = "Quelle: UART (frisch)";
    else el.innerText = "Quelle: keine Daten";
}

function batLoad() {
    fetch("/api/bat/cells")
        .then(r => r.json())
        .then(j => {

            renderCacheTs("cache_ts_bat", j.cacheTimestamp || 0);
            renderBatStatus(j.statusText || "-");
            renderBatSource(j.dataSource || "none");

            // Config
            document.getElementById("enable_bat").checked = j.config.enableBat;
            document.getElementById("topic_bat").value   = j.mqtt.topicBat;
            document.getElementById("cell_prefix").value = j.mqtt.cellPrefix;
            document.getElementById("interval_bat").value = j.config.intervalBat / 1000;

            // Tabelle
            let table = document.getElementById("bat_table");
            table.innerHTML = "";

            if (!j.headers || j.headers.length === 0) {
                table.innerHTML = '<tr><td colspan="7" style="color:#aaa">Keine Cache-Daten vorhanden. BAT wird automatisch abgerufen…</td></tr>';
                setTimeout(() => pollBat(40), 500);
                return;
            }

            // gespeicherte Felder
            let saved = {};
            j.fields.forEach(f => saved[f.name] = f);

            for (let i = 0; i < j.headers.length; i++) {

                let name = j.headers[i];
                let raw  = j.values[i] || "";

                // Default
                let display = name;
                let factor  = "1";
                let unit    = "";
                let mqtt    = false;
                let send    = false;

                // gespeicherte Werte überschreiben
                if (saved[name]) {
                    display = saved[name].display;
                    factor  = saved[name].factor;
                    unit    = saved[name].unit;
                    mqtt    = saved[name].sendMQTT;
                    send    = saved[name].sendPayload;
                }
                else {
                    // Verwende nur Defaults, kein Autodetect
                    // (autodetect verursacht verlorene Häkchen beim Speichern)
                    factor = "1";
                    unit   = "";
                    mqtt   = false;
                    send   = false;
                }

                // Tabelle rendern
                let row = document.createElement("tr");
                row.dataset.fieldName = name;
                row.innerHTML = `
                    <td>${name}</td>
                    <td><input id="disp_${name}" value="${display}"></td>
                    <td>${raw}</td>
                    <td>
                        <select id="fac_${name}">
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
                        <select id="unit_${name}">
                            <option value=""></option>
                            <option value="V">V</option>
                            <option value="A">A</option>
                            <option value="°C">°C</option>
                            <option value="%">%</option>
                            <option value="Ah">Ah</option>
                            <option value="timestamp">timestamp</option>
                        </select>
                    </td>
                    <td><input type="checkbox" id="mqtt_${name}"></td>
                    <td><input type="checkbox" id="send_${name}"></td>
                `;

                table.appendChild(row);

                // Werte setzen
                document.getElementById("fac_" + name).value = factor;
                document.getElementById("unit_" + name).value = unit;
                document.getElementById("mqtt_" + name).checked = mqtt;
                document.getElementById("send_" + name).checked = send;
            }
        });
}

document.addEventListener("change", function(e) {
    let target = e.target;

    if (target.id.startsWith("mqtt_")) {
        let row = target.closest("tr");
        if (!row) return;
        let sendBox = row.querySelector("[id^='send_']");
        if (!sendBox) return;
        if (!target.checked) sendBox.checked = false;
    }

    if (target.id.startsWith("send_")) {
        let row = target.closest("tr");
        if (!row) return;
        let mqttBox = row.querySelector("[id^='mqtt_']");
        if (!mqttBox) return;
        if (target.checked) mqttBox.checked = true;
    }
});


function batSave() {

    let data = {
        config: {
            intervalBat: parseInt(document.getElementById("interval_bat").value) * 1000,
            enableBat: document.getElementById("enable_bat").checked
        },
        mqtt: {
            topicBat:   document.getElementById("topic_bat").value,
            cellPrefix: document.getElementById("cell_prefix").value
        },
        fields: []
    };

    document.querySelectorAll("#bat_table tr").forEach(row => {
        let name = row.dataset.fieldName || row.cells[0].textContent || "";
        if (!name) return;
        let disp = sanitize(document.getElementById("disp_" + name).value);

        data.fields.push({
            name: name,
            display: disp,
            factor: document.getElementById("fac_" + name).value,
            unit:   document.getElementById("unit_" + name).value,
            sendMQTT: document.getElementById("mqtt_" + name).checked,
            sendPayload: document.getElementById("send_" + name).checked
        });
    });

    fetch("/api/bat/set", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(data)
    }).then(() => alert("Gespeichert"));
}

function refreshBat() {
    let table = document.getElementById("bat_table");
    table.innerHTML = '<tr><td colspan="7" style="color:#aaa">Daten werden abgerufen…</td></tr>';
    fetch('/api/bat/refresh', { method: 'POST' })
        .then(() => pollBat(40));
}

function pollBat(retries) {
    if (retries <= 0) {
        renderBatStatus("Timeout: kein BAT-Datensatz empfangen");
        const table = document.getElementById("bat_table");
        table.innerHTML = '<tr><td colspan="7" style="color:orange">Keine Antwort auf <b>bat</b>-Befehl. Bitte in der Battery Console \'bat 1\' manuell testen.</td></tr>';
        return;
    }
    fetch("/api/bat/cells")
        .then(r => r.json())
        .then(j => {
            if (j.headers && j.headers.length > 0) {
                batLoad();
            } else {
                setTimeout(() => pollBat(retries - 1), 2000);
            }
        })
        .catch(() => setTimeout(() => pollBat(retries - 1), 2000));
}

batLoad();
