# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- **Manual calibration never appeared.** The fallback was entered correctly after three failed automatic runs — the retry loop stopped and the device waited for input, exactly as designed — but `getCalibrationState()` did not report the manual steps, so it kept returning `CALIBRATION_IDLE`. The app showed an ordinary uncalibrated gear display with the trim controls greyed out, and nothing indicated the device was waiting for the rider. The manual states are now reported, and checked ahead of `isHoming` so the confirmation sweep shows its own step rather than a generic "calibrating". `tools/check_calibration_states.py` (run via pre-commit, which CI already executes) now fails the build if any calibration state is defined but unreachable from `getCalibrationState()`.
- **Builds are now reproducible.** Three dependencies (`esp-nimble-cpp`, `FastAccelStepper`, `ArduinoWebsockets`) tracked a git branch rather than a fixed version, so every build silently took whatever was on that branch. All three are now pinned to the exact commits the last known-good build used, alongside the already-pinned TMCStepper, ArduinoJson and platform. `esp-nimble-cpp` is pinned back to `8cefc0a` (2.3.1) — the version every green build through 2026-08-01 actually compiled against. To move to a newer upstream version, bump the hash deliberately and let CI prove it builds.
- **A calibration could complete successfully and still leave no upshift ceiling.** Travel limits were trusted if *either* hMin or hMax was set, and the missing one was then passed to `setMinStep()`/`setMaxStep()`, which translate the `INT32_MIN` "unset" sentinel into the ±200,000,000 defaults. A device holding a valid hMin but no hMax therefore logged "using configured travel limits" while having a 200-million-step ceiling — i.e. none. This is why a bike whose calibrated range is ~21 gears could be shifted to gear 31 and beyond right after a calibration that ran and finished normally. Three sources are fixed: `setStepperMinMax()` now requires *both* limits before trusting either; a one-directional (startup re-verify) homing run no longer declares the device homed when no maximum is known, and instead requests a full both-directions calibration on the next pedal stroke; and the power-table upload path no longer writes `hMin = 0` on its own, which is what manufactured the half-configured state.
- **Upshifting had no stopping point on an uncalibrated device.** Travel limits default to ±200,000,000 steps, which is not a limit at all, and nothing narrowed them until `PowerTable::setStepperMinMax()` ran from the ERG loop — which is gated on `spinDownFlag`, so the entire "waiting for the first pedal stroke" window ran unbounded. The bike could be shifted well past its physical stop (gear 31 and climbing on a bike whose calibrated range was ~21 gears). Three fixes: stored hMin/hMax are now applied at boot instead of sitting unused; a device that has never calibrated gets a provisional range of `UNCALIBRATED_MAX_GEARS` (24) gears instead of the ±200M defaults; and the "device reports resistance feedback" path in `setStepperMinMax()` no longer hands out the ±200M defaults either. A real calibration still replaces all of these with measured limits.
- **Reported resistance was wrong whenever travel was uncalibrated.** `calculateResistanceFromPosition()` computed `(position - minPos) * 100` in 32-bit arithmetic; with the ±200M default limits that product is ~2×10¹⁰, which overflowed and wrapped negative, so the resistance broadcast to Zwift and shown in the app was pinned at 0 and non-monotonic rather than tracking the knob. The intermediate is now 64-bit.
- **False coupler-slip warnings on uncalibrated devices.** The shift-response monitor classifies a flat power response by *where* in the travel it happened, but an uncalibrated device only has provisional limits, under which nearly every position counts as "mid travel" — the region that means slip. A device that simply had not been calibrated could therefore report a mechanical fault. The monitor now waits for real travel limits before drawing any conclusion.
- **Resistance runaway on Zwift downhills**: the FTMS `SetIndoorBikeSimulationParameters` handler decoded the signed sint16 grade field with an unsigned macro, so any negative grade (e.g. -1.00% = `0xFF9C`) became a huge positive incline (+65436). `moveStepper()` multiplied that by `inclineMultiplier`, commanding ~+458,000 steps and driving the knob to maximum resistance for the entire descent (and effectively without limit whenever homing had not completed, since default travel limits are ±200M steps). The grade is now decoded signed and clamped to ±40%. This was very likely the true cause of the "downhill runaway" previously attributed to `ERG_GUARDRAILS` (removed in 26.7.10) — grade is not even used in ERG mode.
- `bytes_to_s16` macro sign-extended the *low* byte, corrupting any decode whose LSB was ≥ 0x80 (e.g. `0x019C` = 412 decoded as -100). Rewritten to assemble both bytes unsigned and reinterpret the 16-bit result as signed.
- `shifterPosition` BLE write decoded unsigned: a `-1` gear write from an app became gear 65535, which before homing commanded a ~78-million-step move toward max resistance. The recent app-side floor only masked this for up-to-date apps; the firmware now decodes the gear as signed so its own floor/bounds logic applies.
- `targetIncline` BLE custom-characteristic write decoded unsigned, with the same negative-becomes-huge failure mode.
- Homing failure no longer unlocks unprotected riding: the BLE client task cleared `spinDownFlag` even when `goHome()` failed, re-enabling ERG and shift handling with the default ±200M travel limits. The flag is now cleared only when homing succeeded; a failed attempt retries after ~2 s of continued cadence. The post-homing "middle gear" preset is likewise applied only on success.
- FTMS homing sweeps (`_findFTMSHome`) treated a timeout or user abort as success: the sweep lambda's `return` only exited the lambda, after which the function zeroed the position counter and set `homed = true` with garbage travel limits. Sweep failures now propagate and abort the run with `homed = false` (falling back to StallGuard homing where supported), and any previous homing result is invalidated for the duration of the run.
- **Travel limits sat exactly on the physical stops when calibrating via FTMS resistance feedback.** `goHome()`'s StallGuard path always reserved one gear of margin at each end (backing off before zeroing the minimum, and subtracting a shift step from the maximum), but `_findFTMSHome()` did neither — it zeroed the counter at the min-resistance point and set `hMax` to the exact max-resistance position. ERG could therefore drive the knob right into a hard stop, which is consistent with the brief grinding and coupler slip observed at the ends of travel during ERG workouts. The FTMS path now reserves the same one-gear margin at both ends. Note this does not address counter-vs-physical drift *after* a slip has already occurred; detecting that is tracked separately with the power-curve work.
- **`minWatts` is now honored as a software end stop.** `PowerTable::setStepperMinMax()` returned early whenever hMin/hMax were configured, so the `minWatts`-derived minimum position below it was unreachable once the device had been homed — the setting had no effect on a calibrated device. The effective minimum is now `max(hMin, position that produces minWatts @ 90 rpm)` whenever the power table has learned that position, keeping the knob off the low hard stop. Until the table has data the mechanical limit stands, and the floor is capped at 1/3 of the calibrated travel so a bad table entry can never collapse the usable gear range. **Consequence to expect:** the lowest gears become unreachable once the floor is learned — shifts below it are blocked rather than silently ignored.

