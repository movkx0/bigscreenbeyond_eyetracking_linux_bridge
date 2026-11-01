# Bigscreen Beyond eye-tracking bridge for Linux

Standalone distribution of the bridge developed in Baballonia's
`tools/beyond-gaze` directory. The setup commands below have been adapted for
this repository.

This SteamVR add-on receives Baballonia's OSC eye gaze on `127.0.0.1:9020` and
publishes it through SteamVR's eye-tracking API for the connected Bigscreen
Beyond. It is a native Linux `.so`; no Proton wrapper is involved.

The bridge attaches `/eyetracking` to the existing Beyond HMD through public
OpenVR driver APIs. It leaves display and Lighthouse pose tracking with their
existing drivers. It does not patch SteamVR, hook driver functions, change
headset calibration, or access the cameras itself.

The cameras and a calibrated gaze model are supplied by **native Baballonia**,
which must stay running while using eye tracking. In Baballonia, enable
**Settings → Use DFR**. Although the setting mentions PimaxMagic4All, its dedicated
OSC output already sends the protocol this bridge consumes. That output is
separate from the OSC target used for VRChat, so both can be used together.

## Build and install

Requirements: a C++17 compiler, CMake, Ninja, Python 3.10+, and native Linux
SteamVR. OpenVR headers are vendored at a pinned revision, with their license.
OpenXR development headers and `pkg-config` are optional, for `xr-probe`.

Clone this repository and run the commands from its root:

```sh
git clone https://github.com/movkx0/bigscreenbeyond_eyetracking_linux_bridge.git
cd bigscreenbeyond_eyetracking_linux_bridge
python3 gaze_bridge.py build
python3 gaze_bridge.py install
```

The `install` command previews the installation. Exit SteamVR, then install:

```sh
python3 gaze_bridge.py install --apply
```

Optionally pin the driver to a particular Beyond with `--serial LHR-xxxxxxxx`.
Otherwise it accepts the active HMD only when its model identifies it as a
Beyond. `--port 9020` and `--timeout-ms 250` override the defaults. Subsequent
installs preserve the existing settings unless an override is supplied.

If you already installed the bridge from another checkout, exit SteamVR and run
`python3 gaze_bridge.py uninstall --apply` from that old bridge directory first.
Then install from this checkout. The installer refuses duplicate `beyond_gaze`
registrations.

Restart SteamVR normally. If SteamVR blocks the add-on after a crash, enable
`beyond_gaze` under **Settings → Startup/Shutdown → Manage Add-ons**, then restart.
The installed driver lives at `.local/driver` beside this script. Keep that
directory in place while it is registered.

Installation backs up the OpenVR registry and any previous bridge binary and
settings under `.local/backups`. It registers only this driver and preserves
other registrations. It refuses to replace its binary while SteamVR is running.
`--steamvr /path/to/SteamVR` before the subcommand overrides runtime discovery.

## Verify the connection

```sh
python3 gaze_bridge.py status
python3 gaze_bridge.py probe --seconds 10
python3 gaze_bridge.py xr-probe
```

`probe` uses `IVRSystem::GetEyeTrackedFoveationCenter`, the API used for native
SteamVR foveated rendering. It prints the HMD serial, advertised gaze capability,
current scene process ID, validity, and per-eye normalized-device-coordinate
foveation centers. With calibrated Baballonia output running, `valid` should
become true and the centers should follow your eyes. They need not be zero when
looking straight ahead: the headset's projections can be asymmetric.

`xr-probe` checks `XR_EXT_eye_gaze_interaction` and its HMD capability flag in
SteamVR's native OpenXR runtime. It selects the SteamVR manifest **only for that
process**, without changing the system's chosen OpenXR runtime. This capability
check does not create a rendering session or validate a particular Proton game.

For a repeatable test without cameras or an eye model, exit VR games and stop
any real gaze output, then run:

```sh
python3 gaze_bridge.py self-test
```

This briefly publishes **synthetic** gaze and reads the actual SteamVR foveation
API. It checks center/left/right/up/down motion, mixed OSC bundles, closed eyes,
malformed data, resumed tracking, and loss of the sender. It refuses to run over
an active VR scene or an already-valid gaze source. Results and samples are kept
under `.local/checks`; the sender always stops when the test finishes.

