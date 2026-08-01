/*
 * Copyright (C) 2020  Anthony Doud & Joel Baranick
 * All rights reserved
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ShiftResponse.h"
#include "Main.h"
#include "SS2KLog.h"
#include "settings.h"
#include <math.h>

ShiftResponseMonitor shiftResponse;

void ShiftResponseMonitor::_resetAverages() {
  _avgWatts = 0.0f;
  _avgCad   = 0.0f;
  _samples  = 0;
}

void ShiftResponseMonitor::_reset() {
  _phase       = PHASE_STABLE;
  _haveBefore  = false;
  _basePosition = ss2k->getCurrentPosition();
  _lastPosition = _basePosition;
  _resetAverages();
}

void ShiftResponseMonitor::clearSlipSuspicion() {
  _slipSuspected = false;
  _midFlatCount  = 0;
}

void ShiftResponseMonitor::_accumulate(int watts, int cadence) {
  // Cumulative mean, so no sample buffer is needed.
  _samples++;
  _avgWatts += ((float)watts - _avgWatts) / (float)_samples;
  _avgCad += ((float)cadence - _avgCad) / (float)_samples;
}

ShiftResponseMonitor::Region ShiftResponseMonitor::_regionOf(int32_t position) const {
  const int32_t low  = rtConfig->getMinStep();
  const int32_t high = rtConfig->getMaxStep();
  if (high <= low) {
    return REGION_UNKNOWN;
  }
  const int32_t margin = (high - low) / SHIFT_RESPONSE_EDGE_DIVISOR;
  if (position >= high - margin) {
    return REGION_TOP;
  }
  if (position <= low + margin) {
    return REGION_BOTTOM;
  }
  return REGION_MID;
}

void ShiftResponseMonitor::_bumpK() {
  const float currentK = userConfig->getHighEndPowerScaleFactor();
  if (currentK >= ERG_MODEL_K_MAX) {
    SS2K_LOG(SHIFT_RESPONSE_LOG_TAG, "Top of travel is flat but K is already at its %.2f ceiling.", ERG_MODEL_K_MAX);
    return;
  }
  float newK = currentK + ERG_MODEL_K_BUMP_DELTA;
  if (newK > ERG_MODEL_K_MAX) {
    newK = ERG_MODEL_K_MAX;
  }
  userConfig->setHighEndPowerScaleFactor(newK);
  // Defer the flash write to the maintenance loop rather than blocking here.
  ss2k->saveFlag = true;
  SS2K_LOG(SHIFT_RESPONSE_LOG_TAG, "Power flat at top of travel over %d moves: K %.2f -> %.2f", SHIFT_RESPONSE_CONFIRMATIONS, currentK, newK);
}

void ShiftResponseMonitor::_evaluate() {
  if (!_haveBefore) {
    return;
  }
  // Only gear-sized moves are informative; small ERG-style nudges are within the noise.
  const int32_t positionDelta = _afterPosition - _basePosition;
  if (abs(positionDelta) < userConfig->getShiftStep()) {
    return;
  }
  // If the rider changed cadence across the move, any power change (or lack of one) could be
  // theirs rather than the knob's.
  if (fabsf(_avgCad - _beforeCad) > (float)SHIFT_RESPONSE_MAX_CAD_DELTA) {
    return;
  }

  const float wattDelta = fabsf(_avgWatts - _beforeWatts);
  const float threshold = fmaxf((float)SHIFT_RESPONSE_FLAT_WATTS, _beforeWatts * SHIFT_RESPONSE_FLAT_FRACTION);
  if (wattDelta >= threshold) {
    // Normal response: the knob is doing its job here, so clear any partial evidence.
    _topFlatCount    = 0;
    _bottomFlatCount = 0;
    _midFlatCount    = 0;
    return;
  }

  const Region region = _regionOf(_afterPosition);
  SS2K_LOG(SHIFT_RESPONSE_LOG_TAG, "Flat response: pos %d->%d (%+d), %.0fw->%.0fw, cad %.0f->%.0f, region %d", _basePosition, _afterPosition, positionDelta, _beforeWatts,
           _avgWatts, _beforeCad, _avgCad, (int)region);

  switch (region) {
    case REGION_TOP:
      if (++_topFlatCount >= SHIFT_RESPONSE_CONFIRMATIONS) {
        _topFlatCount = 0;
        _bumpK();
      }
      break;

    case REGION_BOTTOM:
      if (++_bottomFlatCount >= SHIFT_RESPONSE_CONFIRMATIONS) {
        _bottomFlatCount  = 0;
        _learnedDeadZone = _afterPosition;
        // Recorded for visibility only. Raising the travel floor from this is deliberately not
        // automatic yet: minWatts already provides a learned floor, and stacking a second
        // heuristic on top of it without hardware validation risks eating the gear range.
        SS2K_LOG(SHIFT_RESPONSE_LOG_TAG, "Low-end dead zone learned at %d (downshifts below this produce no power change).", _learnedDeadZone);
      }
      break;

    case REGION_MID:
      if (++_midFlatCount >= SHIFT_RESPONSE_CONFIRMATIONS) {
        _midFlatCount = 0;
        if (!_slipSuspected) {
          _slipSuspected = true;
          SS2K_LOG(SHIFT_RESPONSE_LOG_TAG,
                   "Mid-travel moves are not changing power. The position counter may no longer match the knob (coupler slip). Recalibration recommended.");
        }
      }
      break;

    default:
      break;
  }
}

void ShiftResponseMonitor::update() {
  const unsigned long now = millis();
  if ((now - _lastSample) < SHIFT_RESPONSE_SAMPLE_MS) {
    return;
  }
  _lastSample = now;

  // In ERG the controller moves the knob precisely to keep power on target, so flat power
  // across a move is success, not a fault. ERG has its own saturation detection.
  if (rtConfig->getFTMSMode() == FitnessMachineControlPointProcedure::SetTargetPower) {
    _reset();
    return;
  }
  // Homing drives the knob deliberately into the stops; none of it is a rider-driven sample.
  if (ss2k->isHoming || spinBLEServer.spinDownFlag || ss2k->externalControl) {
    _reset();
    return;
  }

  const int cadence = rtConfig->cad.getValue();
  const int watts   = rtConfig->watts.getValue();
  if (cadence < SHIFT_RESPONSE_MIN_CADENCE || watts < 0) {
    // Coasting: power says nothing about resistance.
    _reset();
    return;
  }

  const int32_t position = ss2k->getCurrentPosition();
  const bool moving      = (position != _lastPosition) || ss2k->stepperIsRunning;
  _lastPosition          = position;

  if (moving) {
    if (_phase == PHASE_STABLE) {
      // Freeze the pre-move picture. _basePosition still holds where the knob was resting.
      _beforeWatts = _avgWatts;
      _beforeCad   = _avgCad;
      _haveBefore  = (_samples >= SHIFT_RESPONSE_MIN_SAMPLES);
      _phase       = PHASE_MOVING;
    }
    return;  // averages taken mid-move would be meaningless
  }

  switch (_phase) {
    case PHASE_MOVING:
      // Came to rest. Give the mechanics and the power meter time to catch up before sampling.
      _phase         = PHASE_SETTLING;
      _settleStart   = now;
      _afterPosition = position;
      _resetAverages();
      break;

    case PHASE_SETTLING:
      if ((now - _settleStart) < SHIFT_RESPONSE_SETTLE_MS) {
        break;
      }
      _accumulate(watts, cadence);
      if (_samples >= SHIFT_RESPONSE_MIN_SAMPLES) {
        _evaluate();
        // The post-move window becomes the baseline for whatever happens next.
        _phase        = PHASE_STABLE;
        _basePosition = position;
        _haveBefore   = false;
      }
      break;

    case PHASE_STABLE:
    default:
      _accumulate(watts, cadence);
      _basePosition = position;
      break;
  }
}