### Added
- **Manual calibration fallback.** Automatic homing depends on either the bike reporting its resistance accurately or the motor stalling consistently at the stop; when neither holds it can fail indefinitely, and retrying a failed algorithm does not make it work. After `MANUAL_CALIBRATION_AFTER_FAILURES` (3) consecutive failed runs the device stops retrying and hands calibration to the rider: shift to the lowest resistance and confirm, shift to the highest and confirm, then a confirmation sweep runs between the two. The range is stored either way — a sweep that falls short raises a warning rather than discarding the only travel measurement we have. New states 6–9 on characteristic `0x2F`, driven by new write-only characteristic `0x33`. Manual calibration can also be started on demand. While the rider is positioning the knob, travel opens to ±`MANUAL_CAL_WINDOW_GEARS` (40) gears and the pre-homing gear floor is lifted, because reaching the real stops is exactly what is being asked for.
- **Cadence correction factor** (new setting, characteristic `0x32`, default 1.0, range 0.5–2.0) for bikes whose cadence sensor reads consistently low or high, which makes app workout RPM targets unreachable. The multiplier is applied at sensor ingestion, so one setting corrects everything downstream consistently: the cadence broadcast to Zwift, ERG's minimum-cadence gate, the homing trigger, the power table's cadence keys, and the shift-response monitor. Changing it materially downgrades the power table's confidence, because the table is keyed by cadence and existing rows were recorded against the old scale. Note this rescales what the bike *reports* — it corrects an inaccurate sensor, it does not make you faster.
- **The power curve now trains from ordinary riding.** Previously the high-end scale factor (K) only moved when ERG sat against the configured hard stop for 25 continuous seconds — a corner case most rides never hit, which is why training appeared not to happen. A new shift-response monitor watches what happens to power after the knob moves: at a steady cadence, moving the knob must change power, and where it fails to tells us something different. Near the **top** of travel it means the pad is saturated, so K is bumped (three corroborating moves required). Near the **bottom** it means a low-end dead zone, which is recorded and logged. In **mid travel** it means the counter moved but the knob didn't — a coupler slip — so it is flagged rather than treated as a model correction. Each move must be gear-sized, follow a 4-second settle, and hold cadence within 5 rpm, and a single flat move never acts on its own: riders change effort constantly. The monitor is deliberately inert in ERG mode, where the controller moves the knob specifically to hold power constant and flat power is the goal rather than a fault.
- **Suspected coupler slip is surfaced** as calibration state `5` on characteristic `0x2F`, with a log line explaining that recalibration is recommended. Nothing is recalibrated automatically — a surprise homing sweep mid-ride is worse than a stale counter — and a successful calibration clears the suspicion. Apps that don't know state 5 treat it as "not busy" and behave exactly as before.
- **Learned power scale factor (K) is readable** on new read-only characteristic `0x31` (value × 10), so the app can show whether the high-end power model is actually training.
- **Calibration status is now reported to the app** on a new read-only custom characteristic (`0x2F`), with every transition notified: idle / pending (waiting for you to pedal) / active / retrying / aborted. The value is derived from the existing homing flags rather than stored separately, so it cannot drift out of sync with the state that actually gates the control loop. Apps that predate this characteristic are unaffected, and a new app talking to old firmware simply sees "idle".
- **Hold either shifter button for 5 seconds to abort calibration.** Previously a single button press only cancelled the current sweep and the retry loop immediately started another, so there was no way to stop a calibration that was misbehaving. The hold sets an abort latch that the homing sweeps poll and that stops the retry loop. On abort the previous travel limits are restored when they are known (so the device stays fully usable); if the device has never been calibrated, travel is fenced to ±4 gears around the current position so an uncalibrated motor still cannot run away.

### Changed
- **Split `rtConfig->targetIncline` into two variables.** It previously held three incompatible quantities depending on mode — the SIM-mode grade (0.01% units, multiplied by `inclineMultiplier`), the ERG PID's absolute stepper position, and a magic `1.0f` — so any unexpected mode flip misinterpreted a stale value by a factor of thousands. `targetIncline` now holds only the SIM grade; a new `controlTargetPosition` carries the absolute position commanded by ERG and resistance modes (including homing sweeps and the `ERG_GUARDRAILS` block).
- `ErgMode::_userIsSpinning()` is now a pure predicate: the cadence-dropout reaction (switch to SIM at near-flat grade, reset PID integral) moved into `computeErg()` where it is visible, instead of being a hidden side effect of a boolean check.
- `BLE_simulatedTargetWatts` (0x28) writes now set the ERG *target* watts; a copy-paste from the `simulatedWatts` case made them overwrite the measured power value instead.
- Deduplicated the "bike reports real resistance" check and the hMin/hMax travel-limit fallback into `BLE_Fitness_Machine_Service::hasResistanceReporting()` / `getEffectiveTravelLimits()`; removed the dead resistance-to-position TODO block from the `SetTargetResistanceLevel` handler.
- ERG saturation K-bump no longer calls `saveToLittleFS()` from the control path; it defers to the maintenance loop's `saveFlag` handler.
- `resetIfShiftersHeld()` formats LittleFS once instead of 20 times in a loop (~8 s faster, less flash wear).
- Startup homing now parks at the middle of the *calibrated* gear range (`(hMax − hMin) / shiftStep / 2`) instead of hardcoded gear 8, which could land near max resistance on short-travel bikes.

### Hardware

## [26.7.18]

### Fixed
- Virtual shifter gear display going negative in simulation mode: the shift-blocker used the composite `targetPosition` (gear × shiftStep + incline × inclineMultiplier) as its bounds reference. On an uphill in Zwift, the incline term kept the composite position well above minStep even at gear 0, so the downshift blocker never fired and repeated downshifts drove the gear into negative values. The check now uses only the pure gear component (`nextGear × shiftStep`) so the floor at gear 0 (minStep) is correctly enforced regardless of the current Zwift grade.
- Fixed off-by-one in the gear-position blocker: `nextGearPos` was computed as `(currentGear + shiftDelta) × shiftStep`, which double-counts the delta because `currentGear` already reflects the shift applied by `handleShiftButtons()`. Changed to `(lastShifterPosition + shiftDelta) × shiftStep` so the check evaluates exactly the proposed new gear's position; this also fixes valid shifts to the exact minStep/maxStep boundary being incorrectly blocked.
- Downshifting below gear 0 before homing now clamped to gear 0 in `handleShiftButtons()`. While `spinDownFlag != 0`, `FTMSModeShiftModifier()` is gated and cannot enforce gear limits; with the default `minStep = -200,000,000` the motor was commanded 1200 steps in the decreasing direction (physically toward max resistance) on the very next maintenance-loop tick. The pre-homing floor prevents the motor from moving in an uncalibrated direction before travel limits are established.

