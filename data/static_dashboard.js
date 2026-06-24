function loadDashboard() {
    fetch("/api/dashboard")
        .then(r => r.json())
        .then(d => {
            wifi_mode.textContent = d.wifi.mode;
            wifi_ssid.textContent = d.wifi.ssid;
            wifi_ip.textContent = d.wifi.ip;
            wifi_mac.textContent = d.wifi.mac || "–";
            wifi_rssi.textContent = d.wifi.rssi;

            if (d.lan) {
                lan_status.textContent = d.lan.linked
                    ? (d.lan.connected ? "Connected" : "Link up, no IP")
                    : (d.lan.enabled ? "Disconnected" : "Disabled");
                lan_ip.textContent    = d.lan.ip    || "–";
                lan_mac.textContent   = d.lan.mac   || "–";
                lan_speed.textContent = d.lan.speed ? d.lan.speed + " Mbit/s" : "–";
            } else {
                lan_status.textContent = "n/a";
                lan_ip.textContent = lan_mac.textContent = lan_speed.textContent = "–";
            }

            mqtt_status.textContent = d.mqtt.connected ? "Connected" : "Disconnected";
            mqtt_server.textContent = d.mqtt.server + ":" + d.mqtt.port;
            mqtt_last.textContent = d.mqtt.last_contact;
            bat_modules.textContent = d.battery.modules;
            bat_soc.textContent = (d.battery.soc !== undefined && d.battery.soc !== null) ? d.battery.soc : "–";
            bat_last.textContent = d.battery.last_update;
            
            // Stack Status
            if (d.stack) {
                document.getElementById("stack_volt").textContent = d.stack.voltage.toFixed(2);
                document.getElementById("stack_curr").textContent = d.stack.current.toFixed(2);
                document.getElementById("stack_power").textContent = d.stack.power.toFixed(1);
                document.getElementById("stack_power_in").textContent = d.stack.power_in.toFixed(1);
                document.getElementById("stack_power_out").textContent = d.stack.power_out.toFixed(1);
                document.getElementById("stack_temp").textContent = (d.stack.temperature / 1000).toFixed(1);
            }
            
            // Use backend-formatted time (which respects system timezone)
            sys_time.textContent = d.system.time || "–";
            
            // Debug: show UTC and offset if available
            let tz_info = d.system.timezone || "";
            if (d.system.offset !== undefined) {
                tz_info += " (UTC" + (d.system.offset >= 0 ? "+" : "") + d.system.offset + ")";
            }
            if (d.system.utc) {
                tz_info += " [UTC: " + d.system.utc + "]";
            }
            sys_timezone.textContent = tz_info ? "(" + tz_info + ")" : "";
            sys_uptime.textContent = d.system.uptime;
            sys_fw.textContent = d.system.version;
        })
        .catch(err => console.error("Dashboard update error:", err));
}

// Initial load and auto-refresh every 5 seconds
loadDashboard();
setInterval(loadDashboard, 5000);

function dashReboot() {
    if (!confirm("Reboot device?")) return;
    let msg = document.getElementById("reboot_msg");
    msg.textContent = "Rebooting…";
    fetch("/api/restart", { method: "POST" })
        .then(() => {
            msg.textContent = "Reboot sent. Reconnecting…";
            setTimeout(() => location.reload(), 8000);
        })
        .catch(() => {
            msg.textContent = "Reboot sent.";
            setTimeout(() => location.reload(), 8000);
        });
}