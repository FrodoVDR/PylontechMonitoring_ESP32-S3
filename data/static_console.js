function sendCmd() {
    const cmd = document.getElementById('cmdline').value;
    if (cmd.length === 0) return;
    runCmd(cmd);
    document.getElementById('cmdline').value = '';
}

function quickCmd(c) {
    runCmd(c);
}

function runCmd(cmd) {
    const box = document.getElementById('rawout');
    box.value = '> ' + cmd + '\nwarte auf Antwort ...';
    box.scrollTop = box.scrollHeight;

    fetch('/req?code=' + encodeURIComponent(cmd))
        .then(r => r.text())
        .then(seqStr => {
            const baseSeq = parseInt(seqStr, 10) || 0;
            pollFrame(cmd.trim().toLowerCase(), baseSeq, Date.now());
        })
        .catch(() => { box.value = 'Fehler beim Senden des Befehls'; });
}

function pollFrame(cmd, baseSeq, started) {
    // Scheduled poll commands have priority, so a console command can take a
    // while. Poll up to 18s and only accept a fresh frame for our command.
    const TIMEOUT_MS = 18000;
    fetch('/api/lastframe')
        .then(r => r.json())
        .then(d => {
            const box = document.getElementById('rawout');
            const fresh = d.seq > baseSeq;
            const match = (d.cmd || '').trim().toLowerCase() === cmd;
            if (fresh && match) {
                box.value = d.text;
                box.scrollTop = box.scrollHeight;
                return;
            }
            if (Date.now() - started > TIMEOUT_MS) {
                box.value = 'TIMEOUT - keine Antwort fuer: ' + cmd;
                return;
            }
            setTimeout(() => pollFrame(cmd, baseSeq, started), 400);
        })
        .catch(() => setTimeout(() => pollFrame(cmd, baseSeq, started), 400));
}

document.getElementById('cmdline').addEventListener('keydown', e => {
    if (e.key === 'Enter') {
        e.preventDefault();
        sendCmd();
    }
});
