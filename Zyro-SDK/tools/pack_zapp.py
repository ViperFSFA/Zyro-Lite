"""
Zyro-SDK packer, turns your App.cpp (plain C++, using only zyro_sdk_api.h)
into a AppName.zApp binary the Zyro-Lite_TDeck firmware can load.

Usage:
    python pack_zapp.py MyApp.cpp
    python pack_zapp.py MyApp.cpp Helper.cpp OtherFile.cpp   (multi-file app)
    python pack_zapp.py MyApp.cpp -o build/MyApp.zApp
    python pack_zapp.py MyApp.cpp --name CoolApp

What it does, in order:
  1. Finds the ESP32-S3 Xtensa toolchain.
  2. Compiles your source(s) + the SDK's tiny freestanding runtime
     (zyro_runtime.cpp) to .o files. -ffreestanding/-nostdlib: no libc, no
     Arduino, nothing but what zyro_sdk_api.h declares plus the handful of
     helpers zyro_runtime.cpp supplies (memcpy/new/etc).
  3. Links everything with tools/zapp.ld, a linker script that assigns
     .text/.rodata/.data fixed-but-fake addresses purely so every internal
     pointer can be fully resolved at link time.
  4. Reads the linked ELF (via pyelftools) and turns every absolute-address
     relocation left in it into a ZappReloc entry. the tiny table the
     firmware re-applies wherever it actually put your app's sections in
     RAM at load time. This is the whole trick that lets a normal C++
     build run as a relocatable blob on a device with no dynamic linker.
  5. Packs header + relocations + code + data into <AppName>.zApp.

If the C++/link step fails, you'll see the compiler's own error. fix your
source and re-run. If pack_zapp.py itself errors out (e.g. "app too big",
"relocation out of range"), that's a genuine SDK/size-limit problem, not a
typo in your code.

Requires: Python 3.8+, `pip install pyelftools`, and the same PlatformIO
espressif32 platform install the firmware itself uses (for the toolchain).
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import glob

try:
    from elftools.elf.elffile import ELFFile
except ImportError:
    print("ERROR: pyelftools is required. Install it with:\n"
          "    pip install pyelftools", file=sys.stderr)
    sys.exit(1)

SDK_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(SDK_DIR, "tools")
SDK_INCLUDE = os.path.join(SDK_DIR, "include")
SDK_SRC = os.path.join(SDK_DIR, "src")
FW_ROOT = os.path.dirname(SDK_DIR)  # Zyro-Lite_TDeck/
FW_CONFIG_H = os.path.join(FW_ROOT, "include", "config.h")
LINKER_SCRIPT = os.path.join(TOOLS_DIR, "zapp.ld")

ZAPP_MAGIC = 0x31504100
ZYRO_SDK_API_VERSION = 1  # must track include/zyro_sdk_api.h's #define

TEXT_BASE = 0x00000000
RODATA_BASE = 0x00100000
DATA_BASE = 0x00200000
REGION_LEN = 0x00100000  # matches zapp.ld. just needs to exceed any real app

R_XTENSA_32 = 1  # the only relocation type that represents a load-time-
                 # relevant absolute pointer. everything else (PC-relative
                 # slot ops, diff/alignment relocs) is already fully baked
                 # into the instruction bits by the link and needs no
                 # further action at load time.

SECTION_NAMES = {"TEXT": 0, "RODATA": 1, "DATA": 2}


# Helpers
def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def read_fw_limit(macro_name, fallback):
    """Pulls ZAPP_TEXT_MAX / ZAPP_RODATA_DATA_MAX straight from the
    firmware's own config.h, so these limits can never silently drift out
    of sync between the SDK and the device that enforces them again at
    load time."""
    if not os.path.isfile(FW_CONFIG_H):
        print(f"  (warning: could not find {FW_CONFIG_H}, using built-in "
              f"default for {macro_name})")
        return fallback
    text = open(FW_CONFIG_H, "r", encoding="utf-8", errors="ignore").read()
    m = re.search(rf"#define\s+{macro_name}\s+\(?([^)\n]+)\)?", text)
    if not m:
        print(f"  (warning: {macro_name} not found in config.h, using default)")
        return fallback
    expr = m.group(1).strip()
    try:
        # Handles things like "48 * 1024" safely (no arbitrary eval of
        # unrelated text - only digits/whitespace/* survive the regex).
        if not re.fullmatch(r"[0-9\s*]+", expr):
            raise ValueError(expr)
        return eval(expr, {"__builtins__": {}}, {})
    except Exception:
        print(f"  (warning: could not parse {macro_name}={expr!r}, using default)")
        return fallback


def find_toolchain():
    """Locates the xtensa-esp32s3-elf-g++ compiler. Same toolchain the
    firmware's own PlatformIO build uses (see extra_scripts/fix_toolchain_path.py),
    so app builds always match the firmware's ABI/ISA exactly."""
    env_override = os.environ.get("ZYRO_TOOLCHAIN")
    candidates = []
    if env_override:
        candidates.append(env_override)

    home = os.path.expanduser("~")
    candidates.append(os.path.join(home, ".platformio", "packages",
                                    "toolchain-xtensa-esp-elf", "xtensa-esp-elf", "bin"))
    # Older PlatformIO installs used a per-target toolchain package name.
    candidates.append(os.path.join(home, ".platformio", "packages",
                                    "toolchain-xtensa-esp32s3", "bin"))

    for bin_dir in candidates:
        gpp = os.path.join(bin_dir, "xtensa-esp32s3-elf-g++")
        gpp_exe = gpp + ".exe"
        if os.path.isfile(gpp) or os.path.isfile(gpp_exe):
            return bin_dir

    # Fall back to whatever's already on PATH.
    which = shutil.which("xtensa-esp32s3-elf-g++")
    if which:
        return os.path.dirname(which)

    die(
        "Could not find the xtensa-esp32s3-elf-g++ toolchain.\n"
        "  This SDK reuses the same compiler PlatformIO installs for the\n"
        "  firmware build. Make sure you've built Zyro-Lite_TDeck at least\n"
        "  once (so PlatformIO has downloaded 'toolchain-xtensa-esp-elf'),\n"
        "  or set the ZYRO_TOOLCHAIN environment variable to the toolchain's\n"
        "  bin/ directory."
    )


