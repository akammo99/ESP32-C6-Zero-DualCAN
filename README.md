> **Work in Progress:** Currently adapting this fork for the Waveshare ESP32-C6-Zero (Pinout, BLE fix, 3D enclosure, Android APK).

# ESP32-DualCAN (Waveshare ESP32-C6-Zero Edition)

Dieses Projekt ist ein angepasster Fork von **[ESP32-DualCAN / Dorky Commander]([Link zum Original-Repo])**, einer Open-Source-Alternative zum S3XY Commander mit Molex-Anschluss (kompatibel mit Enhauto-Kabelbäumen).

## Änderungen & Herkunftshinweis (gemäß CERN-OHL-W)

Gemäß Abschnitt 3.3(b) der **CERN Open Hardware Licence Version 2 - Weakly Reciprocal (CERN-OHL-W)** wurden am **19.11.2026** von **[akammo99]** folgende Änderungen am Originalprojekt vorgenommen:

* **Pinout-Anpassung:** Das Pinout der Firmware wurde für das **Waveshare ESP32-C6-Zero** Board angepasst.
* **3D-Gehäuse hinzugefügt:** Ein passendes 3D-druckbares Gehäuse wurde konstruiert, da die Waveshare-Platine von der anderen Seite angelötet wird.
* **Bluetooth-Fix:** Probleme mit der Bluetooth-Verbindung (BLE) nach dem Auslesen des CAN-Busses auf dem ESP32-C6-Zero wurden behoben.
* **Android APK & OpenRouter-Erweiterung:** Aus der Web-UI wurde eine eigenständige Android-APK gebaut und der integrierte AI-Assistent um **OpenRouter** erweitert (zusätzlich zu Anthropic, für freie Modellwahl).

---

## Funktionen & Kurzanleitung

* **Dual CAN** zum Auslesen und Senden von Fahrzeugdaten
* **Bluetooth (BLE)** für die App / Web-UI, OTA-Updates und TeslaBLE
* **WLAN** (integrierter Datei-Browser) & **USB-C** zum Flashen und Debuggen

### Schnelleinstieg
1. **Verbinden:** Installierte APK (oder Web-UI in Chrome) öffnen, auf **Connect** tippen und **Dorky Commander** auswählen. *(Falls sich das Gerät nicht koppeln lässt: **B-Taste** kurz drücken, um das 60-Sekunden-Kopplungsfenster zu öffnen).*
2. **Fahrzeug wählen:** Oben auf das Auto-Symbol tippen und das Fahrzeug (z. B. Tesla Model 3/Y) auswählen, um die passenden CAN-DBC-Definitionen zu laden.
3. **Skripte & Kacheln nutzen:** Unter **Gallery → Scripts** fertige Automationen (z. B. Spiegel einklappen oder Fenster absenken) installieren und aktivieren.
4. **AI-Assistent (OpenRouter & Anthropic):** Unter **Settings → AI assistant** deinen **OpenRouter**- oder **Anthropic**-API-Key hinterlegen, um eigene Berry-Skripte per Texteingabe generieren zu lassen.

*(Detaillierte Anleitungen zum Flashen, Kompilieren und Schreiben eigener Berry-Skripte befinden sich in den jeweiligen Unterordnern `hardware/`, `firmware/`, `webui/` und `docs/`).*

---

## Lizenz

Dieses Projekt steht weiterhin unter der **CERN Open Hardware Licence Version 2 - Weakly Reciprocal (CERN-OHL-W)**. Alle ursprünglichen Copyright- und Haftungshinweise bleiben bestehen. Details sind in der Datei `LICENSE` zu finden.
