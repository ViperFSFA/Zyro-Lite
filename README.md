# Project Status

> **Note:** This section reflects the current state of the project. Please keep it up to date as development progresses. Only include information that is accurate.

> **Repository Policy**
>
> This repository is **public for viewing purposes**, but development is maintained exclusively by the **Zyro-Lite development team**.
>
> * Only members of the Zyro-Lite team may modify the project's source code.
> * Pull requests from external contributors will **not** be accepted.
> * Issues and bug reports are welcome if they provide useful information.
> * Please do not fork this project with the intention of submitting changes back to this repository.

## Current Feature Status (Zyro-Lite V1.0)

| Feature                    |       Status       | Notes                                                                                                                    |
| -------------------------- | :----------------: | ------------------------------------------------------------------------------------------------------------------------ |
| Wi-Fi                      |    ✅ Functional    | AP scanning, connect/disconnect, spectrum view, a signal monitor that reads the actual connected network, and a real (promiscuous-mode, not simulated) packet monitor.                                                 |
| BLE (Bluetooth Low Energy)  |    ✅ Functional    | The scan crash (caused by fully reinitialising the BLE stack on every scan) is fixed. The stack now comes up once per session and scans asynchronously, with safe teardown on exit.                                    |
| RF (Sub-GHz)                |    ✅ Functional    | Sub-GHz sweep, 433MHz scope, scan & capture (with saved captures), FSK mode (RX/TX/monitor/BER), and radio info/diagnostics. Note: replay is not supported on this hardware (SX1262 is not a raw sub-GHz OOK/ASK chip). |
| LoRa                        |    ✅ Functional    | Receiver monitor, ping sender, and a broadcast text chat (username + selectable frequency, keyboard input). Not wire-compatible with real Meshtastic devices. different packet format, no encryption. it only talks to other devices running this firmware. |
| GPS                          |    ✅ Functional    | Status/fix screen, coordinate tracker (speed/distance), heading/compass, and a track logger that writes CSV to SD.        |
| Ethernet                    |  🧪 Experimental   | USB-C CDC-ECM adapter status and a network ping test. Marked experimental in the app itself. Hasn't had a full hardware test pass.                                                                                      |
| Games                        |    ✅ Functional    | Pong and Snake are complete. No additional games are planned for v1.0.                                                   |
| Apps                          |   ⚠️ In Progress   | Good progress overall. The File Browser still requires improvements.                                                     |
| Settings                      |    ✅ Functional    | Settings are stored using the ESP32 Preferences (NVS).                                                                    |

---

# Development Guidelines

These guidelines apply to the Zyro-Lite development team:

* Prioritise fixing existing issues before adding new functionality.
* Avoid large architectural or UI changes unless they have been discussed and approved.
* Preserve the current layout and design unless a redesign has been agreed upon.
* Do **not** add additional games for v1.0. The existing games primarily serve as graphics demonstrations. Larger additions (such as Doom) may be considered in future releases.

---

# Planning Future Versions

New versions are determined collaboratively by the Zyro-Lite development team.

A new version may be released when:

* Planned features for the current milestone are complete.
* Important bug fixes are ready.
* Major improvements justify a new release.

Feature priorities and release scope are discussed and agreed upon by the team before implementation.

- www.viperfsfa.com