### Added

### Changed

### Hardware


## [26.7.10]

### Fixed
- ERG stepper runaway on downhill (or any segment where motor coupling slips): `ERG_GUARDRAILS` set `targetIncline = currentPosition + 1` whenever the stepper was above the ERG-computed position and watts were still below target. Once the physical coupling broke, the position counter climbed freely, watts could never respond, and the guardrail chased the counter upward indefinitely — completely bypassing the `maxStep` clamp that ERG applies to `newIncline`. The guardrail has been disabled; the ERG PID clamp (`newIncline` bounded to `[minStep, maxStep]`) and time-based saturation detection are sufficient and do not create runaway.

## [26.7.9]

### Fixed
- Calibration broken after moveStepper() was moved outside the spinDownFlag gate: goHome() runs in the BLE client task and calls stepper->move/moveTo directly; the main loop simultaneously calling moveStepper() fought it for stepper control. Added isHoming flag set at the start of goHome() (cleared on all exit paths) so moveStepper() yields during the homing sequence while still running freely when only waiting for homing to be triggered.
- Motor frozen after firmware flash: `moveStepper()` was gated inside `if (!spinDownFlag)`, so any time homing was pending (spinDownFlag=1 on startup with known limits, or spinDownFlag=2 waiting for first pedal stroke) the motor would not respond to app or shifter commands at all. `moveStepper()` now always runs; only ERG mode and automatic mode changes remain gated on spinDownFlag.
- ERG stepper runaway: saturation detection previously required `getCurrentPosition() >= getMaxStep()`, but `maxStep` defaults to ±200 million steps when the device has not been homed, so the guard never fired and the motor would spin indefinitely when Zwift requested unachievable wattage. Detection is now time-based: if ERG has been pushing upward for 25 s without power responding, position is held and upward integral is zeroed regardless of where the configured stop is.
- `resetPowerTableFlag` was never cleared after processing, causing the ERG task to delete and reset the power table on every loop tick once the flag was set. The flag is now cleared immediately before the work begins.
- `resetPowerTableFlag` handler set `spinDownFlag = 0` after clearing hMin/hMax, allowing ERG to resume with no travel limits. `autoHomingScheduled` had already fired so auto-homing would not re-trigger in the same session. The handler now sets `spinDownFlag = 2` to schedule a full bidirectional homing run before ERG resumes.
- WiFi OTA: `rebootFlag` was set inside `UPLOAD_FILE_END` before the `onComplete` callback could send its HTTP response, creating a race between the 2-second reboot countdown and TCP delivery of the result. `rebootFlag` is now set solely in `onComplete`, after the response is sent, and fires on both success and failure so the Update state machine is always clean.
- WiFi OTA: `LittleFS.end()` was not called before writing the SPIFFS data partition, risking corruption of cached dirty pages. It is now called before `Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)`.
- WiFi OTA page showed "Success! You can leave this page." when bytes were fully *sent* to the device, before flashing had begun. Progress now shows "Flashing… please wait." during the write phase, and a proper `onload`/`onerror` handler displays the server's 200 OK / 500 FAIL response with guidance to reconnect after reboot.

### Changed

### Hardware


## [26.7.1]

### Added
- Added position-aware power model with saturation learning (`highEndPowerScaleFactor`, K). Bikes whose power meters don't account for resistance report the same wattage regardless of knob position, preventing ERG from covering its full dynamic range. K drives a curve-fit model `W(P,C) = minWatts × (C/50) × [1 + (K−1) × P_norm^1.5]`; when ERG is pinned at the resistance stop and still undershooting target by >15 W for 25 s, K bumps by 0.05 (capped at 6.0) so the model inverse starts directing ERG to reachable positions. K=1.0 (default) is identical to prior behaviour; K persists across reboots in `config.txt` and resets to 1.0 when the power table is wiped. Hidden model constants (cadence reference, gamma, bump step, cap, hold time) are named `#define`s in `settings.h` for future exposure.
- Added automatic homing trigger: if travel limits have never been configured (hMin/hMax both unset), SmartSpin2k now schedules a full homing run on the first detected pedal stroke instead of operating indefinitely with uncalibrated limits.
- Fixed ERG mode stepper saturation: the PID loop's integral no longer winds up in the saturating direction when the motor is already held at a hard stop, and app-configured travel limits (hMin/hMax set via BLE) are now respected without requiring the homed flag to be set first.
- Implemented full PID control for ERG mode: `ErgMode::_inSetpointState()` now computes real integral and derivative terms (with clamped anti-windup) instead of only a proportional term, replacing the previous hardcoded gain-scheduling band-aid. The user-facing min-watts ramp-up boost and `ERGSensitivity` tuning knob are preserved.
- Power table now keeps training from a connected real power meter in the background even while "Power Table for Power" (pTab4Pwr) mode is on, instead of disabling training entirely while the table is in use. A new `rtConfig->rawPmWatts` channel carries the PM's reading independently of `rtConfig->watts` (which pTab4Pwr overwrites with its own estimate), so training never feeds on the table's own predictions.
- Added an early-training transparency bypass: if the table has no real (non-inferred) reading near the current cadence/position operating point, pTab4Pwr now reports the connected PM's real wattage directly instead of an unsupported `ResistanceModel` extrapolation, while training keeps running in the background.
- Added calibration-range-change detection: a full re-home (`SS2K::goHome(true)`) whose resulting travel range diverges more than 15% from the table's previously homed range now downgrades all real table entries to inferred confidence (`PowerTable::downgradeConfidence()`) instead of silently continuing to trust them, since a meaningfully different range suggests the mechanical setup changed.

