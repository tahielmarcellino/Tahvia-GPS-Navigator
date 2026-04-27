/**
 * display.cpp
 * All rendering: sprite management, map drawing, info panel, OTA screen,
 * boot/waiting screens, and shared UI widget helpers.
 *
 * Optimisations vs original:
 *  - No PSRAM dependency; sprite lives entirely in internal heap.
 *  - 4 bpp colour depth (16 colours via palette) halves RAM vs 8 bpp.
 *    Falls back to 8 bpp if 4 bpp allocation fails, then falls back to
 *    direct TFT draw with a clear-before-draw (still faster on slow MCUs
 *    because we skip the sprite overhead entirely).
 *  - Sprite is pushed in a single DMA-friendly call → zero flicker.
 *  - Info-panel rows carry a hash of their last-drawn content; a row is
 *    only redrawn when its content actually changes → far fewer SPI writes
 *    per frame and stable 30 fps on ESP32 without PSRAM.
 *  - drawOtaScreenHeader() is called once (guarded by otaHeaderDrawn) so
 *    the OTA screen never flickers either.
 */

#include "display.h"
#include "geo.h"
#include <Update.h>
#include <math.h>
#include <string.h>

// =============================================================================
// Sprite management  (no PSRAM – internal heap only)
// =============================================================================

/**
 * Try to allocate the map sprite.
 * Attempt order: 4 bpp → 8 bpp → give up (direct-TFT mode).
 * Call once at startup (or after releaseSprite()).
 */
void ensureSprite()
{
    if (spriteOk && mapSprite.created()) return;
    if (spriteTried)                     return;
    spriteTried = true;

    // ── 4 bpp attempt ────────────────────────────────────────────────────────
    mapSprite.setColorDepth(4);
    mapSprite.createSprite(MAP_W, MAP_H);
    if (mapSprite.created()) {
        spriteOk       = true;
        spriteBpp      = 4;
        // Build a compact 16-colour palette that covers every colour used in
        // drawMap().  Indices must match the C_PAL_* constants in display.h.
        uint16_t pal[16] = {
            C_MAP_BG,      // 0
            C_ROUTE_SHAD,  // 1
            C_ROUTE_FILL,  // 2
            C_DONE_SHAD,   // 3
            C_DONE_FILL,   // 4
            C_TURN_DOT,    // 5
            C_START,       // 6
            C_END,         // 7
            TFT_WHITE,     // 8
            RGB(180,40,30),// 9  – end-pin dark ring
            C_YOU_PULSE,   // 10
            C_YOU_RING,    // 11
            C_YOU_FILL,    // 12
            C_BLUE,        // 13
            TFT_BLACK,     // 14
            C_PANEL_BG,    // 15  (rarely needed on map but keeps palette full)
        };
        mapSprite.createPalette(pal, 16);
        Serial.printf("[SPRITE] 4bpp allocated – heap=%u\n", ESP.getFreeHeap());
        return;
    }

    // ── 8 bpp fallback ───────────────────────────────────────────────────────
    mapSprite.setColorDepth(8);
    mapSprite.createSprite(MAP_W, MAP_H);
    if (mapSprite.created()) {
        spriteOk  = true;
        spriteBpp = 8;
        Serial.printf("[SPRITE] 8bpp allocated (4bpp failed) – heap=%u\n",
                      ESP.getFreeHeap());
        return;
    }

    // ── No sprite – direct TFT draw ──────────────────────────────────────────
    spriteOk  = false;
    spriteBpp = 0;
    Serial.printf("[SPRITE] alloc failed – direct TFT draw active – heap=%u\n",
                  ESP.getFreeHeap());
}

void releaseSprite()
{
    if (!mapSprite.created()) { spriteOk = false; return; }
    mapSprite.deleteSprite();
    spriteOk    = false;
    spriteTried = false;
    spriteBpp   = 0;
    Serial.printf("[SPRITE] freed for OTA – heap=%u\n", ESP.getFreeHeap());
}