def tool(bin_dir, name):
    exe = os.path.join(bin_dir, name)
    if os.name == "nt" and os.path.isfile(exe + ".exe"):
        return exe + ".exe"
    return exe


# Compile
def compile_sources(bin_dir, sources, include_dirs, build_dir):
    gpp = tool(bin_dir, "xtensa-esp32s3-elf-g++")
    objs = []
    for src in sources:
        obj = os.path.join(build_dir, os.path.splitext(os.path.basename(src))[0] + ".o")
        cmd = [
            gpp, "-c", src, "-o", obj,
            "-Os", "-std=gnu++17",
            "-ffreestanding", "-fno-exceptions", "-fno-rtti",
            "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
            "-fno-threadsafe-statics", "-fno-use-cxa-atexit",
            "-mlongcalls", "-mtext-section-literals",
            "-Wall", "-Wextra",
        ]
        for inc in include_dirs:
            cmd += ["-I", inc]
        print("  " + " ".join(os.path.basename(c) if c == gpp else c for c in cmd[:3]) + " ...")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            die(f"compilation failed for {os.path.basename(src)}")
        objs.append(obj)
    return objs


def link_objects(bin_dir, objs, build_dir, app_name):
    gpp = tool(bin_dir, "xtensa-esp32s3-elf-g++")
    elf_path = os.path.join(build_dir, app_name + ".elf")
    map_path = os.path.join(build_dir, app_name + ".map")
    cmd = [
        gpp, "-o", elf_path,
        "-nostdlib", "-nostartfiles",
        "-Wl,-T," + LINKER_SCRIPT,
        "-Wl,--emit-relocs",
        "-Wl,--gc-sections",
        "-Wl,-Map=" + map_path,
        "-Wl,--no-undefined",
    ] + objs
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        die(
            "link failed. If you see 'undefined reference', your app is\n"
            "  calling something outside zyro_sdk_api.h / zyro_runtime.h -\n"
            "  a .zApp must be fully self-contained (no libc, no Arduino,\n"
            "  no external libraries)."
        )
    return elf_path