### Changed
- Fixed `SetTargetPower` (ERG mode) FTMS requests being rejected with `OpCodeNotSupported` while pTab4Pwr was on and only a power meter (no cadence sensor) was connected, since `connectedPM` was never set in that mode; it is now set whenever a real PM sends data, regardless of pTab4Pwr.
- Fixed a stack buffer overflow in the BLE custom characteristic handler: the response buffer was sized to the (often 2-byte) incoming request instead of the largest possible reply, so several read handlers (incline, hMin, hMax, target position, etc.) wrote past the end of the stack array on every BLE read from a connected client.
- Fixed an always-true bounds check (`||` instead of `&&`) on the power table BLE read path that allowed an out-of-bounds row read from `powerTable->ptData.tableRow`.
- Fixed the BLE custom characteristic's incline read/write using a 0.1% scale while FTMS uses 0.01%, causing incline values set/read via the app's custom characteristic to be off by 10x from what the stepper actually uses.
- Fixed Flywheel devices always reporting `INT_MIN` resistance: `FlywheelData::getResistance()` ignored the decoded value, and both `FlywheelData::hasResistance()` and `EchelonData::hasResistance()` unconditionally returned true even with no valid reading.
- Fixed `SpinBLEClient::removeDuplicates()` resetting a local copy of a duplicate device's slot instead of the actual entry in `myBLEDevices`, leaving a stale slot behind after disconnecting the duplicate.
- Fixed an off-by-one in the DirCon protocol's UUID encoding (`uuidToBytes`) that read one byte past the UUID buffer and never read its first byte, corrupting every UUID sent to DirCon (Zwift direct-connect) clients.
- Fixed DirCon `DISCOVER_CHARACTERISTICS` request parsing: the loop advanced by an unrelated value (a UUID's human-readable string length) instead of the wire size per entry, which could loop forever or read attacker-influenced state from a crafted TCP packet; it now parses the UUID/data pairs directly from the incoming bytes.
- Fixed an erroneous `esp_ota_end()` call on a handle that was never opened via `esp_ota_begin()` in the BLE OTA boot-partition-mismatch error path.
- Fixed `loadFromLittleFS()` silently wiping several config fields (device name, WiFi SSID/password, firmware update URL, etc.) to empty/default whenever the saved `config.txt` was missing one of those keys, instead of keeping the value `setDefaults()` had just set.

### Hardware


## [26.6.28]

### Added

### Changed
- Fixed heart rate monitors that also advertise Cycling Power Service (e.g. an HR-to-virtual-power bridge) still being classified as a power meter: Heart Rate Service is now checked before Cycling Power Service when classifying advertised devices, completing the prior HRM-misclassification fix below.
- Fixed `reconnectAllDevices()` iterating `myBLEDevices` by value, so the per-slot state reset after a forced disconnect was applied to a throwaway copy instead of the real device record.
- Fixed Power Correction Factor being applied twice when "Power Table for Power" (pTab4Pwr) mode is enabled: the table is now trained on raw (uncorrected) sensor watts, and `PowerTable::lookup()`/`lookupWatts()` convert to/from corrected units at the class boundary, so PCF is applied exactly once regardless of whether power comes from a live sensor or the table.

### Hardware


## [0.0.0]

### Added

### Changed
- Fixed heart rate monitors being misclassified as power meters: Heart Rate Service is now checked before Cycling Speed & Cadence when classifying advertised devices, and the IC4 FTMS UUID override workaround no longer clobbers a device already identified as a heart rate monitor.
- Fixed Power Correction Factor having no effect when "Power Table for Power" (pTab4Pwr) mode is enabled; PCF is now applied to power-table-derived watts in addition to live sensor readings.
- Fixed a sign-extension bug in `bytes_to_u16` that corrupted decoded values >= 0x80.
- Added mutex protection around the BLE `writeCache` to prevent a race condition between the FTMS task and BLE server.
- Added length guards on BLE custom-characteristic writes to prevent out-of-bounds reads from malformed/short packets.
- Fixed a duplicate `esp_ota_end()` call during firmware updates that could corrupt the OTA slot.
- Fixed a DirCon serial-number buffer size bug and incorrect MAC-to-hex parsing.
- Fixed an integer-overflow risk in the stepper-speed delay calculation.
- Added a 10-second timeout to NTP sync at boot to prevent hanging indefinitely with no network.
- Fixed a float equality comparison in incline tracking to use an epsilon comparison.
- Fixed an operator-precedence bug in the Power Table homed check and a file-size-after-close bug in Power Table logging.
- Added a divide-by-zero guard in stepper trim averaging.
- Removed an incorrect `ARDUINO_ISR_ATTR` attribute from `maintenanceLoop`, which runs as a FreeRTOS task rather than an ISR.
- Removed variable-length-array (VLA) usage in BLE and Power Table logging.

### Hardware


## [26.5.4]

### Added

### Changed
- Adjusted BLE advertising to present SmartSpin2k as a cycling power sensor for improved Garmin discovery and pairing.
- Updated BLE setup to use a public device address for Garmin compatibility.
- added 15 minute timeout to blue status LED -  if not used, then it shuts off. 

### Hardware


## [25.12.11]

### Added

### Changed
 - links card update
## [26.5.2]

### Added
- Added CoolStep support for StealthChop stepper operation.

### Changed
- Improved StallGuard homing reliability with trimmed SG baseline sampling, threshold drift recovery, and stricter multi-tap end stop confirmation.
- Disable CoolStep during StallGuard homing to keep homing current behavior more consistent.

### Hardware


## [26.4.5]

### Added

### Changed

### Hardware


## [26.1.31]

### Added

### Changed

### Hardware


## [26.1.22]

### Added

### Changed

### Hardware
- Added Sole SB1200
- Batch reduced image sizes of reference photos. 


## [26.1.10]

### Added

### Changed
- Updated BLE scan results to include connected devices when using "ANY" wildcard.
- Added feed forward, disabled PowerTable for ERG lookup. 
- Added tests and removal of duplicates in pt column. 
- Added removal of negative numbers in pt table. 
- Refined ERG mode (stateful increase/decrease handling, smarter wait timers, improved PID logging).
- Adjusted FTMS resistance handling: ignore malformed IC Bike ranges, log raw range data, and skip IC Bike resistance samples.
- Rounded cadence/power calculations across CSC, CyclePower, Peloton, FTMS decoding; clamp invalid cadence values.
- Applied rounding for FTMS shift targets/resistance mapping and homing thresholds; use fabs in resistance model and paused duplicate cleanup.
- Fixed ERG mode logic to use correct watt increment and PID window for target calculation.
- Improved power table result validation during ERG mode transitions.
- Reset target incline to 1.0 when spinning stops in ERG mode.

### Hardware


## [25.12.28]

### Added

### Changed
- Removed >0 watts requirement to compute ERG.
- Filter cadence for crazy values. Only >0 && <250 now accepted.
- Filter watts for crazy values. Only >0 && <3000 now accepted. 
- Fixed bug where scans may not happen even when configured devices aren't connected.  
- Worked with Mark Roy to tune PID.
- Added proper rounding from float to int for power and cadence.
- More ERG tweaks for Marc Roy. 
- If homed, we throw out negative PowerTable returns. 
- After startup homing, set gear 8.

### Hardware


## [25.12.17]

### Added

### Changed
 - Links card update.
 - Removed indications from Control Point Characteristic to make Zwift on Android happy. 
 - IC4 reported HR won't override other HRM.
 - Improved ERG response for homed tables.
 - Slightly faster Peloton bike + homing.

### Hardware


## [25.11.21]

### Added

### Changed
- Make Zwift happy during spin down by sending "stop pedaling" every 1 second.
- Added power scalers for stepper hold and homing.
- Updated driver settings for improved stall detection.
- FTMS bikes will now home using reported resistance if available.  

### Hardware


## [25.11.19]

### Added

### Changed

### Hardware


## [25.11.4]

### Added

### Changed
- Added support for reading resistance range from connected FTMS devices
- Improved resistance mode control logic for bikes with and without native resistance reporting
- Fixed resistance value parsing to correctly handle 16-bit values
- Reduced default max brake watts from 1400w to 1000w. 

### Hardware


## [25.10.19]

### Added
## [25.11.3]

### Added

### Changed

### Hardware


## [25.10.19]

### Added
- Added Rouvy Dircon. Working! 

### Changed
- Fixed Rouvy Connection 25-10-18 caused.
- Updated build scripts. 

### Hardware
- Added Merach.


## [25.9.30]

### Added

### Changed

### Hardware


## [25.9.8]

### Added

### Changed
- WiFi will automatically switch/reset if needed after firmware update. 
- BLE advertisement data reworked and optimized. 

### Hardware


## [25.8.26]

### Added

### Changed

### Hardware


## [25.9.17]

### Added

### Changed
- Fixed incline mode handling negative numbers. 

### Hardware


## [25.9.8]

### Added

### Changed
- Only check battery level on initial connection. This is to fix Tempo power meter drops. 
- Moved battery information to the SpinBLEAdvertisedDevice class.
- When adding to SpinBLEAdvertisedDevice, check adevname as well as address to prevent duplicates.
- Stopped reusing BLE clients for better connection reliability.  

### Hardware

## [25.8.26]

### Added

### Changed
- Unique (static) name generation for Android devices to prevent re-pairing issues.

### Hardware


## [25.8.26]

### Added

### Changed

### Hardware


## [25.8.26]

### Added

### Changed

### Hardware


## [25.8.18]

### Added

### Changed
- Added better BLE device logging, even when a connection isn't made.
- Fixed Echelon connections. 

### Hardware

## [25.8.16]

### Added

### Changed

### Hardware

## [25.8.3]

### Added

### Changed
- Fixed rare crash due to calling new scan before the previous onScanEnd callback was complete. 
- Removed serial printf's during updates as it was causing occasional update issues.

### Hardware


## [25.8.3]

### Added

### Changed

### Hardware


## [25.7.30]

### Added

### Changed

### Hardware


## [25.7.29]

### Added
- Added state machine for shifters allowing faster and more reliable shifting. 

### Changed
- Removed unused file. 
- Added license to test files.
- Fixed unterminated comment.
- Fixed BLE and WiFI updates.
- Improving use of pTab4Pwr.
- Fixed edge cases of pTab4Pwr causing runaways.
- Reduced the power output when no power table.
- Fixed Dircon with Mywoosh.
- Fixed Bug with Peloton data being requested too often. 
- Fixed Bug with Dircon data being sent too often. 
- Fixed Bug with BLE TX/RX being overwhelmed by the above two. 
- Homing now takes multiple samples at the start of the run. 

### Hardware


## [25.5.31]

### Added
-Root CA certificates are now updated automatically during every build.

### Changed

- Added CSC sensor selection in BLE Scanner html.
- Turned moving neighbors into a function, moved cubicspline class to power_table.h, merged dircon2, and changed added set_points changes

### Fixed
- Fixed stack smashing protection failure in SpinBLEAdvertisedDevice::enqueueData by adding a buffer size check to prevent overflow when handling BLE notifications larger than the buffer size

### Hardware


## [25.3.13]

### Added

### Changed
- Multiple html and css improvements. 
- Lookup uses existing interpolate and extrapolate functions. 
### Hardware


## [25.1.28]

### Hardware


## [25.4.8]

### Added

### Changed
- PID loop for ERG mode instead of P loop.
- Fixed flags in CPS and CSC which were causing issues in GTA Bike. Thanks @matthewsshirley !
- updated cert.h

### Hardware


## [25.1.28]

### Added

### Changed

### Hardware
- Updated IC SE Insert to fit better.
- Added Schwinn AC bike insert and mount.


## [25.1.19]

### Added

### Changed
- Bugfix in ERG Mode
- Bugfix for spamming log messages when using Peloton and not homed. 

### Hardware


## [25.1.12]

### Added

### Changed
- All New HTML Files!

### Hardware


## [25.1.10]

### Added

### Changed
- Added checks for IC SE Bike Connection. 

### Hardware


## [24.12.8]

### Added

### Changed

### Hardware
- Added Dmasun bike
- Added Equinox Soul Cycle
- Added Sole SB700
- added bike mount for Joroto X2 and any other bike with hex shape front tube
- replaced old inserts for Joroto X2 with new 60.5

## [24.12.7]

### Added

### Changed
- Fixes homing not being removed after powertable reset.
- Shifting will always abort homing, even if homing hasn't been preformed yet. 

### Hardware

## [24.11.25]

### Added

### Changed

### Hardware
- Added rubber band holder to Peloton mount.
- Decreased Peloton insert size slightly. 
- Added rubber band holder to IC4/C6 mount.
- Added Sunny B1805 Bike. 

## [24.11.16]

### Added

### Changed

### Hardware
- Decreased tolerances around bearings and gears.


## [24.11.10]

### Added

### Changed
- Multiple Homing refinements.
- Working with resistance mode on QZ & Peloton
- PowerTable Import via Custom Characteristic fixed. 
- Check for cadence (before homing) so that we don't home when nobody is around.
- Don't depower the stepper if there is cadence. 

### Hardware
- Added Sunny B1805 insert. 

## [24.11.7]

### Added

### Changed
- Homing refinements.
- Resistance shifting improvement.
- Reduced Peloton logging to 1/sec.

### Hardware

## [24.11.5]

### Added
- Knob homing if calibrate trainer is selected in an app.

### Changed
- Added backing off of the stop before we test to prevent runaway grinding during homing. 
- User can abort homing by pressing shifter. 

### Hardware

## [24.10.30]

### Added

- Added pass through shifting in both ERG and SIM mode.
- Refined and added BLE custom characteristics for upcoming configuration app.
- Added CSC Service to BLE server.
- Added Yosuda-007C.
- Updated wiki banner.
- Added automatic update of Changelog sections on pull request to develop. 
- Added support for the Zwift gear display.

### Changed

- Amend always option to git describe.
- Updated communications overview picture.
- Updated kit purchasing links.
- MIN_ERG_CADENCE created and changed from 20 to 30.
- Fixed DNS server in AP mode.
- Fixed an issue with IC4 and variants not displaying device name in Bluetooth scanner. Fixes #500.
- Switched from using Power Table to a Torque Table for better compensation in cad variations.
- added test for invalid Peloton data to keep stepper from running away without resistance information.
- Fixed a bug with Trainer Day and rapid ERG sending.
- Many updates and bug fixes which enable the Config App to communicate with SmartSpin2k.
- Scanned devices no longer saved to filesystem. The new scanning method would keep snowballing them otherwise.
- increased MTU for android.
- Updated WiFi connection setup.
- Firmware no longer updates if only the html files need to be loaded.
- BLE scans blocked during firmware upgrade.
- Increased the default incline multiplier to 5.
- Added more robust activity monitoring and reboot every 30 minutes if there is no activity.
- Updated all references of SmartSkin2K to SmartSpin2k for consistency.
- Fixed bug where BT scanner "Loading" wouldn't disappear if "NONE" and "NONE" were selected.
- Fixed Bug where ERG setpoint state wasn't going to the positive control loop correctly.
- updated arm length readme for JLL IC400
- Added yokeWidth table to bike mount readme for bikes that use the OpenSCAD yoke.
- improved OpenSCAD for yoke to add roundness to the curve.
- Refactored BLE_Server into separate files for each BLE Service.
- Fixed bug in CPS service for Wahoo app (and probably others).
- Depreciated the SPIFFS->LittleFS upgrader.
- Increased ERG mode sensitivity.
- Removed extra logging when loading table.
- Prevent table returns from going in the wrong direction.
- Fixed bug with stepper speed not updating.
- Removed driver temp checking. It's not accurate on the ESP32.
- Peloton resistance limit enhancements.
- Continue updating power metrics to other clients if one client disconnects.
- Freed 19k of ram by consolidating tasks and using timers instead of delays.
- Updated baud rate to 115200 to ensure compatibility with other ESP32 variants.
- Added a final test to check if ERG mode has commanded a move in the proper direction.
- Aligned the values between the config app and web interface.
- Added ability to send target watts through the custom characteristic. 
- Added a final test to check if ERG mode has commanded a move in the proper direction.
- Cleaned up targetPosition to make it easier to understand. 

### Hardware

- added Yesoul S3.
- Wire diameter reduced from 7.2mm to 6.0mm on the window passthrough to accommodate the latest batch of cables.
- Changed reference to M4 bolt to M5 Bolt in the construction instructions pdf.
- Increased right side case mounting hole to 5.5mm so the bolt slides in easier.
- Added Pooboo and York SB300 Bikes.
- Increased size of the arm hardware holes by .25mm.
- Added Spinning L7 bike.
- Added Yosuda bike.
- Added Peloton low profile (for slammed bars) bike mount by @chaloney
- Updated CAD for the case to work flawlessly with small tweaks to motor height.
- Removed some free play in the IC4 insert.
- Added IC Bike SE
- Removed some free play in the IC4 insert.
- Added Bowflex Velocore bike.
- Added another Y cable picture for Peloton.
- Moved wire guard up 1 mm.
- Added JLL-IC400.
- Tightened up tolerances on the case.
- Increased gear spacing by .1mm
- Reduced bearing clearance by .15mm
- Added Stryde Bike.
- Added Life Fitness ICG8.

## [23.6.28]

### Added

- new photos for wiki
- Added battery monitoring of BLE devices by @Flo100. Implemented BLE HID shifting.
- Added table for arm lengths.

### Changed

- Disregard Peloton serial power and cadence if user has a BLE power Meter selected.
- Filesystem no longer updates when auto-update is unchecked.
- Holding shifter buttons on boot now erases LittleFS as well as resetting settings.
- Fixed bug where "none" hr still scanned. Credit to @xpectnil for discovering.
- Simplified Platform Packages to work better with newest version of PlatformIO.
- Fixed broken images in wiki.
- Valid files displayed on OTA page.
- Increased heap for more reliable OTA updates.

### Hardware

- Tweaks to IC4 bike mount
- directory cleanup
- tweaks to echelon bike mount
- Revised an old shifter cover for more options.
- Updated arm folder to procedurally generated arms ov various lengths.
- Updated C7 bike mount to use hook style arms.
- Updated PCB switch placement.
- Updated PCB Inductor.
- Updated PCB Motor Connector.
- Updated PCB Back Side Silkscreen Layer.
- Added fixed length arms.
- Added R3 assembly instructions.
- Added back a modified version of the single button shifter.
- Changed Logo font and position.
- Increased material around the top screw hole.
- Made shifter plugs slightly smaller.
- Increased diameter of shifter strain relief.

## [23.1.22]

### Added

- Added blocking for shifts above or below min/max setpoints.
- Added Peloton serial decoder to sensor factory.
- Added blocking for shifts above or below min/max set points.
- Added power scaler for new board.
- Added Main Index link to develop.html.
- Added feature to automatically reconnect BLE devices if both are specified.
- Added ftms passthrough. FTMS messages from the client app are now passed to a connected FTMS device.
- Added resistance capture to Echelon.
- Added Resistance capture to Flywheel.
- Added Resistance Capture to Peloton.
- Added Resistance capture to FTMS.
- Added scanning when devices are not connected.
- Added ability to set travel limits based on resistance feedback from a bike.
- Added shifting in ERG mode (changes watt target).
- Added shifting in resistance mode (changes resistance target.)

### Changed

- PowerTable values are now adjusted to 90 RPM cad on input.
- PowerTable entries are now validated against previous entries.
- Changes to default settings for better ride-ability. Raised incline multiplier and erg sensitivity, increased incline multiplier and max brake watts.
- Fixed a bug in the new cadence compensation where an int should have been a float.
- Fixed broken pre-commit on my local dev machine.
- Moved serial checking to own function.
- Reduced verbosity of ERG logging.
- Fixed instance of BLE PM dropdown not being saved correctly.
- Moved post connect handling to the ble communication loop. (improves startup stability)
- Fixed bug submitted by @flo100 where MIN_WATTS in ERG should have been userConfig.getMinWatts();
- FTMS resistance mode now changes the attached bike resistance with feedback. (i.e. setting resistance to 50 with a Peloton attached will set 50 on the Peloton)
- Refactored rtConfig to use more measurement class.
- Increased stepper speed when a Peloton is connected. (very light resistance)
- Updated libraries to latest

### Hardware

- Removed duplicate directory in direct mount folder.
- New case for the new PCB :)
- Revised directory structure in /hardware
- updated Bowflex C7 mount for improved usability
- updated Echelon knob insert for durability
- Peloton bike mount updated for improved usability