// =============================================================================
// Shared layout helper
// =============================================================================

void drawSeparator()
{
    tft.drawFastVLine(INFO_X, 0, SCREEN_H, C_PANEL_LINE);
}

// =============================================================================
// Drawing-target abstraction  (sprite → TFT fallback)
// =============================================================================

static inline void mapFill(uint16_t color)
{
    if (spriteOk) mapSprite.fillSprite(color);
    else          tft.fillRect(0, 0, MAP_W, MAP_H, color);
}

static inline void mapPush()
{
    // pushSprite() sends the complete sprite buffer to the display driver in
    // one SPI transaction – the display never sees a partial frame, so there
    // is no visible tearing or flicker.
    if (spriteOk) mapSprite.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Primitive wrappers – route calls to sprite or TFT
// ---------------------------------------------------------------------------

static void thickLine(int x0, int y0, int x1, int y1, int w, uint16_t col)
{
    float dx  = (float)(x1 - x0);
    float dy  = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) {
        if (spriteOk) mapSprite.drawPixel(x0, y0, col);
        else          tft.drawPixel(x0, y0, col);
        return;
    }
    float nx   = -dy / len;
    float ny   =  dx / len;
    int   half = w / 2;
    for (int t = -half; t <= half; t++) {
        int ax = x0 + (int)roundf(nx * t);
        int ay = y0 + (int)roundf(ny * t);
        int bx = x1 + (int)roundf(nx * t);
        int by = y1 + (int)roundf(ny * t);
        if (spriteOk) mapSprite.drawLine(ax, ay, bx, by, col);
        else          tft.drawLine(ax, ay, bx, by, col);
    }
}

static inline void mapFillCircle(int x, int y, int r, uint16_t c)
{
    if (spriteOk) mapSprite.fillCircle(x, y, r, c);
    else          tft.fillCircle(x, y, r, c);
}

static inline void mapFillTriangle(int x0, int y0,
                                    int x1, int y1,
                                    int x2, int y2, uint16_t c)
{
    if (spriteOk) mapSprite.fillTriangle(x0, y0, x1, y1, x2, y2, c);
    else          tft.fillTriangle(x0, y0, x1, y1, x2, y2, c);
}

static inline void mapSetTextColor(uint16_t fg, uint16_t bg)
{
    if (spriteOk) mapSprite.setTextColor(fg, bg);
    else          tft.setTextColor(fg, bg);
}

static inline void mapSetTextDatum(uint8_t d)
{
    if (spriteOk) mapSprite.setTextDatum(d);
    else          tft.setTextDatum(d);
}

static inline void mapSetTextSize(uint8_t s)
{
    if (spriteOk) mapSprite.setTextSize(s);
    else          tft.setTextSize(s);
}

static inline void mapDrawString(const char *str, int x, int y)
{
    if (spriteOk) mapSprite.drawString(str, x, y);
    else          tft.drawString(str, x, y);
}

// =============================================================================
// UI widget helpers  (always write to tft – info panel is not double-buffered)
// =============================================================================

static inline void tftRoundRect(int x, int y, int w, int h, int r, uint16_t col)
{
    tft.fillRoundRect(x, y, w, h, r, col);
}

static void drawProgressBar(int x, int y, int w, int h,
                             int pct, uint16_t fillCol, uint16_t trackCol)
{
    int r = h / 2;
    tftRoundRect(x, y, w, h, r, trackCol);
    int fillW = (int)(w * constrain(pct, 0, 100) / 100.0f);
    if      (fillW >= h)  tftRoundRect(x, y, fillW, h, r, fillCol);
    else if (fillW > 0)   tft.fillRect(x, y, fillW, h, fillCol);
}

static void drawChip(int x, int y, int w, int h, int r,
                     uint16_t bgCol, uint16_t txtCol,
                     const char *text, uint8_t sz = 1)
{
    tftRoundRect(x, y, w, h, r, bgCol);
    tft.setTextColor(txtCol, bgCol);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(sz);
    tft.drawString(text, x + w / 2, y + h / 2);
}

