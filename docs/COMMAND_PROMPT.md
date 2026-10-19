# Command prompt

Open **Command prompt** between Ethernet and Games. Set your nickname or choose **Launch Console**. Nicknames use up to 16 letters, digits, underscores, or hyphens and persist on the SD card. The prompt is `<nickname>@Zyro-Lite`.

Enter submits. Backspace erases; Esc returns to the app menu. `exit` returns to the launcher. Drag the transcript to browse output. A rightward swipe starting at the left edge acts as Back.

USB serial is also supported at 115200 baud: send `console` from the launcher, then enter the same commands. Wi-Fi passwords are masked on screen and omitted from transcripts and serial output. Quote names or passwords containing spaces.

| Command | Behavior |
| --- | --- |
| `help`, `clear`, `exit` | Command help, clear output, or return to the launcher |
| `module list`, `module health` | Enumerate onboard capabilities and report measured/known status; uninitialized hardware is identified |
| `ip me`, `ip v4` | Local station IPv4, gateway, subnet |
| `ip eth` | Existing Ethernet lease; does not switch the USB port into host mode |
| `ip v6` | Assigned global or link-local IPv6, or no address |
| `wifi scan` | Asynchronous scan with SSID, RSSI, channel, and security |
| `wifi connect <ssid> [pass]` | Asynchronous station connection, 15-second timeout; omitted password means open network |
| `wifi status`, `wifi disconnect` | Live station details or disconnect |
| `wifi spect`, `wifi spam` | Open the existing live spectrum/beacon tool; Back restores the console |
| `ble scan`, `ble monitor`, `ble control` | Open existing BLE discovery, manufacturer/beacon monitor, or HID controls |
| `lora ping`, `lora beacon`, `lora monitor` | Initialize SX1262 and send a ping, start periodic beacons, or monitor channel RSSI |
| `gpio list`, `gpio scan` | Pin ownership and read-only levels of configured pins; cannot infer physical connections |
| `gpio read <pin>` | Read a configured pin without changing its mode |
| `gpio bus` | Scan I2C acknowledgement addresses on the shared bus |
| `gpio 3v`, `gpio 5v` | Explain fixed 3.3V supply and external 5V requirements; ESP32 GPIO cannot generate 5V |
| `chip info`, `chip health` | MCU, SDK, memory, uptime, battery estimate, and display-transfer timing |
| `chip restart` | Restart the ESP32 |
| `chip display` | Measure actual presented FPS during a one-second quick-panel drag |
| `chip selftest` | Exercise scroll drag, momentum, bounds, and tap hit testing on the device |
| `chip screen` | USB framebuffer export: `ZYRO_FRAME 320 240` line followed by 153600 little-endian RGB565 bytes |
| `chip display capture` | Benchmark and export the settled quick panel using the same framebuffer format |

Supply rails are not software-selectable voltage modes. External radios require configured wiring. LoRa/RF share the same SX1262, and Ethernet remains experimental. No command fabricates scan results, network replies, or connection detection.

Touch lists follow finger movement in pixels with momentum and boundary clamping; a drag never activates a row. Taps use the actual scrolled hit target. Quick-panel blur and glass surfaces are cached, frames use a 15 ms scheduling budget, and the display uses burst pixel transfers on the existing shared SPI bus. `chip display` measures delivered frames after one-time backdrop preparation rather than assuming the timer target equals panel FPS. The entire cached sheet translates downward from the top, with its header and controls moving together; it does not use a reveal mask. The display transaction runs at 80 MHz; SD and radio retain their own bus settings.

Validation on COM10: console command parsing, password masking, IPv4/IPv6/Ethernet queries, GPIO inspection, asynchronous Wi-Fi scanning, BLE/LoRa tool round-trips, restart, and scroll-physics checks passed. The final whole-sheet slide measured 34.8 presented updates/second; scheduling targets do not override the device transfer and memory limits.
