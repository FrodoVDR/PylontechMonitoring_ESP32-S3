# Hardware – Pylontech Monitoring RS-232 Interface (KiCad)

KiCad-Schaltplan als Ausgangspunkt für eine eigene Trägerplatine (Carrier-Board),
die das **Waveshare ESP32-S3-ETH** als Steckmodul aufnimmt und den **MAX3232**
RS-232-Pegelwandler samt Batterieanschluss und optionalem ST7735-Display integriert.

> **Hinweis:** Dies ist ein **Ausgangs-Schaltplan**, kein fertiges Fabrikations-Layout.
> Pinbelegungen (besonders die GPIO-Zuordnung des Waveshare-Boards und die RJ45/RJ11-
> Steckerbelegung) **vor dem Bestellen einer Platine gegen die realen Bauteile prüfen**.

## Dateien

| Datei | Zweck |
|---|---|
| `PylontechInterface.kicad_pro` | KiCad-Projektdatei |
| `PylontechInterface.kicad_sch` | Schaltplan (in KiCad 10 getestet, öffnet mit `kicad-cli`) |
| `PylontechInterface.kicad_prl` | lokaler Projektzustand (von KiCad erzeugt) |
| `gen_schematic.py` | Generator-Skript, das den Schaltplan erzeugt (Pin-Endpunkte exakt berechnet) |

> Der Schaltplan wird von `gen_schematic.py` generiert. Zum Regenerieren:
> `python3 gen_schematic.py` (aus dem `hardware/`-Ordner). Manuelle Änderungen
> in der GUI werden dabei überschrieben – dann besser das Skript anpassen oder
> nur noch in KiCad weiterarbeiten.

## Öffnen

```bash
# GUI
kicad hardware/PylontechInterface.kicad_pro

# Nur Schaltplan als SVG exportieren (Validierung)
kicad-cli sch export svg --output ./out hardware/PylontechInterface.kicad_sch
```

## Bauteilliste (BOM)

| Ref | Bauteil | Wert / Typ | Anmerkung |
|---|---|---|---|
| J1 | Stiftleiste 1×8 | 2,54 mm | Aufnahme für ESP32-S3-ETH (GPIO16/17, 3V3, GND) |
| U1 | MAX3232 | SOIC-16 / DIP-16 | RS-232 ↔ TTL Pegelwandler |
| C1 | Kondensator | 10 µF | Ladepumpe / Stabilisierung |
| C2 | Kondensator | 0,1 µF | Abblockung |
| J2 | RJ45- oder RJ11-Buchse | 8P8C / 6P4C | Batterie-Konsolenport |
| J3 | Stiftleiste 1×8 *(optional)* | 2,54 mm | ST7735 TFT-Display (SPI3/HSPI) |

Die MAX3232-Ladepumpen-Kondensatoren (an C1+/C1-/V+/V-) sind laut Datenblatt zusätzlich
zu bestücken (typ. 4× 100 nF bzw. 1 µF je nach MAX3232-Variante) – im Schaltplan als
Pins vorhanden, Bestückung modulabhängig.

## Signalzuordnung

### RS-232 (MAX3232 ↔ ESP32)

| ESP32-Pin | MAX3232 | Richtung |
|---|---|---|
| GPIO17 (TX) | T1IN | ESP → Batterie |
| GPIO16 (RX) | R1OUT | Batterie → ESP |
| 3V3 | VCC | Versorgung |
| GND | GND | Masse |

### Batterie-Konsolenport (J2)

| RJ45 | RJ11 | MAX3232 |
|---|---|---|
| Pin 3 (weiß-grün) | Pin 2 | R1IN |
| Pin 6 (grün) | Pin 3 | T1OUT |
| Pin 8 (braun) | Pin 4 | GND |

### Optionales TFT-Display (J3, ST7735, SPI3/HSPI)

| ESP32-Pin | Display |
|---|---|
| GPIO 18 | SDA / MOSI |
| GPIO 15 | SCK / CLK |
| GPIO 38 | CS |
| GPIO 39 | DC / RS |
| GPIO 40 | RST |
| GPIO 41 | BL (PWM) |
| 3V3 / GND | VCC / GND |

> Das Display nutzt **SPI3/HSPI** und kollidiert nicht mit dem W5500-Ethernet (SPI2/FSPI,
> GPIO 11–14). MISO wird nicht angeschlossen (ST7735 ist write-only).
>
> **GPIO 4–7 sind auf dem Waveshare ESP32-S3-ETH fest mit dem TF-Kartenslot verdrahtet**
> (SD_CS=4, SD_MISO=5, SD_MOSI=6, SD_CLK=7) und stehen am Pin-Header nicht zur Verfügung –
> deshalb MOSI/SCK auf die freien Pins GPIO 18/15 gelegt.

## Nächste Schritte zum PCB

1. Schaltplan in KiCad öffnen und Pinbelegung/Footprints prüfen.
2. Footprints zuweisen (Symbole nutzen generische Bibliotheken).
3. **PCB Editor** öffnen → Netzliste importieren → Bauteile platzieren.
4. Board-Umriss + Befestigungsbohrungen definieren (bestimmen später das Gehäuse).
5. Gerber-Export → Fertigung (JLCPCB/PCBWay).
6. Nach Erhalt die realen PCB-Maße messen → Gehäuse (z. B. OpenSCAD).