// =============================================================================
// Map drawing
// =============================================================================

void drawMap()
{
    if (zoomedMode && gpsValid)
        buildZoomedVP(gpsLat, gpsLon);
    else if (routeComplete || route.size() >= 2)
        buildVP();

    // Fill sprite (or direct rect) – this is NOT visible until mapPush().
    mapFill(C_MAP_BG);

    // ── No-route placeholder ─────────────────────────────────────────────────
    if (!vp.ready || route.size() < 2) {
        int cx = MAP_W / 2, cy = MAP_H / 2;
        mapFillCircle(cx, cy - 22, 18, C_BLUE);
        mapFillCircle(cx, cy - 22, 12, C_MAP_BG);
        mapFillCircle(cx, cy - 22,  7, C_BLUE);
        mapFillCircle(cx, cy - 22,  3, C_MAP_BG);
        mapFillCircle(cx, cy - 22,  2, C_BLUE);
        mapSetTextColor(TFT_BLACK, C_MAP_BG);
        mapSetTextDatum(MC_DATUM);
        mapSetTextSize(2);
        mapDrawString("No route loaded", cx, cy + 6);
        mapSetTextSize(1);
        mapSetTextColor(C_BLUE, C_MAP_BG);
        mapDrawString("Connect app to send a route", cx, cy + 26);
        mapPush();
        return;
    }

    // ── Route – 2 passes: shadow then fill ───────────────────────────────────
    for (int pass = 0; pass < 2; pass++) {
        int lw = (pass == 0) ? 7 : 4;
        int x0, y0, x1, y1;
        l2p(route[0].lat, route[0].lon, x0, y0);
        for (size_t i = 1; i < route.size(); i++) {
            l2p(route[i].lat, route[i].lon, x1, y1);
            bool     done = (nearestIdx >= 0 && (int)i <= nearestIdx);
            uint16_t col;
            if (pass == 0) col = done ? C_DONE_SHAD : C_ROUTE_SHAD;
            else           col = done ? C_DONE_FILL  : C_ROUTE_FILL;
            thickLine(x0, y0, x1, y1, lw, col);
            x0 = x1; y0 = y1;
        }
    }

    // ── Turn dots ─────────────────────────────────────────────────────────────
    for (int idx : turnIndices) {
        if (idx < 0 || idx >= (int)route.size()) continue;
        int tx, ty;
        l2p(route[idx].lat, route[idx].lon, tx, ty);
        bool done = (nearestIdx >= 0 && idx <= nearestIdx);
        mapFillCircle(tx, ty, 7, done ? C_DONE_SHAD : C_ROUTE_SHAD);
        mapFillCircle(tx, ty, 5, done ? C_DONE_FILL  : C_ROUTE_FILL);
        mapFillCircle(tx, ty, 3, C_TURN_DOT);
    }

    // ── Start dot ─────────────────────────────────────────────────────────────
    {
        int sx, sy;
        l2p(route.front().lat, route.front().lon, sx, sy);
        mapFillCircle(sx, sy, 9, C_ROUTE_SHAD);
        mapFillCircle(sx, sy, 7, C_START);
        mapFillCircle(sx, sy, 3, TFT_WHITE);
    }

    // ── End pin ───────────────────────────────────────────────────────────────
    {
        int ex, ey;
        l2p(route.back().lat, route.back().lon, ex, ey);
        mapFillCircle(ex, ey - 4, 9, RGB(180, 40, 30));
        mapFillCircle(ex, ey - 4, 7, C_END);
        mapFillCircle(ex, ey - 4, 3, TFT_WHITE);
        mapFillTriangle(ex - 4, ey,   ex + 4, ey, ex, ey + 8, RGB(180, 40, 30));
        mapFillTriangle(ex - 3, ey,   ex + 3, ey, ex, ey + 6, C_END);
    }

    // ── YOU marker ───────────────────────────────────────────────────────────
    if (gpsValid) {
        float drawLat = gpsLat, drawLon = gpsLon;
        if (snapped && nearestIdx >= 0) {
            drawLat = route[nearestIdx].lat;
            drawLon = route[nearestIdx].lon;
        }
        int px, py;
        l2p(drawLat, drawLon, px, py);

        mapFillCircle(px, py, 18, C_YOU_PULSE);

        float hRad = gpsHeading * DEG_TO_RAD;
        int ctx = px + (int)(28 * sinf(hRad));
        int cty = py - (int)(28 * cosf(hRad));
        int clx = px + (int)(10 * sinf(hRad - 0.45f));
        int cly = py - (int)(10 * cosf(hRad - 0.45f));
        int crx = px + (int)(10 * sinf(hRad + 0.45f));
        int cry = py - (int)(10 * cosf(hRad + 0.45f));
        mapFillTriangle(ctx, cty, clx, cly, crx, cry, C_YOU_PULSE);

        mapFillCircle(px, py, 11, C_YOU_RING);
        mapFillCircle(px, py,  9, C_ROUTE_SHAD);
        mapFillCircle(px, py,  8, C_YOU_FILL);
    }

    // Single atomic push – display sees a complete, stable frame every time.
    mapPush();
}

