# Zyro-SDK. Write your own apps for this device

You write your app in C++ on a PC, build it into a `.zApp` file with the
free Zyro-SDK tool, then copy that one file onto this SD card. The device
never compiles anything itself. It just runs the finished `.zApp`.

## Quick start

1. On your PC, get the `Zyro-SDK` folder (it comes with the firmware
   project. Ask whoever set up your device, or find it alongside the
   firmware source).
2. Copy `Zyro-SDK/src/App.cpp` somewhere and rename it, e.g. `MyApp.cpp`.
   Open it and edit `appTick()` / `appHandleInput()` to build your app.
3. Build it:
   - **Windows:** `Zyro-SDK\build.bat MyApp.cpp`
   - **Mac/Linux:** `Zyro-SDK/build.sh MyApp.cpp`

   (First time only: `pip install pyelftools`.)

4. This produces `build/MyApp/MyApp.zApp`. Copy that single file anywhere
   onto this SD card (the root folder is fine).
5. On the device: **Apps > My Apps > Install from SD**, pick your
   `.zApp` file. It now shows up in **Apps > My Apps** under your app's
    name. Select it to run it.

That's it. No cables, no re-flashing the firmware, nothing to install on
the device itself beyond your own app.

## What your app can do

Your app only talks to the device through a fixed set of functions handed
to it when it starts (see `Zyro-SDK/include/zyro_sdk_api.h` for the exact
list, and `Zyro-SDK/src/App.cpp` for a working example that uses most of
them):

- **Screen**. Draw rectangles, lines, pixels, and text in your own
  drawing area below the top status bar. You can't draw over the top bar,
  and there's nothing in the API that would let you.
- **Files**. Read/write files, but only inside your own app's private
  storage folder on this card. Your app can't see or touch any other
  app's files, or the device's own files.
- **Wi-Fi**. Scan for networks, connect, disconnect.
- **Bluetooth (BLE)**. Scan for nearby devices, advertise your own name.
- **LoRa**. Send/receive, if this unit has a LoRa radio built in.
- **GPIO**. Read/write a small set of general-purpose pins (the same
  pins the built-in GPIO app can use. Nothing wired to the screen,
  radios, or SD card is ever exposed).
- **Battery/clock/logging**. Read battery %, elapsed time, and print
  debug messages (visible over USB serial only, never on-screen).

## What your app can't do

This is by design, not a bug to work around:

- It **cannot** modify the firmware, the top bar, or any other app.
- It **cannot** read or write outside its own private data folder.
- It **cannot** call anything other than what's listed above. There is
  no way to reach the display driver, SD card library, or radios
  directly, so there's nothing to accidentally (or deliberately) break.
- If it crashes, freezes, or does something it shouldn't, the device
  shows: **"Error, Review your app on another device"** and returns you
  to the launcher. It does not take down the firmware, and if it
  happens to lock up hard enough to reset the whole device, the same
   message appears once at the next boot too. The app gets flagged
  so you have to confirm before running it again.

## Size limits

Each app has a maximum code size and a maximum combined size for its
constants/variables. If your build fails with a "too big" message, trim
your app down. This can't be worked around, it protects the memory the
rest of the firmware needs.

## Managing installed apps

From **Apps > My Apps** you can also remove an installed app, or
un-block one that got flagged after a crash (you'll be asked to confirm
before it runs again).

## Troubleshooting

- **Build fails with a compiler error**. That's your C++ code; the
  error message points at the line.
- **Build fails with "undefined reference"**. Your app is calling
  something outside `zyro_sdk_api.h`. Apps can't link against Arduino,
  WiFi.h, or any other library directly. Only the functions in that one
  header.
- **"Could not find the xtensa-esp32s3-elf-g++ toolchain"**. Build the
  firmware itself once first with PlatformIO, so it downloads the
  compiler the SDK reuses.
- **App installs but immediately shows the error screen**. Check your
  `init()` isn't doing something that fails immediately (e.g. assuming a
  file already exists). Test on a second device if you're not sure
  whether it's your app or something environmental.
