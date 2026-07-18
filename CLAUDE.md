# SmartSpin2k Firmware — Session Context

## Project Overview

ESP32 firmware for a DIY device that motorizes the resistance knob on any spin bike, turning it into a smart trainer compatible with Zwift, TrainerRoad, and other cycling apps. Supports BLE (FTMS, Zwift, OpenBikeControl services), ERG mode, power calibration, and Peloton serial integration.

**Companion app repo:** `ddavis528-personal/ss2kconfigapp`
**Current version:** `26.7.18` (latest released); `[Unreleased]` runaway/homing fixes pending.
**Build system:** PlatformIO (Arduino framework for ESP32)
**Primary branch:** `develop`

---

## Repository Structure

```
src/
  Main.cpp            # Entry point + core loop; FTMSModeShiftModifier(), handleShiftButtons()  ← recent work
  BLE_ss2kCustomCharacteristic.cpp  # Custom BLE characteristic encode/decode
  ERG_Mode.cpp        # ERG power control
  settings.cpp        # NVS-backed settings persistence
  ...
include/
  Main.h
  BLE_ss2kCustomCharacteristic.h
  settings.h
  ...
lib/                  # Third-party libraries
data/                 # LittleFS assets (web UI, config pages)
test/                 # Unit tests
platformio.ini        # Build config
CHANGELOG.md
```

---

## Key Architecture

### Gear shift flow (`src/Main.cpp`)
1. **Physical buttons** → `handleShiftButtons()` → `rtConfig->setShifterPosition(pos)`.
2. **App write** → BLE custom characteristic `onWrite` → `bytes_to_u16` decode → `setShifterPosition(pos)`.
3. On every tick: `FTMSModeShiftModifier()` detects delta between last and current `shifterPosition`, applies bounds, moves stepper, then calls `BLE_ss2kCustomCharacteristic::notify(BLE_shifterPosition)` to send confirmation back to app.

### `FTMSModeShiftModifier()` gating
- Gated by `spinDownFlag == 0`. Values: `0` = normal, `1` = startup re-verify homing, `2` = full homing pending.
- When `spinDownFlag != 0`, gear writes are queued but not executed until the flag clears (homing completes on first pedal stroke).

### hMin / hMax sentinels
- **Before homing:** `hMin = hMax = INT32_MIN` (-2147483648). The device has not yet found its home position.
- **After homing:** `minStep = 0`, `maxStep = hMax`. Gear range is `0` to `(hMax - hMin) / shiftStep`.
- The app uses `hMax <= hMin` to detect "not yet homed" and shows `?` for max gear.

### Custom BLE characteristic
- Uses **indicate** (acknowledged), not plain notify.
- Packet format for responses: `{0x80, ref, LSB, MSB}` (little-endian u16 value).
- App library (Flutter Blue Plus): `setNotifyValue(true)` enables both notify and indicate; `onValueReceived` receives both.

---

## Recent Changes (as of 2026-07-18)

### Runaway-resistance root cause fixed (deep review round)
- **FTMS SIM grade sign bug (`BLE_Fitness_Machine_Service.cpp`)**: `SetIndoorBikeSimulationParameters` decoded the signed grade with `bytes_to_u16`, so every Zwift downhill (-1% = 0xFF9C) became +65436 → ~+458k-step target → knob slammed to max resistance. Now decoded signed (`bytes_to_s16`) and clamped to ±4000 (±40%). This was the real "downhill runaway" previously blamed on ERG_GUARDRAILS.
- **`bytes_to_s16` macro fixed** (`BLE_Common.h`): old macro sign-extended the low byte, corrupting values with LSB ≥ 0x80.
- **Signed decodes** for `shifterPosition` and `targetIncline` BLE writes (`BLE_Custom_Characteristic.cpp`): -1 no longer becomes 65535.
- **Sticky homing failure**: `BLE_Client.cpp` only clears `spinDownFlag` when `goHome()` succeeded (failed homing retries after ~2 s of cadence); `_findFTMSHome()` sweep timeouts/aborts now propagate instead of finalizing as homed with garbage limits.

### `src/Main.cpp` — virtual shifter bug fixes (26.7.18)
- **Off-by-one in `FTMSModeShiftModifier()`**: Used `getShifterPosition() + shiftDelta` for `nextGearPos`; replaced with `lastShifterPosition + shiftDelta` to avoid double-counting a mid-flight update.
- **Gear floor before homing**: Clamped gear to `0` minimum even when `hMin = INT32_MIN`, so the motor can't be driven below the physical zero stop before calibration.

---

## Known Issues / Next Steps

### Potential improvements
- **Gear ceiling from app**: The app now floors gear writes at 0 but does not yet cap at `maxGear`. The firmware enforces the ceiling, so this is cosmetic — but a symmetric app-side cap would give cleaner UX.
- **`spinDownFlag` UX**: There is no in-app indication that homing is pending (`spinDownFlag != 0`). Gear changes are silently queued. A status field or BLE notification when homing completes would improve UX.
- **ERG mode guardrails**: The `ERG_GUARDRAILS` feature was removed in v26.7.10 after causing stepper runaway on downhills. If re-added, it needs to operate on the PID output rather than the raw position counter.

### CI / Build
- GitHub Actions: PlatformIO builds on every push to `develop`; releases are tagged and published automatically.
- Check run status via the Actions tab on `ddavis528-personal/smartspin2k`.

---

## Development Workflow

```bash
# Clone and build
git clone https://github.com/ddavis528-personal/smartspin2k
cd smartspin2k
pio run                    # Build firmware
pio run -t upload          # Flash to device
pio test                   # Run unit tests

# Push to trigger CI build
git push origin develop
```

**Branch convention:** Development happens on `develop`; releases are tagged from `develop`.

---

## App ↔ Firmware Interaction Reference

| App constant | Firmware vName | Type | Notes |
|---|---|---|---|
| `shifterPositionVname` | `shifterPosition` | int | Current gear (0-based) |
| `shiftStepVname` | `shiftStep` | int | Stepper steps per gear |
| `BLE_hMinVname` | `hMin` | long | INT32_MIN before homing |
| `BLE_hMaxVname` | `hMax` | long | INT32_MIN before homing |
| `fwVname` | `firmwareVersion` | string | Read-only version string |
| `connectedPWRVname` | `connectedPWR` | bool | Power meter connected flag |
