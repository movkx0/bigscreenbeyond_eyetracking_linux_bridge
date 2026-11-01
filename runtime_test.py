"""Exercise the real SteamVR foveation API with short, synthetic OSC input."""
from __future__ import annotations

from datetime import datetime, timezone
import json
import math
from pathlib import Path
import select
import socket
import statistics
import struct
import subprocess
import threading
import time


def osc_string(value: str) -> bytes:
    data = value.encode("ascii") + b"\0"
    return data + b"\0" * (-len(data) % 4)


def message(address: str, *values: float) -> bytes:
    return osc_string(address) + osc_string("," + "f" * len(values)) + struct.pack(">" + "f" * len(values), *values)


def gaze(pitch: float = 0, yaw: float = 0) -> bytes:
    return message("/tracking/eye/LeftRightPitchYaw", pitch, yaw, pitch, yaw)


def bundle(*packets: bytes) -> bytes:
    return b"#bundle\0" + struct.pack(">Q", 1) + b"".join(struct.pack(">I", len(p)) + p for p in packets)


def run(probe: Path, output: Path, port: int = 9020) -> dict:
    baseline = subprocess.run([str(probe), "1"], capture_output=True, text=True, timeout=10, check=True)
    rows = [json.loads(line) for line in baseline.stdout.splitlines() if line.startswith("{")]
    if not rows or not rows[0].get("supports_eye_gaze"):
        raise RuntimeError("SteamVR does not report eye-gaze support. Check that the Beyond gaze add-on loaded.")
    if rows[0].get("scene_pid"):
        raise RuntimeError("Exit the current VR scene/game before injecting synthetic test gaze.")
    if any(row.get("valid") for row in rows):
        raise RuntimeError("A live gaze source is already active. Stop its output before running this synthetic test.")
    if not rows[0].get("hmd", "").startswith("LHR-"):
        raise RuntimeError("The current HMD does not have a Lighthouse serial; refusing synthetic input.")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    session = output / stamp
    session.mkdir(parents=True)
    phases = [
        ("baseline", 0.6, []),
        ("center", 1.0, [gaze()]),
        ("right", 1.0, [gaze(yaw=20)]),
        ("left", 1.0, [gaze(yaw=-20)]),
        ("up", 1.0, [gaze(pitch=-20)]),
        ("down", 1.0, [gaze(pitch=20)]),
        ("mixed_bundle", 1.0, [bundle(message("/tracking/eye/EyesClosedAmount", 0), gaze(),
                                       message("/avatar/parameters/cheekPuffLeft", 1))]),
        ("closed_eyes", 1.8, [bundle(message("/tracking/eye/EyesClosedAmount", 1), gaze(yaw=20))]),
        ("reopened", 1.0, [bundle(message("/tracking/eye/EyesClosedAmount", 0), gaze())]),
        ("malformed", 2.0, [gaze(pitch=math.nan), gaze()[:-1], bundle(gaze(), b"bad!")]),
        ("resumed", 0.8, [gaze(yaw=20)]),
        ("timeout", 2.0, []),
    ]
    samples: list[dict] = []
    windows = {}
    errors = []
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    stderr_path = session / "probe.stderr"
    with stderr_path.open("w") as stderr:
        process = subprocess.Popen([str(probe), "16"], stdout=subprocess.PIPE, stderr=stderr, text=True, bufsize=1)
        assert process.stdout is not None
        if not select.select([process.stdout], [], [], 10)[0]:
            process.kill()
            process.wait()
            raise RuntimeError(f"SteamVR probe did not initialize; see {stderr_path}")
        header = json.loads(process.stdout.readline())
        if header.get("scene_pid") or not header.get("supports_eye_gaze"):
            process.terminate()
            process.wait(timeout=5)
            raise RuntimeError("SteamVR changed since preflight; no test gaze was sent.")
        started = time.monotonic()

        def collect():
            try:
                for line in process.stdout:
                    if line.startswith("{"):
                        samples.append(json.loads(line))
            except (ValueError, OSError) as error:
                errors.append(str(error))

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        try:
            for name, duration, packets in phases:
                begin = time.monotonic() - started
                end = time.monotonic() + duration
                while time.monotonic() < end:
                    for packet in packets:
                        sock.sendto(packet, ("127.0.0.1", port))
                    time.sleep(0.01)
                windows[name] = (begin, time.monotonic() - started)
            process.wait(timeout=10)
        finally:
            sock.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            reader.join(timeout=5)
    (session / "samples.json").write_text(json.dumps({"header": header, "windows": windows, "samples": samples}, indent=2) + "\n")
    if process.returncode or errors:
        raise RuntimeError(f"Runtime probe failed: {errors}; see {session}")

    checks = {}
    means = {}
    for name, _, _ in phases:
        _, end = windows[name]
        tail = [s for s in samples if end - 0.35 < s["time"] < end - 0.05]
        expected_valid = name not in {"baseline", "closed_eyes", "malformed", "timeout"}
        checks[name] = bool(tail) and all(s["valid"] == expected_valid for s in tail)
        if expected_valid and tail:
            means[name] = [statistics.mean((s["left"][i] + s["right"][i]) / 2 for s in tail) for i in (0, 1)]
    if all(name in means for name in ("center", "right", "left", "up", "down")):
        checks["horizontal_direction"] = means["left"][0] + 0.05 < means["center"][0] < means["right"][0] - 0.05
        checks["vertical_direction"] = means["down"][1] + 0.05 < means["center"][1] < means["up"][1] - 0.05
    else:
        checks["horizontal_direction"] = checks["vertical_direction"] = False
    stopped = windows["timeout"][0]
    invalid = [s["time"] for s in samples if s["time"] >= stopped and not s["valid"]]
    result = {"passed": all(checks.values()), "hmd": header["hmd"], "checks": checks,
              "observed_consumer_timeout_seconds": round(min(invalid) - stopped, 3) if invalid else None,
              "mean_foveation_centers": means, "artifacts": str(session)}
    (session / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result
