#pragma once
/**
 * ble_handler.h
 * BLE server/characteristic callbacks and the ring-buffer processing task
 * that runs on CPU1.
 */

#include <Arduino.h>

// Called from setup() after queues and BLE stack are initialised.
void bleSetupCallbacks();

// FreeRTOS task pinned to CPU1 — do not call directly.
void bleProcessTask(void *);