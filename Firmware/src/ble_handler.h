#pragma once
/**
 * ble_handler.h
 * BLE server/characteristic callbacks and the ring-buffer processing task
 * that runs on CPU1.
 */

#include <Arduino.h>
#include <BLECharacteristic.h>

// Attach BLEServerCallbacks to pServer. Call after pServer is created.
void bleSetupCallbacks();

// Returns a heap-allocated BLECharacteristicCallbacks that feeds the ring
// buffer. Pass the result to pChar->setCallbacks().
BLECharacteristicCallbacks *createChrCallbacks();

// FreeRTOS task pinned to CPU1 — do not call directly.
void bleProcessTask(void *);