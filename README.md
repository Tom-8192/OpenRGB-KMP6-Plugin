# OpenRGB KM-P6 plugin

[Deutsch](README.de.md)

An [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) **1.1** plugin for the **Aukey KM-P6** RGB mouse pad
(USB `04D9:A1FA`, Holtek, product string "USB Gaming Keyboard") on **Linux**.

The protocol was worked out for the [KM-P6 RGB Tool](https://github.com/Tom-8192/aukey-km-p6-rgb-control)
(`docs/PROTOCOL.md` there); this plugin uses the same reports.

## Why a plugin

OpenRGB has its own driver for the pad ("Holtek Mousemat"), but it finds the pad through hidapi on USB interface 2.
That interface has **no endpoint**, so Linux' `usbhid` refuses it (`couldn't find an input interrupt endpoint`) and
there is no `/dev/hidraw*` for it - OpenRGB on Linux never sees the pad. The plugin sends the same feature report as
a control transfer through usbfs (`/dev/bus/usb/BBB/DDD`) instead. The keyboard interface 0 keeps its driver.

On **Windows** the plugin is not needed (and not built): OpenRGB's own driver works there.

## Behaviour

Under *Devices → Aukey KM-P6* the pad has one LED (*Mouse pad*; edge and logo always share the colour) and these
modes, in the order the button on the pad steps through them:

| Mode | Settings |
|---|---|
| Static | colour, brightness 0–9 |
| Wave | brightness, speed 0–4, direction (left/right look the same on the pad) |
| Breathing | colour, brightness, speed |
| Neon | brightness, speed |
| Off | – |

Brightness and speed are the levels of the vendor software G-aim. OpenRGB's own driver has a fixed brightness and
no "off"; the pad's "cycle through all colours" is left out on purpose.

- A setting is sent once it stayed unchanged for half a second (dragging the colour picker or a slider sends only
  the end result). Choosing a mode or moving a slider always sends, also when nothing changed, so after the button
  on the pad changed the effect, applying the mode again brings it back.
- Colour updates that do not change the colour (effects, SDK clients repeating it) send nothing.
- There is **no direct mode**: the pad keeps its setting without power, so it presumably writes every setting to
  its flash.
- **Starting OpenRGB sends nothing.** The pad cannot be read back, so the device view shows what the plugin sent
  last (stored in `plugins/settings/KMP6.json` in the OpenRGB configuration directory; factory setting before the
  first change).
- **OpenRGB profiles** keep the setting in the plugin section of the profile; loading a profile sends it (also when
  the pad is plugged in only afterwards).
- Unplugging and replugging is detected; the device disappears and comes back.
- The plugin shares the lock with the KM-P6 RGB Tool (`flock` on the usbfs node; interface 2 is only claimed while a
  setting is sent), so both can run at the same time.
- A status page is under *Settings → KMP6*; messages go to the OpenRGB log with the prefix `[KMP6]`.
- Static and breathing also write G-aim's factory colours into the pad's colour slots 2–7 (the vendor software
  sends them with every static/breathe setting); colours the KM-P6 RGB Tool put there are replaced.

## Tested

Bazzite (Fedora, kernel 7.2), OpenRGB 1.1 Flatpak (KDE runtime 6.10), plugin built on Ubuntu 24.04 (Qt 6.4), on the
real pad (2026-11-08): detection, static with colour and brightness, wave, breathing and neon with speed, off.

## Installation

The plugin must match the OpenRGB build's Qt major version (Qt 6). The release is built on Ubuntu 24.04 (Qt 6.4) and
loads into newer Qt 6 builds such as the Flatpak.

- **Flatpak** (`org.openrgb.OpenRGB`, has `--device=all`): copy `libOpenRGB-KMP6.so` to
  `~/.var/app/org.openrgb.OpenRGB/config/OpenRGB/plugins/` and restart OpenRGB.
- **Other packages:** copy it to `~/.config/OpenRGB/plugins/`. AppImages bring their own Qt; if the plugin does not
  load, build it against your OpenRGB's Qt.

Access to the pad: the udev rule of OpenRGB (`60-openrgb.rules`, Holtek Mousemat) already grants it to the logged-in
user. Without it install the rule of this plugin:
```
sudo cp linux/60-openrgb-kmp6.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

## Building

```
git clone --recurse-submodules https://github.com/Tom-8192/OpenRGB-KMP6-Plugin
```

The submodule `OpenRGB/` is OpenRGB 1.1; only its headers are used.

```
sudo apt install qt6-base-dev qmake6
mkdir build && cd build && qmake6 ../OpenRGB-KMP6.pro && make
```

On an immutable system (Bazzite, Silverblue) the same works inside a distrobox with Ubuntu 24.04.

Test: `g++ -std=c++17 -Isrc tests/protocol_test.cpp src/KMP6Protocol.cpp -o protocol_test && ./protocol_test`

## Layout

| Path | Content |
|---|---|
| `src/UsbTransport_Linux.cpp` | finds the pad in sysfs, SET_REPORT through usbfs (`USBDEVFS_CONTROL`), lock shared with the KM-P6 RGB Tool |
| `src/KMP6Protocol.*` | settings → reports (port of `kmp6-core` `protocol.rs`), sending with pauses and retries (`device.rs`) |
| `src/KMP6Plugin.*` | OpenRGB plugin: worker thread, virtual RGB controller (plugin API 5), status page |
| `tests/protocol_test.cpp` | tests against a simulated pad |

## License

GPL-2.0-or-later, like OpenRGB.
