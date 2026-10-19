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

## V1.3 local development update

- Context-aware top bar, stable estimated battery level, and consistent diagnostic surfaces.
- **Command prompt**, between Ethernet and Games, uses the supplied 28-frame animated icon. It includes a persistent nickname, device details, a scrollable terminal, live chip/network commands, and tool shortcuts. See [command reference](docs/COMMAND_PROMPT.md).
- Settings uses the launcher card width, spacing, and eased selection movement. Labels and values no longer collide.
- Touch defaults to on; toggle it in **Settings > Personalisation > Touch**. Existing SD configurations remain compatible.
- Tap menu rows directly; drag lists to scroll with momentum. Swipe right from the left edge to go back. Deep diagnostic views accept tap-to-OK. Wi-Fi/BLE lists, file lists, chat setup, and GPIO rows have direct targets; games use directional tap zones. Typing still uses the physical keyboard. SDK apps receive the existing navigation/OK events.
- Swipe down from the top bar for the frosted Control Center: Wi-Fi, sound, keyboard lighting, power saving, and brightness. The complete panel slides downward from the top together; swipe up on its handle or press Back to dismiss it. Blur and card surfaces are cached.
- Wi-Fi/BLE discovery cards show identity, signal, and security/manufacturer details. GPS has separate position, satellite, accuracy, altitude, and speed cards.
- Correct pulse-controlled display brightness, boot-log setting restoration, settings-value validation, SD-return status, scrolling for long installed-app/GPIO lists, and menu animation cleanup.
- Battery readings use trimmed ADC samples, smoothing, hysteresis, and a nonlinear single-cell Li-ion curve. `~` marks an estimate. This board has no verified software-accessible charger-status signal: voltage trends no longer trigger a fabricated charging animation. Charge state remains unavailable; exact capacity requires calibration/a fuel gauge.
- Ethernet remains experimental; link state is never counted as an ICMP reply.

Validation on 19 October 2026: PlatformIO build passed, COM10 flash hash verified, and startup checks passed for the main loop, GT911 detection, NVS, and battery ADC initialization. On-device menu/gesture appearance still needs hands-on checking.

Hardware reference: [LilyGO T-Deck UnitTest](https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/examples/UnitTest/UnitTest.ino) (GT911 orientation and backlight pulse protocol).

## Current Feature Status (Zyro-Lite V1.3)

| Feature                    |       Status       | Notes                                                                                                                    |
| -------------------------- | :----------------: | ------------------------------------------------------------------------------------------------------------------------ |
| Wi-Fi                      |    ✅ Functional    | AP scanning, connect/disconnect, spectrum view, a signal monitor that reads the actual connected network, and a real (promiscuous-mode, not simulated) packet monitor.                                                 |
| BLE (Bluetooth Low Energy)  |    ✅ Functional    | The scan crash (caused by fully reinitialising the BLE stack on every scan) is fixed. The stack now comes up once per session and scans asynchronously, with safe teardown on exit.                                    |
| RF (Sub-GHz)                |    ✅ Functional    | Multi-band RSSI sweep, selectable live scope, IRQ-driven LoRa packet capture with inspectable saved captures, selectable 433/868MHz FSK tools (RX/TX/monitor/sequence-aware BER), and 433/868MHz waterfall scans. Replay is not supported because the SX1262 is not a raw OOK/ASK transceiver. |
| LoRa                        |    ✅ Functional    | Receiver monitor, ping sender, and a broadcast text chat (username + selectable frequency, keyboard input). Not wire-compatible with real Meshtastic devices. different packet format, no encryption. it only talks to other devices running this firmware. |
| GPS                          |    ✅ Functional    | Status/fix screen, coordinate tracker (speed/distance), heading/compass, and a track logger that writes CSV to SD.        |
| Ethernet                    |  🧪 Experimental   | USB-C CDC-ECM adapter status and a network ping test. Marked experimental in the app itself. Hasn't had a full hardware test pass.                                                                                      |
| Games                        |    ✅ Functional    | Pong and Snake are complete. No additional games are planned for v1.0.                                                   |
| Apps                          |   ⚠️ In Progress   | Good progress overall. The File Browser still requires improvements.                                                     |
| Settings                      |    ✅ Functional    | Settings are stored in `/zyro.conf` on the SD card; touch and boot-log preferences persist.                                                                    |

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
