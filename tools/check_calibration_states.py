#!/usr/bin/env python3
"""Cross-check the calibration state contract between firmware and app.

The firmware defines CalibrationState in include/Main.h and reports a value from
SS2K::getCalibrationState(). The app decodes those values and drives its UI from them. Nothing
ties the three together at compile time, so a state can be defined, entered, and acted on while
getCalibrationState() never returns it - the whole manual calibration flow shipped that way and
silently reported "idle" instead. This catches exactly that.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def fail(msg):
    print(f"calibration-states: {msg}", file=sys.stderr)
    return 1


def main():
    main_h = (ROOT / "include" / "Main.h").read_text()
    main_cpp = (ROOT / "src" / "Main.cpp").read_text()

    enum_match = re.search(r"enum CalibrationState\s*:\s*uint8_t\s*\{(.*?)\};", main_h, re.S)
    if not enum_match:
        return fail("could not find 'enum CalibrationState' in include/Main.h")
    names = re.findall(r"^\s*(CALIBRATION_[A-Z_]+)\s*=", enum_match.group(1), re.M)
    if not names:
        return fail("found the enum but no CALIBRATION_* members")

    body = re.search(r"uint8_t SS2K::getCalibrationState\(\)\s*\{(.*?)\n\}", main_cpp, re.S)
    if not body:
        return fail("could not find SS2K::getCalibrationState() in src/Main.cpp")
    # Scan whole return statements rather than a single identifier, so ternaries like
    # `return failed ? CALIBRATION_RETRY : CALIBRATION_PENDING;` count both branches.
    returned = set()
    for statement in re.findall(r"return[^;]*;", body.group(1)):
        returned.update(re.findall(r"CALIBRATION_[A-Z_]+", statement))

    missing = [n for n in names if n not in returned]
    if missing:
        return fail(
            "these states are defined but getCalibrationState() can never return them, so the "
            "app will never see them: " + ", ".join(missing)
        )

    print(f"calibration-states: OK - all {len(names)} states are reachable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