// =============================================================================
// Info panel  –  dirty-row caching to minimise SPI traffic
// =============================================================================

// Each row stores a 32-bit FNV-1a hash of the last string/colour combination
// written.  A row is only redrawn when the hash changes.
// This lets updatePanel() be called every loop() tick without burning SPI
// bandwidth on unchanged content.

static uint32_t rowHash[PANEL_ROW_COUNT] = {0};

static uint32_t fnv1a(const char *s, uint16_t col)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    h ^= (uint16_t)col; h *= 16777619u;
    return h;
}

static uint32_t fnv1aBar(const char *s, int pct, uint16_t col)
{
    uint32_t h = fnv1a(s, col);
    h ^= (uint32_t)pct; h *= 16777619u;
    return h;
}

static void clearRow(int row)
{
    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    tft.fillRect(INFO_X + 1, y, INFO_W - 1, PANEL_ROW_H - 1, C_PANEL_BG);
    tft.drawFastHLine(INFO_X + 1, y + PANEL_ROW_H - 1, INFO_W - 1, C_PANEL_LINE);
}

static void drawRow(int row, const char *label, const char *value,
                    uint16_t valColor = TFT_BLACK)
{
    uint32_t h = fnv1a(value, valColor);
    if (rowHash[row] == h) return;   // nothing changed – skip redraw
    rowHash[row] = h;

    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    clearRow(row);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 7, y + 6);
    tft.setTextColor(valColor, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X + 7, y + 20);
}

static void drawRowWithBar(int row, const char *label, const char *value,
                            int pct, uint16_t barCol)
{
    uint32_t h = fnv1aBar(value, pct, barCol);
    if (rowHash[row] == h) return;
    rowHash[row] = h;

    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    clearRow(row);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 7, y + 5);
    tft.setTextColor(TFT_BLACK, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X + 7, y + 17);
    drawProgressBar(INFO_X + 7, y + 36, INFO_W - 14, 6, pct, barCol, C_BAR_TRACK);
}

static void drawRowWithChip(int row, const char *label,
                              const char *chipText,
                              uint16_t chipBg, uint16_t chipTxt)
{
    uint32_t h = fnv1a(chipText, chipBg ^ chipTxt);
    if (rowHash[row] == h) return;
    rowHash[row] = h;

    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    clearRow(row);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 7, y + 6);
    drawChip(INFO_X + 7, y + 20, INFO_W - 14, 20, 5, chipBg, chipTxt, chipText, 1);
}

