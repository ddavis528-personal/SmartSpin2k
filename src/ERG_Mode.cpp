/*
 * Copyright (C) 2020  Anthony Doud & Joel Baranick
 * All rights reserved
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ERG_Mode.h"
#include "SS2KLog.h"
#include "Main.h"
#include "BLE_Custom_Characteristic.h"
#include "Power_Table.h"
#include <LittleFS.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

static unsigned long ergTimer = millis() + ERG_MODE_DELAY;
static bool isDelayed         = false;

void ErgMode::runERG() {
  static ErgMode ergMode;
  static PowerBuffer powerBuffer;
  static bool hasConnectedPowerMeter = false;
  static bool simulationRunning      = false;
  static int loopCounter             = 0;

  if (mode == Mode::INCREASING) {
    if (rtConfig->watts.getValue() > rtConfig->watts.getTarget()) {  // Resume PID control
      ergTimer = 0;
      mode     = Mode::MAINTAIN;
      SS2K_LOG(ERG_MODE_LOG_TAG, "ERG increasing target reached.");
    } else if (rtConfig->watts.getValue() >= this->prevWatts.getValue()) {
      // power is still increasing, wait longer
      return;
    }
  } else if (mode == Mode::DECREASING) {
    if (rtConfig->watts.getValue() < rtConfig->watts.getTarget())  // Resume PID control
    {
      ergTimer = 0;
      mode     = Mode::MAINTAIN;
      SS2K_LOG(ERG_MODE_LOG_TAG, "ERG decreasing target reached.");
    } else if (rtConfig->watts.getValue() <= this->prevWatts.getValue()) {
      // power is still decreasing, wait longer
      return;
    }
  }
  if (isDelayed && (ss2k->getCurrentPosition() == ss2k->getTargetPosition())) {
    SS2K_LOG(ERG_MODE_LOG_TAG, "ERG delay cleared,  %dw, tgt %dw, pos %d, tgt %d", rtConfig->watts.getValue(), rtConfig->watts.getTarget(), ss2k->getCurrentPosition(),
             ss2k->getTargetPosition());
    ergTimer  = millis() + ERG_MODE_DELAY;
    isDelayed = false;
  }

  if ((millis() > ergTimer)) {
    if (isDelayed) {
      SS2K_LOG(ERG_MODE_LOG_TAG, "ERG wait expired, %dw, tgt %dw, pos %d, tgt %d", rtConfig->watts.getValue(), rtConfig->watts.getTarget(), ss2k->getCurrentPosition(),
               ss2k->getTargetPosition());
      isDelayed = false;
    }

    // reset the timer.
    ergTimer = millis() + ERG_MODE_DELAY;

    static unsigned long int saveFlagCooldown = 0;
    // save powertable if saveFlag has been set for 10 seconds using a saveFlagCooldown timer
    // this is to provide enough time to transmit a new powerTable using BLE.
    if (powerTable->saveFlag) {
      if (saveFlagCooldown == 0) {
        saveFlagCooldown = millis();
      }
      if ((millis() - saveFlagCooldown) > 10000) {
        powerTable->_save();
        saveFlagCooldown     = 0;
        powerTable->saveFlag = false;
      }
    }
    // Load power table if not yet loaded this session
    if (!powerTable->_hasBeenLoadedThisSession) {
      powerTable->_manageSaveState();
    }

    if (rtConfig->cad.getValue()) {
      hasConnectedPowerMeter = spinBLEClient.connectedPM;
      simulationRunning      = rtConfig->watts.getTarget();
      if (!simulationRunning) {
        simulationRunning = rtConfig->watts.getSimulate();
      }

      if (hasConnectedPowerMeter) {
        // Train on the real PM's ground-truth signal (independent of whatever PTab4Pwr is
        // currently reporting outward via rtConfig->watts), so the table keeps learning in the
        // background even while its own estimate is being used for reporting/control. Training
        // on rtConfig->watts here would risk feeding the table its own predicted output back
        // into itself once PTab4Pwr is on.
        powerTable->processPowerValue(powerBuffer, rtConfig->cad.getValue(), rtConfig->rawPmWatts);
      }

      // compute ERG
      if ((rtConfig->getFTMSMode() == FitnessMachineControlPointProcedure::SetTargetPower) && (hasConnectedPowerMeter || simulationRunning)) {
        ergMode.computeErg();
      }

      // Set Min and Max Stepper positions
      if (loopCounter > 50) {
        loopCounter = 0;
        powerTable->setStepperMinMax();
      }
    }

    if (ss2k->resetPowerTableFlag) {
      ss2k->resetPowerTableFlag = false;  // clear before work so a concurrent set isn't lost
      LittleFS.remove(POWER_TABLE_FILENAME);
      powerTable->reset();
      userConfig->setHMin(INT32_MIN);
      userConfig->setHMax(INT32_MIN);
      rtConfig->setHomed(false);
      userConfig->saveToLittleFS();
      // Wipe clears travel limits, so schedule a full homing run before resuming ERG.
      spinBLEServer.spinDownFlag = 2;
    }
    loopCounter++;
  }

  if (userConfig->getPTab4Pwr()) {
    // only do this twice as often as ERG_MODE_DELAY
    static float previousPower             = 0;
    static unsigned long int pTab4pwrTimer = millis();
    int _smoothPWR                         = 0;
    if (millis() - pTab4pwrTimer > ERG_MODE_DELAY / 2) {
      // reset the timer.
      pTab4pwrTimer = millis();
      // Lookup watts using the Power Table.
      if (powerTable->_hasBeenLoadedThisSession) {
        int tablePWR = powerTable->effectiveWatts(rtConfig->cad.getValue(), ss2k->getCurrentPosition());
        // Early-training bypass: if a real PM is connected but the table has no confident real
        // reading near this operating point yet, report the PM's ground truth directly instead of
        // an unsupported ResistanceModel extrapolation. Skip when K>1 — the model is providing
        // a position-aware estimate that is intentionally different from the flat PM reading.
        if (userConfig->getHighEndPowerScaleFactor() <= 1.0f && spinBLEClient.connectedPM &&
            !powerTable->hasConfidentDataNear(tablePWR, rtConfig->cad.getValue())) {
          tablePWR = rtConfig->rawPmWatts.getValue();
        }
        // Instead of directly outputting this, we should smooth the output by averaging it with the last value.
        _smoothPWR = ((previousPower + tablePWR) / 2);
      } else {
        // only run _manageSaveState every 5 seconds
        static unsigned long int saveStateTimer = millis();
        if ((millis() - saveStateTimer) > 5000) {
          // load the power table, true to skip checks.
          powerTable->_manageSaveState(true);
          saveStateTimer = millis();
        }
      }
      // So the user knows pTab4PWR is enabled, provide some cadence feedback even if the value returned by the table is 0.
      // powerTable->lookupWatts() already returns corrected/final units, so no further scaling is applied here.
      int minimumPower = rtConfig->cad.getValue() / 2;  // 50% of the cadence value
      _smoothPWR       = _smoothPWR < minimumPower ? round((minimumPower + previousPower) / 2.0f) : _smoothPWR;
      rtConfig->watts.setValue(round(_smoothPWR));
      previousPower = (_smoothPWR + previousPower) / 2;
    }
  }
}

// as a note, Trainer Road sends 50w target whenever the app is connected.
void ErgMode::computeErg() {
  int32_t result = RETURN_ERROR;

  bool isUserSpinning = this->_userIsSpinning(rtConfig->cad.getValue(), ss2k->getCurrentPosition());
  if (!isUserSpinning) {
    SS2K_LOG(ERG_MODE_LOG_TAG, "ERG Mode but no User Spin");
    return;
  }

  // set minimum set point to minimum bike watts if app sends set point lower than minimum bike watts.
  if (rtConfig->watts.getTarget() < userConfig->getMinWatts()) {
    SS2K_LOG(ERG_MODE_LOG_TAG, "ERG Target Below Minumum Value.");
    rtConfig->watts.setTarget(userConfig->getMinWatts());
  }

  // check for new watt value or new set point, if watts < 0 treat as faulty
  if ((this->prevWatts.getTimestamp() == rtConfig->watts.getTimestamp() && this->prevWatts.getTarget() == rtConfig->watts.getTarget()) || rtConfig->watts.getValue() < 0) {
    SS2K_LOG(ERG_MODE_LOG_TAG, "Watts previously processed.");
    return;
  }

#ifdef ERG_MODE_USE_POWER_TABLE
  if (abs(this->prevWatts.getTarget() - rtConfig->watts.getTarget()) > (POWERTABLE_WATT_INCREMENT + ERG_MODE_PID_WINDOW) && rtConfig->getHomed()) {
    result = _setPointChangeState();
  }
#endif
#ifdef ERG_MODE_USE_PID
  // Setpoint unchanged
  if (result == INT32_MIN) {
    result = _inSetpointState();
  }
#endif
  _updateValues(result);
}

int32_t ErgMode::_setPointChangeState() {
  this->integral = 0.0;  // starting a new approach to setpoint; don't carry windup from the previous one.
  mode           = (rtConfig->watts.getTarget() > rtConfig->watts.getValue()) ? Mode::INCREASING : Mode::DECREASING;
  // It's better to undershoot increasing watts and overshoot decreasing watts, so lets set the lookup target to the nearest side of POWERTABLE_WATT_INCREMENT
  int adjustedWattTarget = (mode == Mode::INCREASING) ? rtConfig->watts.getTarget() - ERG_MODE_PID_WINDOW : rtConfig->watts.getTarget() + ERG_MODE_PID_WINDOW;
  int adjustedCad        = (mode == Mode::INCREASING) ? rtConfig->cad.getValue() + POWERTABLE_CAD_INCREMENT : rtConfig->cad.getValue() - POWERTABLE_CAD_INCREMENT;

  float K             = userConfig->getHighEndPowerScaleFactor();
  int32_t tableResult = RETURN_ERROR;

  if (K > 1.0f) {
    // Model inverse: W(P,C) = minWatts*(C/C_ref)*[1+(K-1)*P_norm^gamma]
    // Solve for P_norm given target W and C, then convert to stepper position.
    int32_t minStep  = rtConfig->getMinStep();
    int32_t maxStep  = rtConfig->getMaxStep();
    int minWatts     = userConfig->getMinWatts();
    if (maxStep > minStep && minWatts > 0 && adjustedCad > 0) {
      float denom = (float)minWatts * ((float)adjustedCad / (float)ERG_MODEL_CADENCE_REF);
      if (denom > 0.0f) {
        float ratio = (float)adjustedWattTarget / denom;
        if (ratio <= 1.0f) {
          tableResult = minStep;
        } else {
          float P_norm_gamma = (ratio - 1.0f) / (K - 1.0f);
          if (P_norm_gamma >= 1.0f) {
            tableResult = maxStep;
          } else {
            float P_norm = powf(P_norm_gamma, 1.0f / ERG_MODEL_GAMMA);
            tableResult  = (int32_t)round((float)minStep + P_norm * (float)(maxStep - minStep));
          }
        }
      }
    }
  } else {
    tableResult = powerTable->lookup(adjustedWattTarget, adjustedCad);
    // Sanity check — table lookup should never return a negative position when homed.
    if (rtConfig->getHomed() && tableResult < 0) {
      SS2K_LOG(ERG_MODE_LOG_TAG, "PowerTable returned negative result with homing enabled. Using PID");
      tableResult = RETURN_ERROR;
    }
  }

  // Test current watts against the table result. If We're already lower or higher than target, flag the result as a return error.
  if (tableResult != RETURN_ERROR) {
    if (mode == Mode::INCREASING && tableResult <= ss2k->getCurrentPosition()) {
      SS2K_LOG(ERG_MODE_LOG_TAG, "Table Result Failed increasing Test: %d", tableResult);
      tableResult = RETURN_ERROR;
    }
    if (mode == Mode::DECREASING && tableResult >= ss2k->getCurrentPosition()) {
      SS2K_LOG(ERG_MODE_LOG_TAG, "Table Result Failed decreasing Test: %d", tableResult);
      tableResult = RETURN_ERROR;
    }
  }

  // Handle return errors
  if (tableResult == RETURN_ERROR) {
    SS2K_LOG(ERG_MODE_LOG_TAG, "Lookup Error. Using PID");
    tableResult = _inSetpointState();
  } else {
    if (tableResult != ss2k->getCurrentPosition()) {  // add some time to wait while the knob moves to target position.
      isDelayed             = true;
      long int stepDistance = abs(ss2k->getCurrentPosition() - tableResult);
      // Calculate time to add based on step distance and stepper speed
      long int timeToAdd = round((((double)stepDistance * 1000.0) / (double)userConfig->getStepperSpeed()) * 2);
      if (timeToAdd > 10000) {  // 10 seconds
        SS2K_LOG(ERG_MODE_LOG_TAG, "Capping ERG seek time to 10 seconds");
        timeToAdd = 10000;
      }
      SS2K_LOG(ERG_MODE_LOG_TAG, "Adjusted setpoint returned: %dw %drpm Waiting:%dms PowerTable Result: %d", adjustedWattTarget, rtConfig->cad.getValue(), timeToAdd, tableResult);
      ergTimer += timeToAdd;
    }
    ergTimer += (ERG_MODE_DELAY);  // Wait for power meter to register new watts
  }
  return tableResult;
}

// PID CONTROL LOOP
// Error: Difference between target watts and current watts.
// Proportional term: directly proportional to error.
// Integral term: accumulated error over time, eliminates steady-state error near setpoint.
// Derivative term: rate of change of error, damps overshoot on larger corrections.
int32_t ErgMode::_inSetpointState() {
  // Kp is the only user-exposed gain (ERG Sensitivity). Ki/Kd are derived from it via fixed
  // time constants rather than exposing new tuning knobs - Ti/Td below are conservative
  // defaults for a mechanically-delayed system and may need field tuning.
  double Kp            = userConfig->getERGSensitivity();
  constexpr double Ti  = 8.0;  // integral time constant, seconds
  constexpr double Td  = 0.5;  // derivative time constant, seconds
  double Ki            = Kp / Ti;
  double Kd            = Kp * Td;

  int watts  = rtConfig->watts.getValue();
  int target = rtConfig->watts.getTarget();
  int error  = target - watts;

  int prevError = this->prevWatts.getTarget() - this->prevWatts.getValue();
  // dt between the previous and current real watts reading. Falls back to the nominal tick
  // interval if timestamps are missing/stale (e.g. first run) so I/D terms stay well-behaved.
  double dt = (rtConfig->watts.getTimestamp() - this->prevWatts.getTimestamp()) / 1000.0;
  if (dt <= 0 || dt > 5.0) {
    dt = ERG_MODE_DELAY / 1000.0;
  }

  mode = Mode::MAINTAIN;

  double proportional = Kp * error;

  // Integral, with clamped anti-windup so a long error streak can't build an integral term
  // larger than the stepper can ever act on.
  int maxChange        = round((long)userConfig->getStepperSpeed() * ERG_MODE_DELAY / 1000.0f);
  double integralLimit = (Ki > 0) ? (maxChange / Ki) : 0;
  this->integral += error * dt;
  if (this->integral > integralLimit) {
    this->integral = integralLimit;
  } else if (this->integral < -integralLimit) {
    this->integral = -integralLimit;
  }
  double integralTerm = Ki * this->integral;

  double derivativeTerm = Kd * ((error - prevError) / dt);

  double PID_output = proportional + integralTerm + derivativeTerm;

  if (watts < userConfig->getMinWatts()) {
    PID_output = PID_output * userConfig->getERGSensitivity();  // ramp faster below the bike's meaningful dynamic range. Prevents Zwift from timeout on initial interval.
  }

  // log PID terms every five seconds
  static unsigned long lastTime = 0;
  if (millis() - lastTime > 5000) {
    lastTime = millis();
    SS2K_LOG(ERG_MODE_LOG_TAG, "%dw, Target %dw, P: %f, I: %f, D: %f", rtConfig->watts.getValue(), rtConfig->watts.getTarget(), proportional, integralTerm, derivativeTerm);
  }

  // Cap the change to no more than we can move until the next reading
  if (PID_output > maxChange) {
    PID_output = maxChange;
  } else if (PID_output < -maxChange) {
    PID_output = -maxChange;
  }

  // Calculate new incline and clamp to the configured travel limits so that ERG mode
  // saturates cleanly when Zwift requests a wattage the bike can never reach.  Without this,
  // the PID keeps computing targets well outside the reachable range while the motor is
  // already held at the hard stop, the integral winds up in the saturating direction, and
  // recovery is sluggish once a reachable target is set again.
  float newIncline = ss2k->getCurrentPosition() + PID_output;
  if (newIncline < (float)rtConfig->getMinStep()) {
    newIncline = (float)rtConfig->getMinStep();
    if (this->integral < 0) this->integral = 0;
  } else if (newIncline > (float)rtConfig->getMaxStep()) {
    newIncline = (float)rtConfig->getMaxStep();
    if (this->integral > 0) this->integral = 0;
  }

  // Saturation detection: if ERG has been pushing upward without power responding (bike at
  // physical max, motor slipping, or physically disconnected from the knob) hold the current
  // position and suppress upward integral.  Does NOT require the position counter to actually
  // reach maxStep — that default is ±200 million steps and would never trigger in practice.
  // K is also bumped when we confirm we are genuinely at the configured hard stop, so the
  // model learns the correct ceiling for future sessions.
  {
    static unsigned long saturationStart = 0;
    static bool          saturationHeld  = false;
    bool  pushingUp  = (PID_output > 0.0f);
    int   undershoot = rtConfig->watts.getTarget() - rtConfig->watts.getValue();
    float currentK   = userConfig->getHighEndPowerScaleFactor();

    // Release held state once power catches up or ERG is no longer pushing upward.
    if (undershoot <= ERG_MODEL_SATURATION_UNDERSHOOT_W || !pushingUp) {
      saturationHeld  = false;
      saturationStart = 0;
    } else if (!saturationHeld) {
      if (saturationStart == 0) {
        saturationStart = millis();
      } else if (millis() - saturationStart >= ERG_MODEL_SATURATION_HOLD_MS) {
        saturationHeld  = true;
        saturationStart = 0;
        SS2K_LOG(ERG_MODE_LOG_TAG, "ERG saturated at pos %d (target %dw actual %dw)",
                 (int)ss2k->getCurrentPosition(), rtConfig->watts.getTarget(), rtConfig->watts.getValue());
        // Only bump K when actually at the configured physical ceiling.
        if (ss2k->getCurrentPosition() >= rtConfig->getMaxStep() - 1 && currentK < ERG_MODEL_K_MAX) {
          float newK = currentK + ERG_MODEL_K_BUMP_DELTA;
          if (newK > ERG_MODEL_K_MAX) newK = ERG_MODEL_K_MAX;
          userConfig->setHighEndPowerScaleFactor(newK);
          userConfig->saveToLittleFS();
          SS2K_LOG(ERG_MODE_LOG_TAG, "K bumped to %.2f (at physical max)", newK);
        }
      }
    }

    // While saturated: prevent the motor from going higher and clear any upward integral.
    if (saturationHeld) {
      if (newIncline > (float)ss2k->getCurrentPosition()) {
        newIncline = (float)ss2k->getCurrentPosition();
      }
      if (this->integral > 0) this->integral = 0;
    }
  }

  return newIncline;
}

void ErgMode::_updateValues(float newIncline) {
  rtConfig->setTargetIncline(newIncline);
  _writeLog(ss2k->getCurrentPosition(), newIncline, this->prevWatts.getTarget(), rtConfig->watts.getTarget(), this->prevWatts.getValue(), rtConfig->watts.getValue(),
            this->prevCadence.getValue(), rtConfig->cad.getValue());

  this->prevWatts   = rtConfig->watts;
  this->prevCadence = rtConfig->cad;
}

bool ErgMode::_userIsSpinning(int cadence, float incline) {
  if (cadence <= MIN_ERG_CADENCE) {
    rtConfig->setFTMSMode(FitnessMachineControlPointProcedure::SetIndoorBikeSimulationParameters);
    rtConfig->setTargetIncline(1.0f);
    this->integral = 0.0;  // not pedaling; don't let error accumulate while idle.
    return false;          // Cadence too low, nothing to do here
  }
  this->engineStopped = false;
  return true;
}

void ErgMode::_writeLog(float currentIncline, float newIncline, int currentSetPoint, int newSetPoint, int currentWatts, int newWatts, int currentCadence, int newCadence) {
  SS2K_LOGW(ERG_MODE_LOG_CSV_TAG, "%d;%.2f;%.2f;%d;%d;%d;%d;%d", currentIncline, newIncline, currentSetPoint, newSetPoint, currentWatts, newWatts, currentCadence, newCadence);
}
