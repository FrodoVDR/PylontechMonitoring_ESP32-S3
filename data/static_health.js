// ---------------------------------------------------------
// Auto‑Reload Steuerung
// ---------------------------------------------------------
let healthAutoReload = true;


// ---------------------------------------------------------
// Hauptfunktion: Health laden
// ---------------------------------------------------------
function loadHealth() {
    fetch("/api/health")
        .then(r => r.json())
        .then(data => {

            // Header
            let h = document.getElementById("health_header");
            h.innerText = "Health: " + data.strongest;
            h.style.background =
                data.color === "green" ? "#00cc00" :
                data.color === "yellow" ? "#ffcc00" :
                "#ff4444";

            // Module
            let tbody = document.querySelector("#health_table tbody");
            tbody.innerHTML = "";

            data.modules.forEach(m => {
                let row = document.createElement("tr");
                row.innerHTML = `
                    <td>${m.index}</td>
                    <td>${m.status}</td>
                    <td>${m.tempMax.toFixed(2)}°C</td>
                    <td>${m.cellMin.toFixed(3)}V</td>
                    <td>${m.cellMax.toFixed(3)}V</td>
                    <td>${m.cellDiff.toFixed(3)}V</td>
                `;
                tbody.appendChild(row);
            });

            // Stack
            document.getElementById("stack_info").innerText =
                `Min: ${data.stack.cellMin.toFixed(3)}V, ` +
                `Max: ${data.stack.cellMax.toFixed(3)}V, ` +
                `Delta: ${data.stack.cellDiff.toFixed(3)}V`;

            // Listen
            document.getElementById("ok_list").innerText   = data.ok.join(", ");
            document.getElementById("warn_list").innerText = data.warn.join(", ");
            document.getElementById("err_list").innerText  = data.error.join(", ");

            // Historie
            document.getElementById("warn_hist").innerText = data.warnHistory.join(", ");
            document.getElementById("err_hist").innerText  = data.errorHistory.join(", ");

            // Schwellwerte anzeigen
            if (data.config) {
                document.getElementById("cellDiffWarn").value  =
                    data.config.cellDiffWarn.toFixed(3);

                document.getElementById("cellDiffError").value =
                    data.config.cellDiffError.toFixed(3);
            }
        });
}


// ---------------------------------------------------------
// Reset der Health‑History
// ---------------------------------------------------------
function healthReset() {
    fetch("/api/health/reset")
        .then(() => loadHealth());
}


// ---------------------------------------------------------
// Schwellwerte speichern
// ---------------------------------------------------------
function saveHealthConfig() {

    // Auto‑Reload wieder aktivieren
    healthAutoReload = true;

    let warn  = parseFloat(document.getElementById("cellDiffWarn").value);
    let error = parseFloat(document.getElementById("cellDiffError").value);

    fetch("/api/health", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
            cellDiffWarn: warn,
            cellDiffError: error
        })
    })
    .then(r => r.text())
    .then(() => loadHealth());
}


// ---------------------------------------------------------
// Auto‑Reload alle 2 Sekunden (nur wenn erlaubt)
// ---------------------------------------------------------
setInterval(() => {
    if (healthAutoReload) loadHealth();
}, 2000);


// ---------------------------------------------------------
// Initialer Load
// ---------------------------------------------------------
loadHealth();
