/*
 * Copyright (C) 2020  Anthony Doud & Joel Baranick
 * All rights reserved
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <NimBLEDevice.h>
#include "BLE_Common.h"

class BLE_Fitness_Machine_Service {
 public:
  BLE_Fitness_Machine_Service();
  void setupService(NimBLEServer *pServer, MyCharacteristicCallbacks *chrCallbacks);
  void update();
  bool spinDown(uint8_t response);
  void processFTMSWrite();
  
 private:
  int calculateResistanceFromPosition();
  // True when a bike is actively reporting real resistance (fresh reading, not simulated).
  // Single definition for a check that was previously duplicated inline in update() and
  // the SetTargetResistanceLevel handler.
  static bool hasResistanceReporting();
  // Effective travel limits: homing values when known, otherwise the runtime min/max steps.
  static void getEffectiveTravelLimits(int32_t &minPos, int32_t &maxPos);
  BLEService *pFitnessMachineService;
  BLECharacteristic *fitnessMachineFeature;
  BLECharacteristic *fitnessMachineIndoorBikeData;
  BLECharacteristic *fitnessMachineStatusCharacteristic;
  BLECharacteristic *fitnessMachineControlPoint;
  BLECharacteristic *fitnessMachineResistanceLevelRange;
  BLECharacteristic *fitnessMachinePowerRange;
  BLECharacteristic *fitnessMachineInclinationRange;
  BLECharacteristic *fitnessMachineTrainingStatus;
};

extern BLE_Fitness_Machine_Service fitnessMachineService;
