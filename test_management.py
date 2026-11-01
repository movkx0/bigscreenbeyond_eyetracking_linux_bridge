import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import gaze_bridge as bridge


class ManagementTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name, value in {"ROOT": self.root, "LOCAL": self.root / ".local",
                            "BUILD": self.root / ".local/build", "INSTALLED": self.root / ".local/driver",
                            "REGISTRY": self.root / "openvrpaths.vrpath"}.items():
            mocked = patch.object(bridge, name, value)
            mocked.start(); self.addCleanup(mocked.stop)
        self.runtime = self.root / "SteamVR"
        self.original = {"runtime": [str(self.runtime)], "external_drivers": ["/another/driver"], "version": 1}
        bridge.REGISTRY.write_text(json.dumps(self.original))
        source = bridge.BUILD / "driver"
        library = source / "bin/linux64/driver_beyond_gaze.so"
        library.parent.mkdir(parents=True)
        library.write_bytes(b"\x7fELF\x02test-library")
        (source / "driver.vrdrivermanifest").write_text('{"name":"beyond_gaze"}')
        defaults = bridge.ROOT / "driver/resources/settings/default.vrsettings"
        defaults.parent.mkdir(parents=True)
        defaults.write_text(json.dumps({"driver_beyond_gaze": {"serial": "", "port": 9020, "timeout_ms": 250}}))
        sdk = bridge.ROOT / "vendor/openvr/source.json"
        sdk.parent.mkdir(parents=True)
        sdk.write_text('{"commit":"test-sdk"}')
        mocked = patch.object(bridge, "running_server", return_value=[])
        self.running = mocked.start(); self.addCleanup(mocked.stop)

    def command(self, args, **kwargs):
        contents = bridge.registry()
        if args[1] == "adddriver": contents["external_drivers"].append(args[2])
        elif args[1] == "removedriver": contents["external_drivers"].remove(args[2])
        else: self.fail(f"Unexpected command: {args}")
        bridge.REGISTRY.write_text(json.dumps(contents))

    def install(self):
        with patch.object(bridge.subprocess, "run", side_effect=self.command):
            return bridge.install(self.runtime, True, "LHR-12345678", None, None)

    def test_preview_never_registers_or_writes_installation(self):
        before = bridge.REGISTRY.read_bytes()
        with patch.object(bridge.subprocess, "run") as run:
            result = bridge.install(self.runtime, False, "LHR-12345678", None, None)
        self.assertFalse(result["installed"])
        self.assertFalse(bridge.INSTALLED.exists())
        self.assertEqual(bridge.REGISTRY.read_bytes(), before)
        run.assert_not_called()

    def test_install_preserves_other_drivers_and_saves_backup(self):
        result = self.install()
        self.assertTrue(result["installed"])
        self.assertEqual(bridge.registry()["external_drivers"], ["/another/driver", str(bridge.INSTALLED)])
        self.assertEqual(json.loads((Path(result["backup"]) / "openvrpaths.vrpath").read_text()), self.original)
        self.assertEqual(bridge.status()["settings"]["serial"], "LHR-12345678")
        self.assertEqual(bridge.status()["driver_sha256"], result["sha256"])

    def test_update_preserves_serial_filter(self):
        self.install()
        values = bridge.configuration(None, 9022, None)["driver_beyond_gaze"]
        self.assertEqual(values, {"serial": "LHR-12345678", "port": 9022, "timeout_ms": 250})

    def test_running_steamvr_prevents_install_and_uninstall(self):
        self.running.return_value = [123]
        with self.assertRaises(RuntimeError): self.install()
        with self.assertRaises(RuntimeError): bridge.uninstall(self.runtime, True)
        self.assertEqual(bridge.registry(), self.original)
        self.assertFalse(bridge.INSTALLED.exists())

    def test_uninstall_preserves_new_unrelated_registration(self):
        self.install()
        contents = bridge.registry()
        contents["external_drivers"].append("/added/later")
        bridge.REGISTRY.write_text(json.dumps(contents))
        with patch.object(bridge.subprocess, "run", side_effect=self.command):
            result = bridge.uninstall(self.runtime, True)
        self.assertTrue(result["removed"])
        self.assertEqual(bridge.registry()["external_drivers"], ["/another/driver", "/added/later"])
        self.assertTrue(bridge.INSTALLED.exists())

    def test_invalid_configuration_rejected_before_changes(self):
        for serial, port, timeout in [("other-device", None, None), (None, 0, None), (None, None, 60000)]:
            with self.subTest(serial=serial, port=port, timeout=timeout), self.assertRaises(RuntimeError):
                bridge.install(self.runtime, True, serial, port, timeout)
        self.assertFalse(bridge.INSTALLED.exists())
        self.assertEqual(bridge.registry(), self.original)

    def test_duplicate_driver_name_is_rejected(self):
        other = self.root / "other-copy"
        other.mkdir()
        (other / "driver.vrdrivermanifest").write_text('{"name":"beyond_gaze"}')
        contents = bridge.registry(); contents["external_drivers"].append(str(other))
        bridge.REGISTRY.write_text(json.dumps(contents))
        with self.assertRaises(RuntimeError): self.install()
        self.assertFalse(bridge.INSTALLED.exists())


if __name__ == "__main__":
    unittest.main()
