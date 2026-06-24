# CheckMK Custom Agent Setup für PylontechMonitoring ESP32-S3

## Option 2: Remote Monitoring vom CheckMK-Server

Der Custom Agent Plugin läuft **direkt auf dem CheckMK-Server** und verbindet sich zum ESP32. Der ESP wird als CheckMK-Host konfiguriert und bekommt Services automatisch zugewiesen.

---

## Deployment-Schritte

### 1. Dateien auf CheckMK-Server deployen

Bei Docker-Setup die Datei **in den laufenden CheckMK-Container** und dort in die Site kopieren.
Beispiel mit Container `ckeckmk` und Site `main`:

```bash
# Vom Admin-Host aus in den Container kopieren
docker cp pylontech_custom_agent.py ckeckmk:/omd/sites/main/local/share/check_mk/agents/special/agent_pylontech

# Rechte im Container setzen
docker exec ckeckmk bash -lc 'chown main:main /omd/sites/main/local/share/check_mk/agents/special/agent_pylontech && chmod 755 /omd/sites/main/local/share/check_mk/agents/special/agent_pylontech'
```

### 2. WATO Konfiguration (Web UI)

#### Variante A: Über bestehende CheckMK-Regel (empfohlen)

1. **Web UI öffnen**: Site `main`
2. In der Suche nach **Individual program call instead of agent access** suchen
3. Navigationspfad (CheckMK 2.4): **Setup > Agents > Access to agents > Individual program call instead of agent access**
3. **Create rule** klicken
4. Folgende Parameter setzen:
    - **Explicit hosts**: nur den Zielhost (hier: `pylontech`)
   - **Command line to execute**:
       `/omd/sites/main/local/share/check_mk/agents/special/agent_pylontech --host 192.168.88.20 --port 80 --timeout 3`
5. **Save** klicken

#### Variante B: Manuell in `rules.mk` (für schnelle Tests)

SSH auf CheckMK-Server:

```bash
# Datei der passenden WATO-Ordnerregel bearbeiten (Beispiel)
vim /omd/sites/main/etc/check_mk/conf.d/wato/<dein_ordner>/rules.mk
```

Wichtig: **Nicht** die alte Tuple-Syntax verwenden.
Für CheckMK 2.4 muss `datasource_programs` als Dict-Rule eingetragen werden, z.B.:

```python
globals().setdefault('datasource_programs', [])

datasource_programs = [
{'id': 'pylontech-agent-rule-1', 'value': '/omd/sites/main/local/share/check_mk/agents/special/agent_pylontech --host 192.168.88.20 --port 80 --timeout 3', 'condition': {'host_name': ['pylontech'], 'host_folder': '/%s/' % FOLDER_PATH}, 'options': {'disabled': False, 'description': 'Pylontech custom agent'}},
] + datasource_programs
```

3. **Konfiguration neu laden**:

```bash
docker exec ckeckmk bash -lc 'su - main -c "cmk -U --debug"'
```

---

## ESP32 Host im CheckMK erstellen/konfigurieren

### Option A: Host existiert bereits

1. **Setup > Hosts** öffnen
2. Host `pylontech` suchen/bearbeiten
3. **Data sources** Tab öffnen
4. Sicherstellen, dass die Datasource Program Rule matched (grünes Häkchen)
5. **Activate changes** klicken

### Option B: Host neu erstellen

1. **Setup > Add host** klicken
2. **Host name**: `pylontech`
3. **IPv4 address**: `192.168.88.20` (optional, wenn Hostname DNS-auflösbar)
4. **Monitoring agents** > **Custom datasources**:
   - Sicherstellen, dass die Pylontech Rule aktiv ist
5. **Save** klicken

---

## Service Discovery & Überwachung starten

1. **Setup > Hosts** > Host `pylontech` auswählen
2. **Services** Button (oder "Discover services")
3. **Full scan** oder **New services**
4. Folgende Services sollten auftauchen:
   - `Pylontech Uptime`
   - `Pylontech CPU`
   - `Pylontech Memory`
   - `Pylontech PSRAM`
   - `Pylontech SPIFFS`
   - `Pylontech NVS`
   - `Pylontech Monitoring` (Status des API-Calls selbst)

5. **Accept all** oder einzeln akzeptieren
6. **Activate changes** klicken

---

## Testing

### Manuell vom CheckMK-Server aus testen:

```bash
cd /tmp
/omd/sites/main/local/share/check_mk/agents/special/agent_pylontech \
   --host 192.168.88.20 \
  --port 80 \
  --timeout 3
```

Erwartete Ausgabe (CheckMK Format):
```
0 Pylontech Uptime - Uptime: 2d 5h 14m 23s (...)
0 Pylontech CPU cpu_load=45% | cpu_load=45%;85;95 cpu_core0=42% cpu_core1=48%
0 Pylontech Memory OK - Heap 58% used (...)
0 Pylontech PSRAM OK - PSRAM 72% used (...)
0 Pylontech SPIFFS OK - 12% used (...)
0 Pylontech NVS OK - 145 entries in use
```

### Im CheckMK Dashboard:

1. **Monitoring > Hosts** > Host suchen
2. **Services** Tab öffnen
3. Alle Pylontech-Services sollten mit grünem Status (0=OK) oder ggfs. Gelb/Rot angezeigt werden

---

## Problembehebung

### "Agent failed to get data"

```bash
# 1. Netzwerk-Verbindung prüfen
ping 192.168.88.20

# 2. HTTP-Endpunkt testen
curl -s http://192.168.88.20/api/monitoring | python3 -m json.tool

# 3. Agent direkt vom Server aus starten (oben)
```

### Plugin nicht im WATO sichtbar

1. In CheckMK 2.4 ist für diesen Fall **kein eigenes WATO-Plugin nötig**.
2. Nutze die eingebaute Regel: **Individual program call instead of agent access**.
3. Falls die Suche nichts zeigt: Berechtigung prüfen (`Can add or modify executables`).

### Service Discovery findet keine Services

1. Host-Datasource prüfen: **Setup > Hosts** > Host > **Datasources**
2. Sicherstellen, dass die Pylontech-Rule aktiv ist (blauer Haken)
3. **Full scan** statt **New services** versuchen

---

## Konfigurationsparameter (ab CheckMK 2.1)

Falls du später Thresholds anpassen möchtest, kannst du diese in `pylontech_custom_agent.py` ändern:

```python
# ca. Zeile 100
cpu_warn, cpu_crit = 85, 95        # CPU % Warnung/Kritisch
mem_warn, mem_crit = 85, 95        # Heap % Warnung/Kritisch
psram_warn, psram_crit = 85, 95    # PSRAM % Warnung/Kritisch
spiffs_warn, spiffs_crit = 85, 95  # SPIFFS % Warnung/Kritisch
nvs_warn, nvs_crit = 85, 95        # NVS % Warnung/Kritisch
heap_warn, heap_crit = 65536, 32768  # Heap KB Warnung/Kritisch
```

Nach Änderung: Datei neu deployen und **Activate changes** im CheckMK.

---

## Zusammenfassung

✅ Custom Agent läuft vom **CheckMK-Server** aus  
✅ Verbindet sich zum **ESP32** über Netzwerk  
✅ Holt Daten von `/api/monitoring`  
✅ Konvertiert zu **CheckMK Services** (Uptime, CPU, Memory, Storage)  
✅ Über eingebaute CheckMK-Regel **Individual program call instead of agent access** konfigurierbar  
✅ **Performance-Daten** mit Schwellwerten  

Der Check kann jetzt mit beliebigen ESP-Devices verwendet werden — einfach Host-Parameter anpassen!