## [22.10.8]

### Added

- Automatic build script for github.
- Added dependabot.yml
- Added changelog merge automation.
- Added StreamFit
- Added developer tools html.
- Added automatic board revision detection.
- Added THROTTLE_TEMP to settings.h. The internal ESP32 temperature at which to reduce driver current.

### Changed

- Fixed a few compile issues for case sensitive operating systems.
- Release is now the default build option.
- New release is automatically created on pull request merge.
- Fixed HR in the hidden btsimulator.html
- Enabled CORS for doudar/StreamFit.
- Re-arranged index.html.
- restored link to bluetooth scanner.
- Reverted conditional variable initialization in powertable lookup function.
- Simplified cadence compensation in powertable lookup.
- Fixed issue where you couldn't set a ERG target less than 50W (MIN_WATTS wasn't being respected.)
- Increased the BLE active scan window.
- BLE scan page now shows previous scan results.
- BLE scan page duplicates bug fixed.
- BLE scan page dropdowns default to devices found during scan.
- Increased THROTTLE_TEMP from 72c to 85c.

### Hardware

- Ultra Short Direct Mount case for use on bikes with limited space between knob and head tube
- Direct mount and arm for Bowflex C7 - for use with Ultra Short Direct Mount

### Hardware

- Minor improvements to tolerances for direct mount mod
- created peloton-specific arm for direct mount use. IC4 model is usable, but a bit short.
- modified short case to include chamfers and fillets at the screw posts to improve thin wall printability in superslicer
- beefier arm for direct mount
- NEW: Direct Mount short case for bikes with reduced clearance in front of knob.
- NEW: Bolt through short case for direct mount use with Generic Bike http://smile.amazon.com/gp/product/B07S3YWSNM
- NEW: Direct mount for Life Fitness IC7

