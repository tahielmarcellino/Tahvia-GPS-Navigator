#pragma once
/**
 * display.h  –  v5
 * All rendering: sprite management, map drawing, overlay panel, OTA screen,
 * boot/waiting screens.
 */
#include <Arduino.h>
#include "state.h"
#include "config.h"

// Sprite colour depth: 4, 8, or 0 (direct TFT). Set by ensureSprite().
extern int spriteBpp;

// ── Sprite lifecycle ──────────────────────────────────────────────────────────
void ensureSprite();
void releaseSprite();

// ── Map ───────────────────────────────────────────────────────────────────────
void drawMap();

// ── Overlay panel ─────────────────────────────────────────────────────────────
void initPanel();
void updatePanel();

// ── OTA screen ────────────────────────────────────────────────────────────────
void drawOtaScreen       (int chunksRcvd, int chunkTotal,
                          size_t bytesWritten, const char *statusMsg, bool isError);
void drawOtaScreenDynamic(int chunksRcvd, int chunkTotal,
                          size_t bytesWritten, const char *statusMsg, bool isError);

// ── Static screens ────────────────────────────────────────────────────────────
void drawWaitingScreen();
void showBootScreen();

// ── OTA abort (CPU0 only) ─────────────────────────────────────────────────────
void otaAbortCPU0(const char *reason);