# elf > .zApp
def classify_address(value):
    if TEXT_BASE <= value < TEXT_BASE + REGION_LEN:
        return SECTION_NAMES["TEXT"], value - TEXT_BASE
    if RODATA_BASE <= value < RODATA_BASE + REGION_LEN:
        return SECTION_NAMES["RODATA"], value - RODATA_BASE
    if DATA_BASE <= value < DATA_BASE + REGION_LEN:
        return SECTION_NAMES["DATA"], value - DATA_BASE
    return None, None


def extract(elf_path, text_max, rodata_data_max):
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)

        sections = {}
        for name in (".text", ".rodata", ".data", ".bss"):
            sec = elf.get_section_by_name(name)
            sections[name] = sec

        if sections[".text"] is None:
            die("no .text section produced - did you define zapp_entry()?")

        text_sec = sections[".text"]
        text_base = text_sec["sh_addr"]
        text_size = text_sec["sh_size"]
        text_bytes = text_sec.data()

        def sec_bytes_and_size(sec):
            if sec is None:
                return b"", 0
            return sec.data(), sec["sh_size"]

        rodata_bytes, rodata_size = sec_bytes_and_size(sections[".rodata"])
        data_bytes, data_size = sec_bytes_and_size(sections[".data"])
        bss_sec = sections[".bss"]
        bss_size = bss_sec["sh_size"] if bss_sec is not None else 0

        rodata_base = sections[".rodata"]["sh_addr"] if sections[".rodata"] else RODATA_BASE
        data_base = sections[".data"]["sh_addr"] if sections[".data"] else DATA_BASE

        if text_base != TEXT_BASE:
            die(f"linker placed .text at unexpected address 0x{text_base:x} "
                f"(expected 0x{TEXT_BASE:x}) - is tools/zapp.ld being used?")

        # entry point
        symtab = elf.get_section_by_name(".symtab")
        if symtab is None:
            die("no symbol table in linked ELF (unexpected)")
        entry_sym = None
        for sym in symtab.iter_symbols():
            if sym.name == "zapp_entry":
                entry_sym = sym
                break
        if entry_sym is None:
            die("zapp_entry() not found. Every app must define:\n"
                "    extern \"C\" ZyroAppModule zapp_entry(const ZyroApi *api)")
        if entry_sym["st_shndx"] == "SHN_UNDEF":
            die("zapp_entry is declared but never defined")
        entry_addr = entry_sym["st_value"]
        entry_offset = entry_addr - TEXT_BASE

        # relocations
        section_bytes = {
            SECTION_NAMES["TEXT"]: bytearray(text_bytes),
            SECTION_NAMES["RODATA"]: bytearray(rodata_bytes),
            SECTION_NAMES["DATA"]: bytearray(data_bytes),
        }
        section_base = {
            SECTION_NAMES["TEXT"]: text_base,
            SECTION_NAMES["RODATA"]: rodata_base,
            SECTION_NAMES["DATA"]: data_base,
        }

        relocs = []
        for rela_name, target_section in ((".rela.text", SECTION_NAMES["TEXT"]),
                                           (".rela.rodata", SECTION_NAMES["RODATA"]),
                                           (".rela.data", SECTION_NAMES["DATA"])):
            rela_sec = elf.get_section_by_name(rela_name)
            if rela_sec is None:
                continue
            for reloc in rela_sec.iter_relocations():
                rtype = reloc["r_info_type"]
                if rtype != R_XTENSA_32:
                    continue  # PC-relative / linker-relaxation-only entries
                r_offset = reloc["r_offset"]
                offset_in_target = r_offset - section_base[target_section]
                buf = section_bytes[target_section]
                if offset_in_target < 0 or offset_in_target + 4 > len(buf):
                    die(f"relocation offset out of range in {rela_name} "
                        f"(offset {offset_in_target}, section size {len(buf)})")
                resolved_value = struct.unpack_from("<I", buf, offset_in_target)[0]
                ref_section, addend = classify_address(resolved_value)
                if ref_section is None:
                    die(
                        f"a pointer in your app resolves to address 0x{resolved_value:x},\n"
                        "  which isn't inside .text/.rodata/.data. This usually means\n"
                        "  your app references something outside itself (a hardware\n"
                        "  address, an external symbol) that slipped past the linker."
                    )
                relocs.append((target_section, ref_section, offset_in_target, addend))

    # size limits
    if text_size == 0:
        die("empty .text section")
    if text_size > text_max:
        die(f".text is {text_size} bytes, limit is {text_max} bytes "
            f"(ZAPP_TEXT_MAX in firmware config.h) - trim your app.")
    combined = rodata_size + data_size + bss_size
    if combined > rodata_data_max:
        die(f".rodata+.data+.bss is {combined} bytes, limit is {rodata_data_max} bytes "
            f"(ZAPP_RODATA_DATA_MAX in firmware config.h) - trim constants/globals.")

    return {
        "text": bytes(section_bytes[SECTION_NAMES["TEXT"]]),
        "rodata": bytes(section_bytes[SECTION_NAMES["RODATA"]]),
        "data": bytes(section_bytes[SECTION_NAMES["DATA"]]),
        "bss_size": bss_size,
        "entry_offset": entry_offset,
        "relocs": relocs,
    }