The driver marks stale input invalid after **250 ms** by default. SteamVR adds
its own delay before returning false from the foveation API: approximately
**1.25 seconds total** was observed on SteamVR 2.18.4. During that delay a client
may still receive the last valid foveation center. Afterward the runtime returns
to its fixed center; the bridge never substitutes synthetic tracking as a live
camera result.

Hardware verification on a Beyond 2e with SteamVR 2.18.4 passed all 14 live
self-test checks. The measured consumer timeout was 1.259 seconds, and the
native OpenXR probe reported both the extension and HMD eye-gaze capability.
These checks used synthetic OSC input; real-eye calibration and in-game
behavior have not yet been validated.

## Games

For **Half-Life: Alyx**, use its native Linux build with SteamVR and enable
**Options → Performance → Advanced Performance → Foveated Rendering**. The
October 26, 2026 PC update added the game's foveation option. Fixed foveation can
work without eye tracking; eye-driven foveation requires valid gaze from this
bridge. See the [Alyx foveation guide](https://steamcommunity.com/sharedfiles/filedetails/?id=3822057317).

OpenXR games also need to implement gaze or foveated rendering, or use an
appropriate rendering layer. Advertising gaze support alone does not make a
game render with foveation. DCS still needs its Quad-Views configuration; Alien:
Rogue Incursion and Iron Rebellion have not been validated with this bridge.

## Protocol and behavior

- Receive UDP only on IPv4 loopback, default port `9020`.
- Accept `/tracking/eye/LeftRightPitchYaw` with exactly four OSC floats: left
  pitch, left yaw, right pitch, right yaw, all in degrees.
- Average and normalize the two eye directions. Positive OSC pitch looks down;
  OpenVR uses +Y up, +X right, and forward along -Z. The result is a combined
  unit gaze ray at the head origin, following the upstream driver's convention.
- Optionally accept `/tracking/eye/EyesClosedAmount`; values at least `0.95`
  temporarily invalidate gaze while that blink sample is fresh.
- Accept immediate OSC bundles, including nested bundles up to four levels.
  Future/timestamped bundles are rejected rather than published prematurely.
- Ignore other OSC expression addresses without resetting or extending gaze
  validity. Reject malformed eye messages, non-finite angles, angles outside
  ±90°, degenerate combined rays, and oversized packets.
- Process at most 64 datagrams per SteamVR frame, with a 4096-byte packet limit.
- Refuse to replace eye tracking already advertised by another driver.

No camera image, gaze history, or network request is stored or sent by the
driver. Diagnostic tests store only their synthetic sample data. Its log entries
in Steam's `logs/vrserver.txt` report attachment, stream transitions, and counts.

## Remove or disable

The SteamVR add-on toggle can disable the bridge. To unregister it, exit SteamVR:

```sh
python3 gaze_bridge.py uninstall --apply
```

Restart SteamVR afterward. Removal retains the build and backups, and removes
only this registration instead of restoring an old copy of the entire registry.

## Development checks and provenance

```sh
python3 -m unittest discover -s . -p 'test_*.py' -q
ctest --test-dir .local/build --output-on-failure
```

The native tests cover OSC framing, atomic rejection of malformed bundles, gaze
axes, binocular averaging, blinking, invalid input, and stale-data handling.
Python tests cover preview/install/remove behavior, backups, preserving other
drivers, configuration validation, and refusing updates while SteamVR runs.
`tests/fuzz_decode.cpp` is a libFuzzer entry point for ASan/UBSan parser checks.

The OpenVR SDK revision and license are recorded in `vendor/openvr`. The OSC
angle convention and component API follow
[Project Babble's VrcOsc-EyeTracker-SteamVR](https://github.com/Project-Babble/VrcOsc-EyeTracker-SteamVR)
and [mbucchia's driver example](https://github.com/mbucchia/Pimax-EyeTracker-SteamVR).
The upstream MIT notice is retained under `vendor/VrcOsc-EyeTracker-SteamVR`
and copied into the installed driver alongside the OpenVR license.

## License

This standalone distribution retains the Babble Software Distribution License
1.0 from the original Baballonia workspace; see [LICENSE](LICENSE). Vendored
OpenVR headers and the upstream driver reference retain their separate license
notices under `vendor/`.