static void drawBleRow(bool connected)
{
    // Only two states – use a simple boolean hash
    uint32_t h = connected ? 0xBEEF0001u : 0xBEEF0000u;
    if (rowHash[5] == h) return;
    rowHash[5] = h;

    int row = 5;
    int y   = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    int h_  = min(SCREEN_H - y, PANEL_ROW_H);
    tft.fillRect(INFO_X + 1, y, INFO_W - 1, h_, C_PANEL_BG);
    uint16_t dotCol = connected ? C_GREEN : C_AMBER;
    tft.fillCircle(INFO_X + 12, y + 14, 4, dotCol);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_BLACK, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(connected ? "Connected" : "Waiting...", INFO_X + 21, y + 14);
}

// ---------------------------------------------------------------------------
// Panel initialisation  (called once at boot, or after a full screen wipe)
// ---------------------------------------------------------------------------

void initPanel()
{
    // Invalidate all row caches so the first updatePanel() redraws everything.
    memset(rowHash, 0, sizeof(rowHash));

    tft.fillRect(INFO_X, 0, INFO_W, SCREEN_H, C_PANEL_BG);
    drawSeparator();

    // Header
    tft.fillRect(INFO_X + 1, 0, INFO_W - 1, 28, C_PANEL_BG);
    tft.drawFastHLine(INFO_X + 1, 28, INFO_W - 1, C_PANEL_LINE);
    tft.setTextColor(TFT_BLACK, C_PANEL_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.drawString("NAVIGATOR", INFO_X + 8, 14);
    tft.fillCircle(INFO_X + INFO_W - 10, 14, 4, C_BLUE);
}

// ---------------------------------------------------------------------------
// Panel update  (call every frame – dirty caching keeps it cheap)
// ---------------------------------------------------------------------------

void updatePanel()
{
    extern float haversineKm(float, float, float, float);
    char buf[32];

    // ── Row 0: Speed ──────────────────────────────────────────────────────────
    {
        float    kmh = gpsSpeed * 3.6f;
        uint16_t col = (kmh > 1.0f) ? TFT_BLACK : C_BLUE;
        if (kmh < 10.0f) snprintf(buf, sizeof(buf), "%.1f km/h", kmh);
        else              snprintf(buf, sizeof(buf), "%.0f km/h", kmh);
        drawRow(0, "SPEED", buf, col);
    }

    // ── Row 1: Battery ───────────────────────────────────────────────────────
    {
        uint16_t barCol = (batPercent > 50) ? C_GREEN
                        : (batPercent > 20) ? C_AMBER
                        :                     C_RED;
        snprintf(buf, sizeof(buf), "%d%%", batPercent);
        drawRowWithBar(1, "BATTERY", buf, batPercent, barCol);
    }

    // ── Row 2: Trip ───────────────────────────────────────────────────────────
    {
        if (tripKm < 1.0f) snprintf(buf, sizeof(buf), "%.0f m",  tripKm * 1000.0f);
        else                snprintf(buf, sizeof(buf), "%.2f km", tripKm);
        drawRow(2, "TRIP", buf, TFT_BLACK);
    }

    // ── Row 3: Route progress ─────────────────────────────────────────────────
    {
        if (routeComplete && !route.empty()) {
            if (nearestIdx == 0) {
                drawRowWithChip(3, "ROUTE", "On route", C_CHIP_GREEN_BG, C_GREEN);
            } else {
                float doneKm  = 0.0f;
                float totalKm = 0.0f;
                for (int i = 1; i < (int)route.size(); i++) {
                    float seg = haversineKm(route[i-1].lat, route[i-1].lon,
                                            route[i].lat,   route[i].lon);
                    totalKm += seg;
                    if (i <= nearestIdx) doneKm += seg;
                }
                int pct = (totalKm > 0) ? (int)(100.0f * doneKm / totalKm) : 0;
                if (doneKm < 1.0f)
                    snprintf(buf, sizeof(buf), "%.0fm/%.1fkm",
                             doneKm * 1000.0f, totalKm);
                else
                    snprintf(buf, sizeof(buf), "%.1f/%.1fkm", doneKm, totalKm);
                drawRowWithBar(3, "ROUTE", buf, pct, C_BLUE);
            }
        } else if (!route.empty()) {
            snprintf(buf, sizeof(buf), "%d/%d", (int)route.size(), routeExpected);
            drawRow(3, "LOADING", buf, C_AMBER);
        } else {
            drawRow(3, "ROUTE", "--", C_BLUE);
        }
    }

    // ── Row 4: Snap / nearest ─────────────────────────────────────────────────
    {
        if (snapped && nearestIdx >= 0) {
            snprintf(buf, sizeof(buf), "WP #%d", nearestIdx);
            drawRowWithChip(4, "POSITION", buf, C_CHIP_BLUE_BG, C_BLUE);
        } else if (gpsValid && nearestIdx >= 0) {
            if (nearestDistM < 1000.0f)
                snprintf(buf, sizeof(buf), "%.0f m away", nearestDistM);
            else
                snprintf(buf, sizeof(buf), "%.1f km", nearestDistM / 1000.0f);
            drawRowWithChip(4, "NEAREST", buf, C_CHIP_GREEN_BG, C_GREEN);
        } else {
            drawRow(4, "NEAREST", "--", C_BLUE);
        }
    }

    // ── Row 5: BLE ────────────────────────────────────────────────────────────
    drawBleRow(bleCon);
}

// =============================================================================
// OTA screen
// =============================================================================

static void drawOtaScreenHeader()
{
    tft.fillScreen(C_OTA_BG);

    // Dark header band
    tft.fillRect(0, 0, SCREEN_W, 73, C_BLUE);
    tft.fillCircle(36, 36, 22, TFT_WHITE);
    for (int t = -1; t <= 1; t++)
        tft.drawFastVLine(36 + t, 22, 16, C_BLUE);
    tft.fillTriangle(24, 37, 48, 37, 36, 50, C_BLUE);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, C_BLUE);
    tft.setTextSize(3);
    tft.drawString("FIRMWARE UPDATE", 70, 36);
    drawChip(SCREEN_W - 74, 8, 62, 22, 6, TFT_WHITE, C_BLUE, FW_VERSION, 1);
    tft.drawFastHLine(0, 72, SCREEN_W, C_ROUTE_SHAD);

    // Red warning band
    tft.fillRect(0, 248, SCREEN_W, SCREEN_H - 248, C_RED);
    tft.fillCircle(30, 284, 16, TFT_WHITE);
    tft.fillRect(28, 274, 4, 10, C_RED);
    tft.fillCircle(30, 288, 2, C_RED);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, C_RED);
    tft.setTextSize(2);
    tft.drawString("Do not power off or disconnect device", 54, 280);
}