def write_zapp(out_path, built):
    text, rodata, data = built["text"], built["rodata"], built["data"]
    relocs = built["relocs"]

    header = struct.pack(
        "<8I",
        ZAPP_MAGIC,
        ZYRO_SDK_API_VERSION,
        len(text),
        len(rodata),
        len(data),
        built["bss_size"],
        built["entry_offset"],
        len(relocs),
    )

    reloc_bytes = b"".join(
        struct.pack("<BBHIi", target, ref, 0, offset, addend)
        for (target, ref, offset, addend) in relocs
    )

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(reloc_bytes)
        f.write(text)
        f.write(rodata)
        f.write(data)


# main
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="+", help="your app's .cpp file(s)")
    ap.add_argument("-o", "--out", help="output .zApp path (default: build/<Name>/<Name>.zApp)")
    ap.add_argument("--name", help="app name override (default: first source file's stem)")
    args = ap.parse_args()

    sources = [os.path.abspath(s) for s in args.sources]
    for s in sources:
        if not os.path.isfile(s):
            die(f"source file not found: {s}")

    app_name = args.name or os.path.splitext(os.path.basename(sources[0]))[0]
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,24}", app_name):
        die(f"app name {app_name!r} must be 1-24 chars, letters/digits/_/- only "
            f"(this becomes the folder+filename the firmware uses)")

    build_dir = os.path.join(os.getcwd(), "build", app_name)
    os.makedirs(build_dir, exist_ok=True)

    out_path = args.out or os.path.join(build_dir, app_name + ".zApp")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)

    text_max = read_fw_limit("ZAPP_TEXT_MAX", 48 * 1024)
    rodata_data_max = read_fw_limit("ZAPP_RODATA_DATA_MAX", 64 * 1024)

    print(f"Zyro-SDK: building {app_name}")
    bin_dir = find_toolchain()
    print(f"  toolchain: {bin_dir}")

    runtime_src = os.path.join(SDK_SRC, "zyro_runtime.cpp")
    all_sources = sources + [runtime_src]
    include_dirs = [SDK_INCLUDE] + list({os.path.dirname(s) for s in sources})

    print("  compiling...")
    objs = compile_sources(bin_dir, all_sources, include_dirs, build_dir)

    print("  linking...")
    elf_path = link_objects(bin_dir, objs, build_dir, app_name)

    print("  extracting sections + relocations...")
    built = extract(elf_path, text_max, rodata_data_max)

    write_zapp(out_path, built)

    print()
    print(f"Done: {out_path}")
    print(f"  .text   {len(built['text']):>6} bytes   (limit {text_max})")
    print(f"  .rodata {len(built['rodata']):>6} bytes")
    print(f"  .data   {len(built['data']):>6} bytes")
    print(f"  .bss    {built['bss_size']:>6} bytes")
    print(f"  combined rodata+data+bss: "
          f"{len(built['rodata']) + len(built['data']) + built['bss_size']} "
          f"bytes (limit {rodata_data_max})")
    print(f"  relocations: {len(built['relocs'])}")
    print()
    print(f"Copy {app_name}.zApp to the SD card, then on the device:")
    print(f"  Apps > My Apps > Install from SD > select {app_name}.zApp")


if __name__ == "__main__":
    main()
