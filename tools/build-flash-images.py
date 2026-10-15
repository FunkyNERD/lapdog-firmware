#!/usr/bin/env python3
"""Builds the release's flash images for every device in devices.json.

For each device it builds the PlatformIO env, then writes to dist/:

  <id>-firmware.bin(.sha256)  the app image (over-the-air updates)
  <id>-factory.bin(.sha256)   everything a first install needs, merged into one image flashed
                              at 0x0: bootloader, partition table and the app
  flash.json                  the devices, their chips and their factory images, for the
                              LapDog website's browser flasher

Offsets come from PlatformIO (project metadata): the bootloader is at 0x0 on the C3 but 0x2000
on the C5, so a device with a different chip or layout needs nothing here, just its entry in
devices.json. (Ported from lapdog-lcd's; this one has no filesystem image or boot_app0, since
the firmware is ESP-IDF.)

  python tools/build-flash-images.py 1.2.3     # FIRMWARE_VERSION to compile in
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
PIO = shutil.which("pio") or os.path.expanduser("~/.platformio/penv/bin/pio")


def run(*args, **kw):
    print("+", " ".join(args), flush=True)
    return subprocess.run(args, cwd=ROOT, check=True, **kw)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def write_sha(path):
    digest = sha256(path)
    with open(path + ".sha256", "w") as f:
        f.write(f"{digest}  {os.path.basename(path)}\n")
    return digest


def build(device, version):
    env, dev = device["env"], device["id"]
    flags = f'-DFIRMWARE_VERSION=\\"{version}\\"' if version else ""
    pio_env = dict(os.environ, PLATFORMIO_BUILD_FLAGS=flags)
    # Build first: on a fresh machine (or CI without its cache) that installs the platform, which
    # a metadata run would otherwise do itself, printing its progress over the JSON. Metadata
    # gets the same flags - a run with different ones clears the build.
    run(PIO, "run", "-e", env, env=pio_env)
    out = run(PIO, "project", "metadata", "-e", env, "--json-output",
              capture_output=True, text=True, env=pio_env).stdout
    json_line = next((ln for ln in reversed(out.splitlines()) if ln.startswith("{")), None)
    if json_line is None:
        sys.exit(f"pio project metadata gave no JSON for {env}:\n{out}")
    meta = json.loads(json_line)[env]["extra"]
    build_dir = os.path.join(ROOT, ".pio", "build", env)

    app = os.path.join(DIST, f"{dev}-firmware.bin")
    shutil.copyfile(os.path.join(build_dir, "firmware.bin"), app)
    if version:
        with open(app, "rb") as f:
            if f"v{version}".encode() not in f.read():  # the boot banner, "LapDog timer v1.2.3"
                sys.exit(f"FIRMWARE_VERSION {version} not found in {dev}'s build")

    parts = [(int(img["offset"], 16), img["path"]) for img in meta["flash_images"]]
    parts.append((int(meta["application_offset"], 16), app))
    factory = os.path.join(DIST, f"{dev}-factory.bin")
    args = ["--chip", device["chip"], "merge_bin", "-o", factory,
            "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"]
    for offset, path in sorted(parts):
        args += [hex(offset), path]
    # A pip-installed esptool if there is one (CI); PlatformIO's copy needs PlatformIO's own
    # Python environment for its dependencies.
    esptool = shutil.which("esptool")
    if esptool:
        run(esptool, *args)
    else:
        run(PIO, "pkg", "exec", "--package", "tool-esptoolpy", "--", "esptool.py", *args)

    write_sha(app)
    return {
        "id": dev,
        "name": device["name"],
        "chipFamily": device["chipFamily"],
        "description": device.get("description", ""),
        "instructions": device.get("instructions", []),
        "factory": os.path.basename(factory),
        "size": os.path.getsize(factory),
        "sha256": write_sha(factory),
    }


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else ""
    with open(os.path.join(ROOT, "devices.json")) as f:
        devices = json.load(f)
    os.makedirs(DIST, exist_ok=True)
    built = [build(d, version) for d in devices]
    with open(os.path.join(DIST, "flash.json"), "w") as f:
        json.dump({"version": version, "devices": built}, f, indent=2)
        f.write("\n")
    print(json.dumps(built, indent=2))


if __name__ == "__main__":
    main()
