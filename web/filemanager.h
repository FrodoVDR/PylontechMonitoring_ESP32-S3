#pragma once
#include <Arduino.h>

const char FILEMANAGER_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>File Manager</title>
<style>
body { font-family: sans-serif; padding: 20px; }
table { border-collapse: collapse; width: 100%; margin-top: 20px; }
td, th { border: 1px solid #ccc; padding: 6px; }
button { padding: 6px 12px; margin-left: 10px; }
</style>
</head>
<body>

<h2>ESP32 File Manager</h2>

<input type="file" id="file" multiple>
<button onclick="upload()">Upload</button>

<progress id="prog" value="0" max="100" style="display:none"></progress>
<span id="progtext"></span>

<table id="tbl"></table>

<script>
function load() {
    fetch('/fm/list')
    .then(r => r.json())
    .then(list => {
        let html = "<tr><th>Name</th><th>Size</th><th>Action</th></tr>";
        list.forEach(f => {
            html += `<tr>
                <td>${f.name}</td>
                <td>${f.size}</td>
                <td>
                    <a href="/fm/download?file=${encodeURIComponent(f.name)}">Download</a>
                    <a href="#" onclick="del('${f.name}')">Delete</a>
                </td>
            </tr>`;
        });
        document.getElementById("tbl").innerHTML = html;
    });
}

async function upload() {
    let files = document.getElementById("file").files;
    if (!files.length) return;

    let prog = document.getElementById("prog");
    let progtext = document.getElementById("progtext");

    prog.style.display = "inline-block";
    prog.value = 0;
    progtext.innerText = "0%";

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
                    progtext.innerText = `Datei ${i+1}/${files.length}: ${p}%`;
                }
            };

            xhr.onload = function() {
                if (xhr.status === 200) resolve();
                else reject();
            };

            xhr.onerror = reject;
            xhr.send(fd);
        });
    }

    prog.value = 100;
    progtext.innerText = "Fertig!";
    load();
}

function del(name) {
    fetch("/fm/delete?file=" + encodeURIComponent(name))
    .then(() => load());
}

load();
</script>

</body>
</html>
)rawliteral";
