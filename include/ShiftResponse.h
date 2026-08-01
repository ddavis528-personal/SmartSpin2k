/*
 * Copyright (C) 2020  Anthony Doud & Joel Baranick
 * All rights reserved
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>

#define SHIFT_RESPONSE_LOG_TAG "ShiftRsp"

// Watches what happens to power after the resistance knob moves.
//
// At a steady cadence, moving the knob must change power. When it doesn't, *where* in the
// travel it happened tells us something different:
//
//   top of travel    - the pad is saturated; the model's high-end scale factor (K) is too low.
//                      This is the fast path for training K from ordinary riding, instead of
//                      waiting for ERG to sit against the configured hard stop for 25 seconds.
//   bottom of travel - a low-end dead zone: shifting down buys nothing below this point.
//   mid travel       - the counter moved but the knob didn't, i.e. the coupler slipped. That is
//                      a mechanical fault, not a model correction, so it is only flagged.
//
// Deliberately does not run in ERG mode: there the controller moves the knob specifically to
// hold power constant, so flat power during a move is the goal rather than a fault. ERG has its
// own saturation detector.
class ShiftResponseMonitor {
 public:
  // Call frequently (self-throttles to SHIFT_RESPONSE_SAMPLE_MS).
  void update();

  // True when repeated mid-travel moves failed to change power, which suggests the position
  // counter no longer matches the physical knob. Recalibration is the cure; nothing is done
  // automatically, because a surprise homing sweep mid-ride is worse than a stale counter.
  bool getSlipSuspected() const { return _slipSuspected; }
  void clearSlipSuspicion();

  // Position where downshifts stopped producing less power, or INT32_MIN when unknown.
  int32_t getLearnedDeadZone() const { return _learnedDeadZone; }

 private:
  enum Phase : uint8_t { PHASE_STABLE, PHASE_MOVING, PHASE_SETTLING };
  enum Region : uint8_t { REGION_UNKNOWN, REGION_BOTTOM, REGION_MID, REGION_TOP };

  void _accumulate(int watts, int cadence);
  void _resetAverages();
  void _reset();
  void _evaluate();
  Region _regionOf(int32_t position) const;
  void _bumpK();

  Phase _phase = PHASE_STABLE;

  unsigned long _lastSample  = 0;
  unsigned long _settleStart = 0;

  int32_t _lastPosition  = 0;
  int32_t _basePosition  = 0;  // where the knob sat before the current move
  int32_t _afterPosition = 0;  // where it came to rest

  // Rolling averages for the current stable window.
  float _avgWatts   = 0.0f;
  float _avgCad     = 0.0f;
  uint16_t _samples = 0;

  // Snapshot taken when a move starts.
  float _beforeWatts = 0.0f;
  float _beforeCad   = 0.0f;
  bool _haveBefore   = false;

  uint8_t _topFlatCount    = 0;
  uint8_t _bottomFlatCount = 0;
  uint8_t _midFlatCount    = 0;

  bool _slipSuspected    = false;
  int32_t _learnedDeadZone = INT32_MIN;
};

extern ShiftResponseMonitor shiftResponse;
