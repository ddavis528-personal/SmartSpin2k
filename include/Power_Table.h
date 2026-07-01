/*
 * Copyright (C) 2020  Anthony Doud & Joel Baranick
 * All rights reserved
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include "settings.h"
#include "SmartSpin_parameters.h"
#include "PowerTable_Helpers.h"
#include <vector>
#define POWERTABLE_LOG_TAG "PTable"

class PowerTable {
 public:
  bool saveFlag                  = false;
  bool _hasBeenLoadedThisSession = false;

  PTData ptData;
  PTHelpers ptHelpers;

  // Pick up new power value and put them into the power table
  void processPowerValue(PowerBuffer& powerBuffer, int cadence, Measurement power);

  // Sets stepper min/max value from power table
  void setStepperMinMax();

  // Catalogs a new entry into the power table.
  void newEntry(PowerBuffer& powerBuffer);

  // returns target position for given cadence and watts (corrected/final units). Returns RETURN_ERROR if not found.
  int32_t lookup(int watts, int cad);

  // returns watts (corrected/final units) for given cadence and target position. Returns RETURN_ERROR if not found.
  int32_t lookupWatts(int cad, int32_t targetPosition);

  // returns position-aware estimated watts using the learned highEndPowerScaleFactor (K) model.
  // Falls back to lookupWatts() when K==1.0 (default — model not yet active).
  int32_t effectiveWatts(int cad, int32_t targetPosition);

  // true if watts/cad (corrected/final units) is backed by a real reading nearby, rather than
  // pure ResistanceModel extrapolation.
  bool hasConfidentDataNear(int watts, int cad);

  // Downgrades all real entries to inferred confidence (keeps values, requires re-confirmation).
  // Used when a re-home's resulting travel range diverges meaningfully from the table's history.
  void downgradeConfidence();

  // automatically load or save the Power Table
  bool _manageSaveState(bool canSkipReliabilityChecks = false);

  // save powertable from littlefs
  bool _save();

  // Reset the active power table and delete the saved power table.
  bool reset();

  // Display power table in log
  void toLog();

 private:
  unsigned long lastSaveTime = millis();
};

extern PowerTable* powerTable;