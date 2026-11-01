#!/usr/bin/env python3
"""Build, install, inspect, and test the native Linux Beyond gaze driver."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import runtime_test

ROOT = Path(__file__).resolve().parent
LOCAL = ROOT / ".local"
BUILD = LOCAL / "build"
INSTALLED = LOCAL / "driver"
REGISTRY = Path.home() / ".config/openvr/openvrpaths.vrpath"


def running_server(proc: Path = Path("/proc")) -> list[int]:
    found = []
    for path in proc.iterdir():
        if not path.name.isdigit():
            continue
        try:
            if ((path / "comm").read_text().strip() == "vrserver" and
                    (path / "stat").read_text().split(") ", 1)[1].split()[0] not in {"Z", "X"}):
                found.append(int(path.name))
        except (OSError, IndexError):
            pass
    return found


def registry() -> dict:
    return json.loads(REGISTRY.read_text())


def steamvr(explicit: Path | None) -> Path:
    paths = [explicit] if explicit else [Path(p) for p in registry().get("runtime", [])]
    for path in paths:
        if path and (path / "bin/linux64/vrpathreg").is_file():
            return path.resolve()
    raise RuntimeError("Linux SteamVR was not found; supply --steamvr /path/to/SteamVR.")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def atomic_write(path: Path, data: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as file:
            file.write(data)
            file.flush()
            os.fsync(file.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def build(runtime: Path) -> None:
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DSTEAMVR_PATH={runtime}"], check=True)
    subprocess.run(["cmake", "--build", str(BUILD)], check=True)
    subprocess.run(["ctest", "--test-dir", str(BUILD), "--output-on-failure"], check=True)


def configuration(serial: str | None, port: int | None, timeout: int | None) -> dict:
    settings = INSTALLED / "resources/settings/default.vrsettings"
    if not settings.is_file():
        settings = ROOT / "driver/resources/settings/default.vrsettings"
    content = json.loads(settings.read_text())
    values = content["driver_beyond_gaze"]
    if serial is not None:
        values["serial"] = serial.upper()
    if port is not None:
        values["port"] = port
    if timeout is not None:
        values["timeout_ms"] = timeout
    if values["serial"] and not re.fullmatch(r"LHR-[0-9A-F]{8}", values["serial"]):
        raise RuntimeError("Serial must be a full Lighthouse serial, LHR-xxxxxxxx.")
    if not 1024 <= values["port"] <= 65535 or not 50 <= values["timeout_ms"] <= 1000:
        raise RuntimeError("Port must be 1024..65535 and timeout 50..1000 milliseconds.")
    return content


def install(runtime: Path, apply: bool, serial: str | None, port: int | None, timeout: int | None) -> dict:
    source = BUILD / "driver"
    library = source / "bin/linux64/driver_beyond_gaze.so"
    if not library.is_file() or library.read_bytes()[:5] != b"\x7fELF\x02":
        raise RuntimeError("Build the Linux driver first: gaze_bridge.py build")
    settings = configuration(serial, port, timeout)
    before = registry()
    for driver in before.get("external_drivers", []):
        path = Path(driver)
        manifest = path / "driver.vrdrivermanifest"
        if path.resolve() != INSTALLED and manifest.is_file():
            if json.loads(manifest.read_text()).get("name") == "beyond_gaze":
                raise RuntimeError(f"Another beyond_gaze driver is registered at {path}. Remove that registration first.")
    result = {"installed": False, "directory": str(INSTALLED), "sha256": sha(library),
              "settings": settings["driver_beyond_gaze"]}
    if not apply:
        return result
    if running_server():
        raise RuntimeError("Exit SteamVR before installing or updating its native gaze driver.")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    backup = LOCAL / "backups" / stamp
    backup.mkdir(parents=True)
    shutil.copy2(REGISTRY, backup / "openvrpaths.vrpath")
    previous = INSTALLED / "resources/settings/default.vrsettings"
    if previous.is_file():
        shutil.copy2(previous, backup / "default.vrsettings")
    previous_library = INSTALLED / "bin/linux64/driver_beyond_gaze.so"
    if previous_library.is_file():
        shutil.copy2(previous_library, backup / "driver_beyond_gaze.so")
    for path in source.rglob("*"):
        if path.is_file() and path.name != "default.vrsettings":
            atomic_write(INSTALLED / path.relative_to(source), path.read_bytes(), 0o755 if path.suffix == ".so" else 0o644)
    atomic_write(previous, (json.dumps(settings, indent=2) + "\n").encode())
    subprocess.run([str(runtime / "bin/linux64/vrpathreg"), "adddriver", str(INSTALLED)], check=True)
    if str(INSTALLED) not in registry().get("external_drivers", []):
        raise RuntimeError(f"Driver registration was not saved. Registry backup: {backup}")
    if sha(previous_library) != result["sha256"]:
        raise RuntimeError(f"Installed driver did not match the build. Backup: {backup}")
    result.update(installed=True, backup=str(backup), sdk=json.loads((ROOT / "vendor/openvr/source.json").read_text()))
    atomic_write(LOCAL / "installation.json", (json.dumps(result, indent=2) + "\n").encode())
    return result


def uninstall(runtime: Path, apply: bool) -> dict:
    if not apply:
        return {"removed": False, "directory": str(INSTALLED), "note": "--apply unregisters this driver; files and backups remain."}
    if running_server():
        raise RuntimeError("Exit SteamVR before removing its native gaze driver.")
    subprocess.run([str(runtime / "bin/linux64/vrpathreg"), "removedriver", str(INSTALLED)], check=True)
    return {"removed": str(INSTALLED) not in registry().get("external_drivers", []), "directory": str(INSTALLED)}


def status() -> dict:
    settings = INSTALLED / "resources/settings/default.vrsettings"
    library = INSTALLED / "bin/linux64/driver_beyond_gaze.so"
    return {"registered": str(INSTALLED) in registry().get("external_drivers", []),
            "server_pids": running_server(), "directory": str(INSTALLED),
            "driver_sha256": sha(library) if library.is_file() else None,
            "settings": json.loads(settings.read_text())["driver_beyond_gaze"] if settings.is_file() else None}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--steamvr", type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("build", help="Compile the native driver and probe, and run unit tests")
    command = commands.add_parser("install", help="Preview driver installation; --apply writes it and registers it")
    command.add_argument("--apply", action="store_true")
    command.add_argument("--serial", help="Restrict to this Beyond serial; default preserves the installed setting")
    command.add_argument("--port", type=int)
    command.add_argument("--timeout-ms", type=int)
    command = commands.add_parser("uninstall", help="Unregister this driver while retaining files and backups")
    command.add_argument("--apply", action="store_true")
    commands.add_parser("status", help="Read installation and configuration status")
    command = commands.add_parser("probe", help="Read SteamVR's actual eye-tracked foveation centers")
    command.add_argument("--seconds", type=int, default=2, choices=range(1, 61), metavar="1..60")
    commands.add_parser("xr-probe", help="Check eye-gaze support in SteamVR's native OpenXR runtime")
    commands.add_parser("self-test", help="Publish synthetic gaze for a runtime test; exit VR games and stop real gaze first")
    args = parser.parse_args()
    try:
        runtime = steamvr(args.steamvr)
        if args.command == "build":
            build(runtime)
            return 0
        if args.command == "install":
            result = install(runtime, args.apply, args.serial, args.port, args.timeout_ms)
        elif args.command == "uninstall":
            result = uninstall(runtime, args.apply)
        elif args.command == "status":
            result = status()
        else:
            if not running_server():
                raise RuntimeError("Start SteamVR first. The diagnostic will not auto-start it.")
            if args.command == "xr-probe":
                probe = BUILD / "gaze_xr_probe"
                if not probe.is_file():
                    raise RuntimeError("Rebuild with OpenXR development headers and pkg-config installed.")
                env = dict(os.environ)
                env["XR_RUNTIME_JSON"] = str(runtime / "steamxr_linux64.json")
                return subprocess.run([str(probe)], env=env, check=False).returncode
            probe = BUILD / "gaze_probe"
            if not probe.is_file():
                raise RuntimeError("Build the diagnostic first: gaze_bridge.py build")
            if args.command == "probe":
                return subprocess.run([str(probe), str(args.seconds)], check=False).returncode
            installed = status()
            if not installed["registered"] or not installed["settings"]:
                raise RuntimeError("The Beyond gaze driver is not installed and registered.")
            result = runtime_test.run(probe, LOCAL / "checks", installed["settings"]["port"])
            print(json.dumps(result, indent=2))
            return 0 if result["passed"] else 1
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"beyond-gaze: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
