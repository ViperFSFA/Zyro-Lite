Import("env")
import os

toolchain_base = os.path.join(
    os.path.expanduser("~"),
    ".platformio", "packages", "toolchain-xtensa-esp-elf", "xtensa-esp-elf"
)

# 1. Add toolchain bin to PATH so compiler/linker executables are found
toolchain_bin = os.path.join(toolchain_base, "bin")
if os.path.isdir(toolchain_bin):
    current_path = env["ENV"].get("PATH", "")
    if toolchain_bin not in current_path:
        env["ENV"]["PATH"] = toolchain_bin + os.pathsep + current_path
        print(f"[fix_toolchain_path] Prepended {toolchain_bin} to build PATH")

# 2. Force GCC multilib driver to select the esp32s3 multilib during linking
env.Append(LINKFLAGS=["-mdynconfig=xtensa_esp32s3"])
print("[fix_toolchain_path] Appended -mdynconfig=xtensa_esp32s3 to LINKFLAGS")
