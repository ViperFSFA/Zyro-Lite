# Zyro-SDK (developer notes)

This is the PC-side half of the Zyro-Lite_TDeck app SDK. If you just want to
write an app, see `src/App.cpp` (copy it) and `sd_card_files/SDK-Readme.md`
(the plain-language guide meant for end users, also shipped on the SD card).
This file is for anyone modifying the SDK itself.

## Layout

```
Zyro-SDK/
  include/
    zyro_sdk_api.h   <- MUST stay byte-identical to ../include/zyro_sdk_api.h
                         on the firmware side. apiVersion is the only thing
                         that guards against a stale copy - it is not a
                         layout-compatibility guarantee by itself.
    zyro_runtime.h    <- declares the tiny helper functions apps can use
                         (zyro_itoa, zyro_utoa, zyro_strcat)
  src/
    zyro_runtime.cpp  <- freestanding libc subset (memcpy/memset/strlen/new)
                         always compiled into every app by pack_zapp.py, so
                         a normal C++ build has zero unresolved symbols
    App.cpp           <- example/template app
  tools/
    zapp.ld           <- linker script; assigns .text/.rodata/.data fixed
                         (fake) link-time addresses purely so every pointer
                         resolves fully at build time
    pack_zapp.py       <- the actual build tool; compiles, links, and packs
                         the result into the ZAPP1 container format
  build.sh / build.bat <- thin wrappers around pack_zapp.py
```

## Why apps aren't compiled on the device

The ESP32-S3 has no C++ compiler or dynamic linker at runtime. So a `.zApp`
is not source code - it's a small pre-linked binary blob (format `ZAPP1`,
defined in `../include/zapp_loader.h`) built here on a PC, which the
firmware's `zapp_loader.cpp` loads into a RAM buffer, patches a handful of
internal-only address references in, and calls into. This is the same
general approach Flipper Zero uses for its `.fap` apps.

Because an app only ever gets the `ZyroApi` function-pointer table (never a
linked reference to `WiFi.h`, `gfx`, `SD.h`, etc.), there is nothing outside
that table for it to call. This is what makes the sandbox real rather than
just a naming convention.

## Keeping `zyro_sdk_api.h` in sync

If you ever change the struct layout in `include/zyro_sdk_api.h` (either
copy), bump `ZYRO_SDK_API_VERSION` in the same file and update both copies
together. A `.zApp` built against a stale copy is rejected by the firmware
at load time (apiVersion mismatch) rather than silently miscalled - but only
because the version number changed. The two copies can still drift apart
undetected if you edit the struct layout without bumping the version, so
treat this as one file that happens to live in two places, not two files.

## Size limits

`pack_zapp.py` reads `ZAPP_TEXT_MAX` and `ZAPP_RODATA_DATA_MAX` directly out
of the firmware's `include/config.h` at build time, so the PC-side limit
check can't drift from what the device enforces again at load time. If you
change those constants in the firmware, the SDK picks it up automatically
next build - no separate constant to update here.

## Requirements

- Python 3.8+
- `pip install pyelftools`
- The firmware's own PlatformIO build must have run at least once, so
  PlatformIO has downloaded the `toolchain-xtensa-esp-elf` package (the SDK
  reuses that exact compiler rather than bundling its own).
