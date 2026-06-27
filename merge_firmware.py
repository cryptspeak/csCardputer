"""Post-build script: merge bootloader + partitions + boot_app0 + firmware
into a single .bin for one-step flashing and web-flasher packaging."""

Import("env")

import os
import shlex


def merge_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")

    # boot_app0.bin lives in the Arduino framework tools
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")

    # Lives alongside the other PlatformIO build outputs (.pio/build/<env>/)
    # instead of the project root, so it doesn't show up as a stray file
    # next to source files.
    output = os.path.join(build_dir, "rscardputer-standalone-factory.bin")

    # Invoke PlatformIO's own bundled esptool.py script directly rather than
    # "python -m esptool" — esptool isn't necessarily installed as an
    # importable module in $PYTHONEXE's environment (e.g. a pipx install of
    # PlatformIO), but PlatformIO always ships this tool package itself.
    python = env.subst("$PYTHONEXE")
    esptool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    esptool_script = os.path.join(esptool_dir, "esptool.py")
    env.Execute(
        f"{shlex.quote(python)} {shlex.quote(esptool_script)} --chip esp32s3 merge_bin "
        "-fm dio -fs 8MB "
        f"-o {shlex.quote(output)} "
        f"0x0000 {shlex.quote(os.path.join(build_dir, 'bootloader.bin'))} "
        f"0x8000 {shlex.quote(os.path.join(build_dir, 'partitions.bin'))} "
        f"0xe000 {shlex.quote(boot_app0)} "
        f"0x10000 {shlex.quote(os.path.join(build_dir, 'firmware.bin'))}"
    )
    print(f"\n** Merged firmware written to: {output}")


env.AddPostAction("$BUILD_DIR/firmware.bin", merge_bin)