void drawOtaScreenDynamic(int chunksRcvd, int chunkTotal,
                           size_t bytesWritten, const char *statusMsg, bool isError)
{
    tft.fillRect(0, 73, SCREEN_W, 175, C_OTA_BG);

    float    pct      = (chunkTotal > 0) ? (float)chunksRcvd / (float)chunkTotal : 0.0f;
    int      pctI     = (int)(pct * 100.0f);
    uint16_t accentCol = isError ? C_RED : C_BLUE;

    // Hero numeral
    char heroBuf[8];
    snprintf(heroBuf, sizeof(heroBuf), "%d%%", pctI);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(accentCol, C_OTA_BG);
    tft.setTextSize(6);
    tft.drawString(heroBuf, SCREEN_W / 2, 108);
    tft.setTextSize(1);
    tft.setTextColor(C_BLUE, C_OTA_BG);
    tft.drawString("transferred", SCREEN_W / 2, 148);

    // Progress bar
    drawProgressBar(24, 158, SCREEN_W - 48, 12,
                    pctI, isError ? C_RED : C_BLUE, C_BAR_TRACK);

    // Data row
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_BLUE, C_OTA_BG);
    tft.setTextSize(1);
    tft.drawString("RECEIVED", 24, 180);

    char bytesBuf[20];
    if (bytesWritten < 1024)
        snprintf(bytesBuf, sizeof(bytesBuf), "%u B",    (unsigned)bytesWritten);
    else
        snprintf(bytesBuf, sizeof(bytesBuf), "%.1f KB", bytesWritten / 1024.0f);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_BLACK, C_OTA_BG);
    tft.setTextSize(2);
    tft.drawString(bytesBuf, SCREEN_W - 24, 182);
    tft.drawFastHLine(24, 200, SCREEN_W - 48, C_PANEL_LINE);

    // Status chip
    uint16_t chipBg, chipTxt;
    if (isError) {
        chipBg = C_CHIP_RED_BG;   chipTxt = C_RED;
    } else if (pctI >= 100) {
        chipBg = C_CHIP_GREEN_BG; chipTxt = C_GREEN;
    } else {
        chipBg = C_CHIP_BLUE_BG;  chipTxt = C_BLUE;
    }
    tftRoundRect(24, 210, SCREEN_W - 48, 30, 8, chipBg);
    tft.fillCircle(46, 225, 5, chipTxt);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(chipTxt, chipBg);
    tft.setTextSize(1);
    tft.drawString(statusMsg, 58, 225);
}

