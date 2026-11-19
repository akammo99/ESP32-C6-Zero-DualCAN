# OTA-Rollback-Schutz (ESP32-C6)

Dieses Projekt aktiviert jetzt `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` und bestätigt ein ausstehendes OTA-Image erst dann aus der Anwendung heraus, wenn die Startdienste laufen, gespeicherte Skripte wiederhergestellt wurden und die Startverzögerung abgelaufen ist.

## Wichtige Voraussetzung bei der Erstinstallation

Die Rollback-Option ist Teil der ESP-IDF-Bootloader-Konfiguration. Ein reines Aktualisieren der Anwendung über BLE ersetzt **nicht** einen bereits installierten Bootloader. Um den Rollback-Schutz auf einem Gerät zu aktivieren, das aktuell noch einen älteren Bootloader nutzt, muss einmalig ein vollständiger Flash-Vorgang über Serial/USB durchgeführt werden, der den neu erstellten Bootloader, die Partitionstabelle und die Anwendung umfasst. Nachfolgende OTA-Updates können dann den Rollback-Schutz nutzen.

Gehe nicht davon aus, dass diese ZIP-Datei bereits auf echter Hardware kompiliert oder getestet wurde. Überprüfe zuerst den PlatformIO-Build und halte eine bewährte Wiederherstellungsmethode bereit, bevor du ein fest verbautes Modul aktualisierst.

## Partitionslayout

Die bestehende `partitions.csv` definiert bereits zwei OTA-Anwendungs-Slots (`ota_0` und `ota_1`) und bleibt unverändert erhalten. Die LittleFS-Partition bleibt ebenfalls erhalten.