### Hardware

- Added new case design for upcoming integraded SMT PCB.
- Added Initial KiCAD PCB Commit.

## [2.7.9]

### Added

- Added comment when files are written to LittleFS.
- Added comment when firmware starts to update.
- Added setting for minWatts.
- Can now update LittleFS via update page.
- Removed dependency on jQuery. (Saves 30k in filesystem)

### Changed

- Driver Over Temp logging fixed.
- Updated Libraries to newest versions.
- Disabled setting of min/maxWatts if minWatts/maxWatts is 0.
- Added a check to workaround a bug where a powertable pair member was zero.
- Fixed a bug where a powertable pair could be returned that was larger than the powertable size.
- Changes to default settings.
- Fixed scanning memory leak.
- Scans continuously unless all devices are connected or set "none"

### Hardware

## [2.6.26]

### Added

- Added functions for automatic settings conversion from SPIFFS
-

### Changed

- updated CA for OTA updates
- Converted filesystem from SPIFFS to LittleFS
- Fixed endianness for ftmsPowerRange and ftmsPowerRange.

### Hardware

- added chamfer to screw posts in case body (direct mount mod)
- minor tweak to shifter cable retainer.

## [2.2.8]

### Added

- Added screenshot for wiki main page
- Added functions to start and stop WiFi and Http server.
- Added Additional logging to the custom characteristic.
- Added option to enable/disable UDP logging. Default is disabled.
- Added Wiki links to most SS2K pages. [see #314](https://github.com/doudar/SmartSpin2k/issues/314)
- Added WebSockets for logging [see #173](https://github.com/doudar/SmartSpin2k/issues/173)
- Reworked logging to run log-appender outside the worker task (task no longer blocked by logger traffic).
- WebsocketsAppender can handle multiple (up to 4) clients. Status.html will reconnect if connection to websockets server is disconnected.

### Changed

- Refactored ERG.
- Reset to Default must be confirmed [see #51](https://github.com/doudar/SmartSpin2k/issues/51)
- Update Firmware: Upload dialog accepts .bin, .html and .css files. [see #98](https://github.com/doudar/SmartSpin2k/issues/98)
- Removed conflicting secondary BLE indicate when a shift was preformed via the custom characteristic.
- Default stepper power is now used on reset to defaults.
- Refactored Main and HTTP Server.
- Changed from hard coding to Enums in BLEServer.
- Added simulateWatts to ERG mode internal check.
- Increased BLE Stack(s) and reduced ERG stack.
- Disabled shifter ISR while ERG is running.
- Fixed possible infinite loop in ERG when stepper never reached target position due to being past min or max position.
- When UDP logging is enabled, html will no longer request logging info.
- Increased remote server minimum packet delay to 325ms and max to 700ms.
- Updated Arduino_esp32 to the latest 2.0.2 version.
- Fixed all libraries to static releases.
- Reduced max_connect_retries from 10 to 3.
- Increased max_scan_retries from 1 to 2.
- Now only send notifications for subscribed characteristics.
- Increased JSON size for userConfig (hopefully fix config saving issues).
- Changed LOGE messages in spiffs logging to regular LOG messages so they will display via network logging.
- Complete BLE Client connection code rebase.

### Fixed

- bluetoothscanner.html now lists fitness machine services in the PM list.
- Fixed bug in external control.

### Hardware

- Added Ultra Short Case mod which should allow as little as ~40mm from knob center to head tube.
- Revised shifter for easier printing. Updated printing instructions.
- moved original shifter design into Archive directory

#### Direct mount Mod

- IC4 Mod renamed to Direct Mount Mod. Several directories have changed.
- bike mount and arm added for Echelon Connect Sport
- Arm design revised for added stiffness
- Case, arm and bike mount separated into individual CAD files for easier edits.
- Arm and bike mount re-drawn in CAD. It should be much easier to create designs for new bikes now.
- Added direct mount for Revmaster and Peloton bikes
- New insert for Startrac

## [1.12.30]

### Added

- Added userConfig shifterDir to change direction of shifters in software to compensate for wiring
- Added userConfig StepperDir to change direction of stepper in software to compensate for wiring
- Added backend and html for shifter and stepper directions.
- Added parameters for auto homing.

### Changed

- Fixed a couple bugs in PowerTables
- Fixed BLE Scanner webpage not displaying devices.
- Corrected a check in the FTMS write control point indication.
- readme copy change
- added bridging improvements for screw holes - in cad but missing in STL

### Hardware

-

#### IC4 Mod

-

## [1.12.26]

### Added

- Added Webpage for Shifting.
- Added /shift server on backend.
- Split userConfig into userConfig and rtConfig.
- Added ERG testing to btsimulator.html
- Broke out ERG computation into it's own task.
- Added image for wiki.
- Replaced existing shifter housing with new and improved 2 in 1 revision

### Changed

- Adjusted the order of "Submit" "Reboot" and "Reset to Defaults" on the settings page.
- Adjusted the setting webpage so "reset to defaults" is harder to accidentally press.
- Increased the amount of free stack by removing the default Arduino loop();
- Updated /shift server on to rtConfig.
- Fixed redeclaring global targetposition in moveStepper().
- Renamed Settings page "Submit" button to "Save Setting"

### Hardware

- Increased hex head and nut size to 13mm.
- Increased depth on Knob Cup 2mm so a thicker nut can be used.
- Added assembly .gif images.

#### IC4 Mod

- NEW: Hex bolt mod for 40t gear and matching ic4 cup/mount. This is a drop-in replacement for the plastic printed gear + cup/holder combination. This adds a lot of strength
- Renamed directory to something more apparent.
- Removed need for support material from case.
- Thicker slide design which removes need for washers.
- Slightly shorter slide - should allow more flexibility for ss2k placement on IC4.
- Tighter tolerances on case and bike mount for slide fitting. It should be a tight enough to prevent accidental removal
- Slightly larger diameter holes for m3 screws used in case assembly. Screws should be much easier to insert
- Additional tolerance for m5 fitting.
- Combined knob cup & knob insert for schwinn to reduce amount of plastic needed for the schwinn.
- Including a revised 11t stl - A bit more clearance on inner diameter the drive shaft on my steppers.

## [1.12.2]

### Added

- Firmware update will now download only spiffs files if missing without updating the firmware.
- New UDP logger by @MarkusSchneider .
- Added custom IC4 build and mount by @eMadman .

### BugFixes

- Power Correction factor now minimum .5 maximum 2.5 and added checks to stay within limits.
- 404 now redirects to index file handler.
- settings_processor now checks shiftsteps field to determine if it's on the main settings page.

## [1.11.24]

### Added

- Moved FTMS callback decoding outside of the callback.
- Revamped the way notify buffer works as it was causing a memory leak.
- BLE Custom Characteristic motor driver calls now apply settings received.
- Motor current now automatically scales if ESP32 temp starts getting too high.
- Added comments after compiler #endif Statements to make it easier to see what the partner #if statement is.
- Added BLE_syncMode to support syncing shifterPosition with bikes that also report their resistance level.
- Added git tag to prevent branch from downgrading to the last release.
- Added Hardware Version 2.0.
- MCWPWM for stepper control.
- Erg Sensitivity control added.
- Function to stop motor and release tension if the user stops pedaling in ERG mode.
- Received BLE is now buffered and then processed.
- Added Fitness Machine supported inclination range characteristic.
- Additional unit tests.

### Changed

- Renamed BLE_stepperPosition to BLE_targetPosition to clarify the variable it controls.
- Increased BLE communications task to 3500 stack.
- Fixed recurring debugging line when driver was at normal temp.
- Fixed length of returnValue on custom BLE bool read requests.

## [1.6.19] - 2021-6-19

### Added

- Initial implementation of the custom characteristic.
- Added additional FTMS characteristics and some refactoring of shared variables
- Added GZipped jQuery to fix non WAN connected manual updates.
- Pin arduino-esp32 package to version 1.0.6 to fix build issue
- Added + - Buttons to sliders.
- Added firmware checklist to "~/" for PR and release candidate testing.
- Added README.md to "~/Hardware/\*" that provides help for the files contained within.
- Added BakerEchelonStrap to "~/Hardware/Mounts/".
- Added positive retention clip to "~/Hardware/Mounts".
- Added Logan clip to "~/Hardware/Mounts".
- Added experimental rigid mounting strap. \* Fixed width to 65mm.
- Add images for video links in Wiki Build How To.
- Added webhook for simulated cadence.
- Add image for video link in Build How To
- Added images for video links in Wiki Build How To
- Added XL (Extra Long) Mounting strap for Echelon.
- Added Insert Peloton 7 Flat V2 .sldpart and .stl.
- Added initial credits file.
- Added initial changelog.
- Enabled cpp-lint, pio check, and clang-format to enforce coding standards and catch errors.
- Added support for ruing pre-commit to run pre-push checks.
- Added github workflow on pull_request to validate changelog and coding standards.
- Add hyphens to Flywheel GATT UUIDs.
- Filter Flywheel advertisements by name.
- Added unit tests for CyclePowerData.cpp
- Add documentation to SensorData class.
- Enabled native testing.
- Added logging library which supports levels.

### Changed

- Moved Vin to the correct side on the ESP32 connection diagram.
- Power Correction Factor minimum value is now .5
- Made Revmaster insert slightly smaller.
- Fixed minor spulling errurs.
- Reorganized hardware library into per part subfolders.
- Updater shifter cover to version 9.
- Fixed missing strap loops on non-pcb case.
- Power Correction Factor slider now updates correctly.
- Removed unused http onServer calls.
- Repaired btsimulator.html
- Shortened HR characteristic to 2 bytes (Polar OH1 format)
- Increased ShiftStep UI slider range.
- Replaced DoublePower setting with PowerCorrectionFactor setting.
- Reverted bytes_to_u16 macro.
- Erg mode tweak.
- Added another test for Flywheel BLE name.
- Updated Echelon Insert
- Fixed many issues exposed by the addition of cpp-lint, pio check, and clang-format.
- Fixed merge issues.
- Fixed Echelon licenses.
- Fix Flywheel power/cadence decoding.
- Ignore zero heart rate reported from remote FTMS.
- Fix Assimoa Uno stuck cadence.
- Started extract non-arduino code into a cross-platform library.
- Changed all logging calls to new logging library.

### Removed

- Deleted and ignored .pio folder which had been mistakenly committed.

\*1.3.21

- SS2K BLE Server now accepts more than one simultaneous connection (you can not connect SS2K to both Zwift and another app simultaneously)
- Echelon bike is now supported
- SmartSpin2k.local more accessible with different browsers (fixed certain MDNS dropouts)
- Flywheel bike support built in (still untested)
- Backend (client) completely revamped to allow more device decoders, better stability, and faster network speeds.
- Lots of FTMS server and client polishing
- Added testing for decoders
- Versioning now comes from releases
- NimBLE library included
- Increased total max connections to 6 devices
- Refined debugging logs

\*1.2.15

- Fixed BLE cadence when accumulated torque values are present
- Lowered memory footprint

\*1.2.6

- Added limited Telegram BLE debugging information for development. No sensitive information is sent back. I can make this telegram info available as a private group (in Telegram) if anyone is interested in seeing it. This was added because there are a couple BLE devices that don't seem to conform to the standard protocol and we need more information about them to get them to work properly.
- Internal web UI links now use IP address instead of the local DNS name for compatibility with certain routers.
- Added a favorite icon (favicon.ico) for browser compatibility.
- Fixed an BLE bug which would occasionally cause a crash on scanning.
- Changed priority of subroutines and optimized task memory footprint.
- Streamlined the WiFi connect sequence.

\*0.1.1.22

- Power meter will now switch to the most recently connected and disconnect the other.
- Double power option in the Bluetooth scanner webpage.
- Bluetooth scanning now happens via a flag set in the client task.
- Backend of the Bluetooth scanning page revamped.
- Removed dedicated HTTP server callback for the Bluetooth scanner.

\*0.1.1.11

- All metrics now zero when correct BLE !connected.
- Added HR->PWR off/auto switch.
- WiFi starts up faster.
- BLE now connects device on save.
- BLE Scans are less error prone.
- MDNS Fix for certain android browsers.
- and probably lots of other stuff.

\*0.1.1.2 A new binary version is out in OTAUpdates. Units should automatically update to this newest version.

- Changes:
- WiFi Fallback to AP mode is now 10 seconds.
- WiFi AP mode Fallback SSID is now device name (MDNS name), and the password is whatever you have set.
- ERG Mode slightly more aggressive.
- StealthChop 2 now selectable in settings.
- Holding both shifters at boot resets the unit to defaults and erases filesystem. (firmware remains intact)
- Holding both shifters for 3 seconds after boot preforms a BLE device scan/reconnect.

- Bugfixes:
- Automatic Updates setting switch now works :
