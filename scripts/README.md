Skripte — Beiträge zur Galerie (Tesla Model Y Legacy)

Der Tab Gallery → Scripts in der Web-UI (und der Android-APK) wird beim Kompilieren automatisch aus diesem Ordner erzeugt. Es ist keine manuelle Registrierung nötig – lege einfach eine .be-Datei hier ab, und sie erscheint direkt als Karte in der App. Die vollständige Berry-API findest du in docs/scripting.md (oder direkt in der App über Automations → Scripting Guide).

[!NOTE]
Derzeit wird ausschließlich Tesla (Model Y / Model 3) unterstützt und ist auf der Hardware getestet (siehe scripts/tesla/).

Speicherort der Dateien

scripts/
  tesla/           # Tesla Model Y
  README.md        # diese Datei


Speichere deine Skripte unter scripts/tesla/dein_skript.be (der Ordnername tesla muss kleingeschrieben bleiben, damit die Galerie die Karten automatisch anzeigt, sobald das Tesla Model Y in der Statusleiste ausgewählt ist – geregelt über app.car.brand → examples.ts:11 / GalleryView.svelte:11-16).

Ebenfalls indexiert werden Dateien unter firmware/test_scripts/*.be (diese erscheinen unter Automations → Load…, nicht in der Galerie).

Quelle: webui/src/examples.ts:7 — import.meta.glob(['../../scripts/tesla/*.be', '../../firmware/test_scripts/*.be'], { query: '?raw' }).

Erforderlicher Datei-Header

Die ersten Zeilen jedes Skripts müssen # @-Kommentare sein – daraus werden Titel, Beschreibung und Bus-Anzeige der Galerie-Karte generiert:

# @name Meine Funktion
# @description Was das Skript macht, wann es auslöst und welches Model-Y-Signal/Bus genutzt wird
# @bus 0


@name — Kartentitel (Fallback: Dateiname)

@description — Untertitel / Suchtext der Karte (mehrzeilige @description-Zeilen werden unterstützt)

@bus — 0 oder 1 (Standard: 0) – wird als bus 0-Abzeichen angezeigt und für die DBC-Vorverarbeitung genutzt

Minimales Grundgerüst (aus firmware/README.md):

# @name Mein Model Y Skript
# @description Beschreibung der Funktion

def setup()
  # wird beim Aktivieren des Skripts aufgerufen
  timer_every(1000, def()
    print("tick")
  end)
end

def teardown()
  # wird beim Deaktivieren des Skripts aufgerufen (optional)
end


Minimale Praxisbeispiele findest du unter scripts/tesla/hello_log.be (einfaches Logging ohne CAN) und scripts/tesla/tesla_fold_mirror_tile.be (zwei Dashboard-Kacheln zum Einklappen der Spiegel).

Skripte schreiben

Die Laufzeitumgebung ruft beim Aktivieren setup() und beim Deaktivieren (falls vorhanden) teardown() auf – typischerweise werden in setup() Timer, CAN-Callbacks oder Dashboard-Aktionen registriert und in teardown() wieder freigegeben.

Nutze can_msg_get(bus, "MessageName") / msg_sig_get(msg, "SignalName") mit den DBC-Signalnamen des Model Y; beim Speichern werden diese automatisch in numerische sb/len/scale-Werte umgeschrieben. Details zu Timern, action_register, led_set, state_* und zur Syntax stehen in docs/scripting.md bzw. in der App unter Automations → Scripting Guide.

Halte die Skripte schlank – sie laufen auf dem ESP32-C6 mit begrenztem Arbeitsspeicher (Heap) und CPU-Leistung. Trotz Schutzmechanismen kann ein überladenes Skript im schlimmsten Fall die BLE-Verbindung blockieren, sodass ein Werksreset per USB nötig wird.

Neue Skripte erscheinen automatisch in der Galerie. Ein Store-Update ist nicht erforderlich – Vite bindet die .be-Inhalte beim Bauen der Web-UI/APK automatisch ein.

Tipps

Wenn du externe KI-Assistenten (oder OpenRouter in der APK) zum Schreiben von Model-Y-Skripten nutzt, verweise auf docs/scripting.md (Raw-Link: https://raw.githubusercontent.com/akammo99/ESP32-C6-Zero-DualCAN/main/docs/scripting.md) – das ist dieselbe Anleitung, die auch der integrierte KI-Editor verwendet.

Verwende für Dateinamen durchgehend Kleinschreibung mit Unterstrichen (snake_case.be), da die Dateien auf dem ESP32-C6 im LittleFS-Pfad /scripts/ gespeichert werden.