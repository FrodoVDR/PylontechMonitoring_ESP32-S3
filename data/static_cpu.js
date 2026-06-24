function fmtBytes(b) {
    if (b >= 1048576) return (b / 1048576).toFixed(1) + " MB";
    if (b >= 1024)    return (b / 1024).toFixed(1)    + " KB";
    return b + " B";
}

function updateCpu() {
    fetch("/api/cpu")
        .then(r => r.json())
        .then(data => {
            // Heap info
            document.getElementById("heapFree").textContent       = fmtBytes(data.heap_free);
            document.getElementById("heapMin").textContent        = fmtBytes(data.heap_min);
            document.getElementById("heapPsram").textContent      = fmtBytes(data.heap_psram);
            document.getElementById("heapPsramTotal").textContent = fmtBytes(data.heap_psram_total);

            // Task table
            const tbody = document.querySelector("#cpuTable tbody");
            tbody.innerHTML = "";
            data.tasks.forEach(t => {
                const tr = document.createElement("tr");
                tr.innerHTML = `<td>${t.name}</td><td>${t.state}</td><td>${t.prio}</td><td>${t.stack}</td>`;
                tbody.appendChild(tr);
            });
        })
        .catch(e => console.error("CPU API error:", e));
}

setInterval(updateCpu, 3000);
updateCpu();
