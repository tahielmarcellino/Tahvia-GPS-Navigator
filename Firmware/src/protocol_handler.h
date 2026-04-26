#pragma once
/**
 * protocol_handler.h
 * Top-level JSON dispatcher and individual message handlers for the
 * BLE protocol: route, GPS, OTA, and version messages.
 */

#include <stddef.h>

// Called from the CPU1 BLE task for every complete JSON object received.
void processJSON(const char *raw, size_t len);