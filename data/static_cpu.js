let cpuChart = null;

function updateCpu() {
    fetch("/api/cpu")
        .then(r => r.json())
        .then(data => {
            const tbody = document.querySelector("#cpuTable tbody");
            tbody.innerHTML = "";

            let idle0 = 0;
            let idle1 = 0;

            data.stats.forEach(line => {
                const parts = line.split(/\s+/);
                const task = parts[0];
                const time = parts[1];
                const percent = parts[2];

                if (task === "IDLE0") idle0 = parseInt(percent);
                if (task === "IDLE1") idle1 = parseInt(percent);

                const tr = document.createElement("tr");
                tr.innerHTML = `<td>${task}</td><td>${time}</td><td>${percent}</td>`;
                tbody.appendChild(tr);
            });

            const load0 = 100 - idle0;
            const load1 = 100 - idle1;

            if (!cpuChart) {
                cpuChart = new Chart(document.getElementById("cpuChart"), {
                    type: "line",
                    data: {
                        labels: [],
                        datasets: [
                            { label: "Core 0 Load %", data: [], borderColor: "red" },
                            { label: "Core 1 Load %", data: [], borderColor: "blue" }
                        ]
                    },
                    options: { animation: false }
                });
            }

            cpuChart.data.labels.push("");
            cpuChart.data.datasets[0].data.push(load0);
            cpuChart.data.datasets[1].data.push(load1);

            if (cpuChart.data.labels.length > 60) {
                cpuChart.data.labels.shift();
                cpuChart.data.datasets[0].data.shift();
                cpuChart.data.datasets[1].data.shift();
            }

            cpuChart.update();
        });
}

setInterval(updateCpu, 2000);
updateCpu();
