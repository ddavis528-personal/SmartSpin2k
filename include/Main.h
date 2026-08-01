/*
 * Copyright (C) 2020  Anthony Doud & Joel Baranick
 * All rights reserved
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include "HTTP_Server_Basic.h"
#include "SmartSpin_parameters.h"
#include "BLE_Common.h"
// #include "LittleFS_Upgrade.h"
#include "boards.h"
#include "SensorCollector.h"
#include "SS2KLog.h"

#define MAIN_LOG_TAG "Main"

// Function Prototypes

enum ButtonState {
  RELEASED,
  PRESSED
};

// Calibration status reported to the app over the custom BLE characteristic (0x2F).
// Derived from spinDownFlag / isHoming / abort state rather than stored separately, so it
// cannot drift out of sync with the flags that actually gate the control loop.
enum CalibrationState : uint8_t {
  CALIBRATION_IDLE    = 0,  // Not calibrating. Normal operation.
  CALIBRATION_PENDING = 1,  // Queued; waiting for the user to pedal so homing can start.
  CALIBRATION_ACTIVE  = 2,  // Homing sequence running now.
  CALIBRATION_RETRY   = 3,  // Last attempt failed; will retry once pedaling resumes.
  CALIBRATION_ABORTED = 4,  // User aborted (5 s shifter hold). No retry until re-requested.
  // Repeated mid-travel knob moves stopped changing power, which means the position counter
  // probably no longer matches the physical knob (coupler slip). Advisory only: nothing is
  // recalibrated automatically, because a surprise homing sweep mid-ride is worse than a
  // stale counter.
  CALIBRATION_SLIP_SUSPECTED = 5,
};

class SS2K {
 private:
  unsigned long int lastDebounceTime = 0;
  ButtonState upButtonState;
  ButtonState downButtonState;
  // Timestamps of the current button presses, used to detect a hold-to-abort gesture.
  // 0 means the button is not currently pressed.
  unsigned long int upButtonPressStart   = 0;
  unsigned long int downButtonPressStart = 0;
  int lastShifterPosition;
  int shiftersHoldForScan;
  unsigned long int scanDelayTime;
  unsigned long int scanDelayStart;
  int32_t targetPosition;
  int32_t currentPosition;
  bool ledEnabled;
  void handleShiftButtons();

 public:
  bool stepperIsRunning;
  bool externalControl;
  bool syncMode;
  int txCheck;
  bool pelotonIsConnected;
  bool rebootFlag          = false;
  bool saveFlag            = false;
  bool resetDefaultsFlag   = false;
  bool resetPowerTableFlag = false;
  bool isUpdating          = false;
  bool isHoming            = false;
  // Set by a 5-second shifter-button hold; read by the homing sweeps so they bail out, and by
  // the BLE client task so it stops retrying. Written from the maintenance loop and read from
  // the BLE client task, hence volatile.
  volatile bool calibrationAbortRequested = false;
  // Latched once an abort has been honored, so the app can show "aborted" instead of silently
  // returning to idle. Cleared whenever a new calibration is requested.
  bool calibrationAborted = false;
  // True when the last completed homing attempt failed (so the app can show "retrying").
  bool calibrationFailed = false;

  // Reports the current calibration status for the app. Derived, never stored.
  uint8_t getCalibrationState();
  // Stops any in-progress calibration and prevents the retry loop from restarting it.
  void abortCalibration();

  static void maintenanceLoop(void *pvParameters);
  static void ARDUINO_ISR_ATTR handleUpShift();
  static void ARDUINO_ISR_ATTR handleDownShift();
  static void moveStepper();
  bool _findEndStop(bool moveForward);
  void _findFTMSHome(bool bothDirections = false);
  void _resistanceMove();

  // the position the stepper motor will move to
  int32_t getTargetPosition() { return targetPosition; }
  void setTargetPosition(int32_t tp) { targetPosition = tp; }

  // the position the stepper motor is currently at
  int32_t getCurrentPosition() { return currentPosition; }
  void setCurrentPosition(int32_t cp) { currentPosition = cp; }

  int getLastShifterPosition() { return lastShifterPosition; }
  void setLastShifterPosition(int sp) { lastShifterPosition = sp; }

  void resetIfShiftersHeld();
  void startTasks();
  void stopTasks();
  void restartWifi();
  void setupTMCStepperDriver(bool reset = false);
  void updateStepperPower(int pwr = 0);
  void updateStealthChop(bool coolStepEnabled = true);
  void updateStepperSpeed(int speed = 0);
  void FTMSModeShiftModifier();
  static void rxSerial(void);
  void txSerial();
  bool pelotonConnected();
  void goHome(bool bothDirections = false);
  void setLEDEnabled(bool enabled);
  void updateLED();

  SS2K() {
    upButtonState        = RELEASED;
    downButtonState      = RELEASED;
    targetPosition      = 0;
    currentPosition     = 0;
    ledEnabled          = false;
    stepperIsRunning    = false;
    externalControl     = false;
    syncMode            = false;
    lastShifterPosition = 0;
    shiftersHoldForScan = SHIFTERS_HOLD_FOR_SCAN;
    scanDelayTime       = 10000;
    scanDelayStart      = 0;
    pelotonIsConnected  = false;
    txCheck             = TX_CHECK_INTERVAL;
  }
};

class AuxSerialBuffer {
 public:
  uint8_t data[AUX_BUF_SIZE];
  size_t len;

  AuxSerialBuffer() {
    for (int i = 0; i < AUX_BUF_SIZE; i++) {
      this->data[i] = 0;
    }
    this->len = 0;
  }
};

extern SS2K *ss2k;

// Main program variable that stores most everything
extern userParameters *userConfig;
extern RuntimeParameters *rtConfig;