void drawOtaScreen(int chunksRcvd, int chunkTotal,
                   size_t bytesWritten, const char *statusMsg, bool isError)
{
    // Header is static – only draw it once.
    if (!otaHeaderDrawn) {
        drawOtaScreenHeader();
        otaHeaderDrawn = true;
    }
    drawOtaScreenDynamic(chunksRcvd, chunkTotal, bytesWritten, statusMsg, isError);
}

// =============================================================================
// Static screens
// =============================================================================

void drawWaitingScreen()
{
    tft.fillRect(0, 0, MAP_W - 1, MAP_H, TFT_WHITE);
    drawSeparator();

    int cx = MAP_W / 2;
    int cy = MAP_H / 2;

    tftRoundRect(cx - 22, cy - 60, 44, 44, 10, C_CHIP_BLUE_BG);
    tft.drawCircle(cx, cy - 38, 14, C_BLUE);
    tft.drawCircle(cx, cy - 38, 10, C_BLUE);
    tft.fillCircle(cx, cy - 38,  5, C_BLUE);
    tft.fillCircle(cx, cy - 38,  2, TFT_WHITE);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextSize(2);
    tft.drawString("No Connection", cx, cy - 4);
    tft.setTextSize(1);
    tft.setTextColor(C_BLUE, TFT_WHITE);
    tft.drawString("Open the app and hit the 'Connect Device'", cx, cy + 18);
}

void showBootScreen()
{
    tft.fillScreen(C_BOOT_BG);
    int cx = SCREEN_W / 2;
    int cy = SCREEN_H / 2;

    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_BLACK, C_BOOT_BG);
    tft.setTextSize(4);
    tft.drawString("TAH", cx, cy - 24);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_BLUE, C_BOOT_BG);
    tft.setTextSize(4);
    tft.drawString("VIA", cx, cy - 24);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, C_BOOT_BG);
    tft.setTextSize(1);
    tft.drawString("GPS NAVIGATOR", cx, cy + 8);

    drawChip(cx - 40, cy + 22, 80, 20, 5, C_CHIP_BLUE_BG, C_BLUE, FW_VERSION, 1);
    delay(2500);
}

// =============================================================================
// OTA abort (CPU0 only)
// =============================================================================

void otaAbortCPU0(const char *reason)
{
    Serial.printf("[OTA] ABORT: %s\n", reason);
    Update.abort();
    otaState       = OTA_ERROR;
    otaIsError     = true;
    otaStartQueued = false;
    otaHeaderDrawn = false;
    strncpy(otaStatusMsg, reason, sizeof(otaStatusMsg) - 1);
    drawOtaScreen(otaChunksRcvd, otaChunkCount, otaTotalBytes, otaStatusMsg, true);
    delay(3000);
    ESP.restart();
}