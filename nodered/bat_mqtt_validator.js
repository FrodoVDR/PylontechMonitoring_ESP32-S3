// ============================================================
//  Pylontech BAT MQTT Validator / Cleaner für Node-RED
// ------------------------------------------------------------
//  Zweck:
//   - prüft Bezeichner (Keys) UND Werte der bat-Zellpayloads
//   - entfernt unbekannte/fehlerhafte Felder (z. B. HTML-/Konsolen-Müll,
//     der sporadisch in MQTT gelangt: "col1", "heap:", "href='/log'", ...)
//   - verwirft ganze Nachrichten, wenn Pflichtwerte unplausibel sind,
//     damit keine korrupten Datenpunkte in die InfluxDB geschrieben werden
//
//  Verdrahtung:
//     [mqtt in: pylontech/bat/#] -> [diese Function] -> [influxdb out (bat)]
//
//  Function-Node mit ZWEI Ausgängen anlegen (Reiter "Setup" -> Outputs = 2):
//     Ausgang 1 = saubere Nachricht  -> influxdb out
//     Ausgang 2 = verworfen/Fehler   -> Debug-Node (optional, zur Diagnose)
//
//  Beispiel einer GÜLTIGEN Payload:
//   {"Battery":3,"Volt":3.381,"Curr":-0.124,"Tempr":31.1,
//    "SOC":100,"Coulomb":97.067,"BAL":"N","Number":3}
// ============================================================

// ---- Konfiguration ----------------------------------------
const TOPIC_PREFIX = "pylontech/bat";       // erwarteter Topic-Anfang (ohne abschließenden /)
const REQUIRE_TOPIC_MATCH = true;           // Topic-Form prüfen
const CHECK_NUMBER_MATCHES_TOPIC = true;    // payload.Number muss zum Modul im Topic passen

// Erlaubte Felder + Validierung. NUR diese Keys dürfen vorkommen –
// jeder zusätzliche/unbekannte Bezeichner führt zum Verwerfen der Nachricht.
const SCHEMA = {
    Battery: { type: "int",   min: 0,    max: 14 },   // Zellindex 0..14
    Volt:    { type: "float", min: 2.0,  max: 4.0 },  // Zellspannung [V]
    Curr:    { type: "float", min: -250, max: 250 },  // Modulstrom [A]
    Tempr:   { type: "float", min: -20,  max: 60 },   // Temperatur [°C]
    SOC:     { type: "float", min: 0,    max: 100 },  // Ladezustand [%]
    Coulomb: { type: "float", min: 0,    max: 100 },  // Coulomb/SOC [%]
    BAL:     { type: "enum",  values: ["N", "Y"] },   // Balancer-Status
    Number:  { type: "int",   min: 1,    max: 14 }    // Modulnummer
};
// -----------------------------------------------------------

function reject(reason, detail) {
    node.warn("BAT verworfen: " + reason + (detail !== undefined ? " | " + detail : ""));
    node.status({ fill: "red", shape: "ring", text: reason });
    return [null, { topic: msg.topic, payload: { error: reason, detail: detail, original: msg.payload } }];
}

// 1) Payload in Objekt wandeln (Buffer/String/Objekt zulassen)
let data = msg.payload;
if (Buffer.isBuffer(data)) data = data.toString("utf8");
if (typeof data === "string") {
    const s = data.trim();
    // Schneller Müll-Filter: eine gültige Zell-Payload ist ein JSON-Objekt.
    if (s.charAt(0) !== "{") return reject("kein JSON-Objekt", s.slice(0, 60));
    try { data = JSON.parse(s); }
    catch (e) { return reject("JSON-Parsefehler", e.message); }
}
if (typeof data !== "object" || data === null || Array.isArray(data)) {
    return reject("Payload ist kein Objekt");
}

// 2) Topic prüfen  ->  pylontech/bat/<modul>/<zelle>
let topicModule = null;
if (REQUIRE_TOPIC_MATCH) {
    const t = String(msg.topic || "");
    if (t.indexOf(TOPIC_PREFIX + "/") !== 0) {
        return reject("unerwartetes Topic", t);
    }
    const m = t.match(/\/(\d{1,2})\/[^/]+$/);  // .../<modul>/<zelleN>
    if (!m) return reject("Topic-Form ungültig", t);
    topicModule = parseInt(m[1], 10);
}

// 3) Unbekannte Bezeichner ablehnen (fängt HTML-/Konsolen-Müll als Keys)
for (const key of Object.keys(data)) {
    if (!Object.prototype.hasOwnProperty.call(SCHEMA, key)) {
        return reject("unbekanntes Feld '" + key + "'");
    }
}

// 4) Pflichtfelder + Werte prüfen und sauberes Objekt aufbauen
const clean = {};
for (const key of Object.keys(SCHEMA)) {
    if (!Object.prototype.hasOwnProperty.call(data, key)) {
        return reject("Feld fehlt: " + key);
    }
    const rule = SCHEMA[key];
    let v = data[key];

    if (rule.type === "enum") {
        const sv = String(v).trim().toUpperCase();
        if (rule.values.indexOf(sv) < 0) {
            return reject("Wert ungültig für " + key, v);
        }
        clean[key] = sv;
        continue;
    }

    // numerische Felder
    if (typeof v === "string") v = v.trim();
    const num = Number(v);
    if (v === "" || v === null || typeof v === "boolean" || !isFinite(num)) {
        return reject("nicht-numerischer Wert für " + key, data[key]);
    }
    if (rule.type === "int" && !Number.isInteger(num)) {
        return reject(key + " ist nicht ganzzahlig", data[key]);
    }
    if (num < rule.min || num > rule.max) {
        return reject(key + " außerhalb [" + rule.min + ".." + rule.max + "]", data[key]);
    }
    clean[key] = num;
}

// 5) Konsistenz: payload.Number muss zum Modul aus dem Topic passen
if (CHECK_NUMBER_MATCHES_TOPIC && topicModule !== null && clean.Number !== topicModule) {
    return reject("Number != Topic-Modul", clean.Number + " vs " + topicModule);
}

// 6) Saubere Nachricht weiterreichen
//    Hinweis (optional): Wenn Number/Battery in InfluxDB als TAGS statt als
//    Felder gespeichert werden sollen, stattdessen so weitergeben:
//      const tags = { Number: clean.Number, Battery: clean.Battery };
//      const fields = { Volt: clean.Volt, Curr: clean.Curr, Tempr: clean.Tempr,
//                       SOC: clean.SOC, Coulomb: clean.Coulomb, BAL: clean.BAL };
//      msg.payload = [fields, tags];
msg.payload = clean;
node.status({ fill: "green", shape: "dot", text: "ok M" + clean.Number + " C" + clean.Battery });
return [msg, null];
