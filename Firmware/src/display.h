#pragma once
/**
 * display.h
 * All rendering: sprite management, map drawing, info panel, OTA screen,
 * boot/waiting screens, and shared UI widget helpers.
 */

#include <Arduino.h>
#include "state.h"
#include "config.h"

// ── Sprite lifecycle
void ensureSprite();
void releaseSprite();

// ── Shared layout helper
void drawSeparator();

// ── Map
void drawMap();

// ── Info panel
void initPanel();
void updatePanel();

// ── OTA screen (full + dynamic-only variants)
void drawOtaScreen       (int chunksRcvd, int chunkTotal,
                           size_t bytesWritten, const char *statusMsg, bool isError);
void drawOtaScreenDynamic(int chunksRcvd, int chunkTotal,
                           size_t bytesWritten, const char *statusMsg, bool isError);

// ── Static screens
void drawWaitingScreen();
void showBootScreen();

// ── OTA abort (CPU0 only)
void otaAbortCPU0(const char *reason);