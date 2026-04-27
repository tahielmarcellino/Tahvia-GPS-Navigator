#pragma once
/**
 * display.h
 * All rendering: sprite management, map drawing, info panel, OTA screen,
 * boot/waiting screens, and shared UI widget helpers.
 */

#include <Arduino.h>
#include "state.h"
#include "config.h"

// -----------------------------------------------------------------------------
// Sprite colour depth (set by ensureSprite(); 4, 8, or 0 = direct TFT draw)
// Defined in display.cpp; extern here so other modules can inspect it if needed.
// -----------------------------------------------------------------------------
extern int spriteBpp;

// -----------------------------------------------------------------------------
// Number of rows in the info panel.
// Must match the actual rows drawn in updatePanel() / drawBleRow().
// The dirty-row cache array in display.cpp is sized by this constant.
// -----------------------------------------------------------------------------
#define PANEL_ROW_COUNT 6

// -----------------------------------------------------------------------------
// Function declarations
// -----------------------------------------------------------------------------

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