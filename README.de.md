# OpenRGB-KM-P6-Plugin

[English](README.md)

Ein Plugin für [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) **1.1** für das RGB-Mauspad **Aukey KM-P6**
(USB `04D9:A1FA`, Holtek, Produktname „USB Gaming Keyboard“) unter **Linux**.

Das Protokoll stammt aus dem [KM-P6 RGB Tool](https://github.com/Tom-8192/aukey-km-p6-rgb-control) (dort
`docs/PROTOCOL.md`); das Plugin sendet dieselben Reports.

## Warum ein Plugin

OpenRGB hat einen eigenen Treiber für das Pad („Holtek Mousemat“), sucht es aber per hidapi auf USB-Interface 2.
Dieses Interface hat **keinen Endpoint**, deshalb lehnt Linux' `usbhid` es ab (`couldn't find an input interrupt
endpoint`), und es gibt kein `/dev/hidraw*` dafür – OpenRGB sieht das Pad unter Linux nie. Das Plugin schickt
denselben Feature-Report stattdessen als Control-Transfer über usbfs (`/dev/bus/usb/BBB/DDD`). Das
Tastatur-Interface 0 behält seinen Treiber.

Unter **Windows** braucht man das Plugin nicht (es wird dort auch nicht gebaut): Dort funktioniert der eingebaute
Treiber von OpenRGB.

## Verhalten

Unter *Devices → Aukey KM-P6* hat das Pad eine LED (*Mouse pad*; Rand und Logo haben immer dieselbe Farbe) und diese
Modi, in der Reihenfolge, in der die Taste am Pad sie durchschaltet:

| Modus | Einstellungen |
|---|---|
| Static | Farbe, Helligkeit 0–9 |
| Wave | Helligkeit, Geschwindigkeit 0–4, Richtung (links/rechts sehen am Pad gleich aus) |
| Breathing | Farbe, Helligkeit, Geschwindigkeit |
| Neon | Helligkeit, Geschwindigkeit |
| Off | – |

Helligkeit und Geschwindigkeit sind die Stufen der Herstellersoftware G-aim. Der eingebaute OpenRGB-Treiber hat eine
feste Helligkeit und kein „Aus“; den Farbwechsel des Pads („alle Farben durchlaufen“) lässt das Plugin bewusst weg.

- Eine Einstellung wird gesendet, sobald sie eine halbe Sekunde unverändert bleibt (beim Ziehen am Farbwähler oder
  an einem Regler geht nur das Endergebnis raus). Einen Modus wählen oder einen Regler bewegen sendet immer, auch
  ohne Änderung – hat die Taste am Pad den Effekt umgeschaltet, holt erneutes Anwenden ihn zurück.
- Farb-Updates ohne neue Farbe (Effekte, SDK-Clients, die sie wiederholen) senden nichts.
- Es gibt **keinen Direct-Modus**: Das Pad behält seine Einstellung ohne Strom, schreibt also vermutlich jede
  Einstellung in seinen Flash.
- **Der Start von OpenRGB sendet nichts.** Das Pad lässt sich nicht auslesen, deshalb zeigt die Geräteansicht, was
  das Plugin zuletzt gesendet hat (gespeichert in `plugins/settings/KMP6.json` im OpenRGB-Konfigurationsordner; vor
  der ersten Änderung die Werkseinstellung).
- **OpenRGB-Profile** speichern die Einstellung im Plugin-Abschnitt des Profils; Laden eines Profils sendet sie
  (auch wenn das Pad erst danach angesteckt wird).
- Ab- und Anstecken wird erkannt; das Gerät verschwindet und kommt wieder.
- Das Plugin teilt sich die Sperre mit dem KM-P6 RGB Tool (`flock` auf den usbfs-Knoten; Interface 2 wird nur
  während des Sendens belegt), beide können gleichzeitig laufen.
- Eine Statusseite gibt es unter *Settings → KMP6*; Meldungen landen im OpenRGB-Log mit dem Präfix `[KMP6]`.
- Static und Breathing schreiben auch die Werksfarben von G-aim in die Farbplätze 2–7 des Pads (die
  Herstellersoftware sendet sie bei jeder Static-/Breathing-Einstellung mit); Farben, die das KM-P6 RGB Tool dort
  abgelegt hat, werden ersetzt.

## Getestet

Bazzite (Fedora, Kernel 7.2), OpenRGB-1.1-Flatpak (KDE-Runtime 6.10), Plugin gebaut unter Ubuntu 24.04 (Qt 6.4), am
echten Pad (2026-11-08): Erkennung, Static mit Farbe und Helligkeit, Wave, Breathing und Neon mit Geschwindigkeit,
Off.

## Installation

Das Plugin muss zur Qt-Hauptversion des OpenRGB-Builds passen (Qt 6). Das Release wird unter Ubuntu 24.04 (Qt 6.4)
gebaut und lädt auch in neueren Qt-6-Builds wie dem Flatpak.

- **Flatpak** (`org.openrgb.OpenRGB`, hat `--device=all`): `libOpenRGB-KMP6.so` nach
  `~/.var/app/org.openrgb.OpenRGB/config/OpenRGB/plugins/` kopieren und OpenRGB neu starten.
- **Andere Pakete:** nach `~/.config/OpenRGB/plugins/` kopieren. AppImages bringen ihr eigenes Qt mit; lädt das
  Plugin nicht, gegen das Qt deines OpenRGB bauen.

Zugriff auf das Pad: Die udev-Regel von OpenRGB (`60-openrgb.rules`, Holtek Mousemat) erlaubt ihn dem angemeldeten
Benutzer bereits. Ohne sie die Regel dieses Plugins installieren:
```
sudo cp linux/60-openrgb-kmp6.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

## Bauen

```
git clone --recurse-submodules https://github.com/Tom-8192/OpenRGB-KMP6-Plugin
```

Das Submodul `OpenRGB/` ist OpenRGB 1.1; nur seine Header werden benutzt.

```
sudo apt install qt6-base-dev qmake6
mkdir build && cd build && qmake6 ../OpenRGB-KMP6.pro && make
```

Auf einem unveränderlichen System (Bazzite, Silverblue) geht das genauso in einer Distrobox mit Ubuntu 24.04.

Test: `g++ -std=c++17 -Isrc tests/protocol_test.cpp src/KMP6Protocol.cpp -o protocol_test && ./protocol_test`

## Aufbau

| Pfad | Inhalt |
|---|---|
| `src/UsbTransport_Linux.cpp` | sucht das Pad in sysfs, SET_REPORT über usbfs (`USBDEVFS_CONTROL`), Sperre gemeinsam mit dem KM-P6 RGB Tool |
| `src/KMP6Protocol.*` | Einstellungen → Reports (Port von `kmp6-core` `protocol.rs`), Senden mit Pausen und Wiederholungen (`device.rs`) |
| `src/KMP6Plugin.*` | OpenRGB-Plugin: Worker-Thread, virtueller RGB-Controller (Plugin-API 5), Statusseite |
| `tests/protocol_test.cpp` | Tests gegen ein simuliertes Pad |

## Lizenz

GPL-2.0-or-later, wie OpenRGB.
