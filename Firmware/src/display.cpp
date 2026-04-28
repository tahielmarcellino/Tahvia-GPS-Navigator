/**
 * display.cpp  –  v3 redesign
 *
 * Design language:
 *  - Pure white base (#FFFFFF) everywhere.
 *  - Blue-tinted map canvas (0xEF37 ≈ #EEF2FF).
 *  - Electric blue (#2D4EF5) as the single structural accent.
 *  - Vivid green (#00C07A) for "done" / connected / healthy.
 *  - Amber (#F5A623) for warnings only.
 *  - Near-black (#1A1A1A) for all primary text and borders.
 *  - Periwinkle (#5060A0) for secondary labels.
 *
 * Layout:
 *  - MAP_W × SCREEN_H map canvas (left), INFO_W panel (right).
 *  - Panel: fixed 44px header + 5 equal rows of 55px each (= 275px).
 *    (SCREEN_H 320 - 44 header - 1 border = 275px exactly.)
 *  - OTA: full-screen, giant percentage number + thin top bar.
 *  - Boot: centred wordmark only, no progress bar.
 *
 * Optimisations preserved:
 *  - 4bpp sprite (palette index 0 = map background).
 *  - Dirty-row hash cache on info panel (per-row FNV-1a).
 *  - Single pushSprite() per frame – zero flicker.
 *  - otaHeaderDrawn guard.
 *
 * Map visual changes (v3.1):
 *  - Route lines: single flat-colour pass, no shadow. done/remaining
 *    transition fixed (i-1 < nearestIdx).
 *  - Turn dots: single outline circle, no double stroke.
 *  - YOU marker: flat filled circle + single border, no aura or arrow.
 *  - Placeholder text drawn BEFORE grid so grid renders on top,
 *    eliminating the solid-bg rectangle cutting the grid lines.
 *
 * Fixes (v3.2):
 *  - Heading arrow no longer appears on the END waypoint.
 *    The end pin's teardrop triangle has been removed; it is now
 *    a red-outlined circle to clearly distinguish it from the start pin.
 *  - YOU marker no longer snaps to the final waypoint, avoiding overlap
 *    with the end pin and erroneous heading arrow on destination.
 *  - All map circles (turn dots, start pin, end pin, YOU marker) now have
 *    a thicker outline: two concentric drawCircle passes (r=9 + r=8) with
 *    a smaller fill (r=7), keeping the overall footprint identical.
 *
 * Fixes (v3.3):
 *  - Marker flicker eliminated. Previously markers (turn dots, start/end
 *    pins, YOU disc) were drawn directly to the TFT *after* pushSprite(),
 *    causing a visible two-phase flash every frame: blank-map frame then
 *    markers frame. Fix: sprite promoted to 8bpp so it accepts arbitrary
 *    RGB565 colors; all marker drawing now uses the mapFillCircle /
 *    mapDrawCircle / mapFillTriangle sprite wrappers, and mapPush() is
 *    called once at the very end with a fully-composited frame.
 *    The 4bpp path is retained as a low-memory fallback (markers drawn
 *    into sprite using nearest palette color) but 8bpp is tried first.
 */

#include "display.h"
#include "geo.h"
#include <Update.h>
#include <math.h>
#include <string.h>

// =============================================================================
// Local colour aliases
// All primary colours come from config.h via display.h.
// Only names not present in config.h are defined here.
// =============================================================================

// Route segment colours mapped to config.h names
#define C_ROUTE_REM   C_ROUTE_FILL    // remaining – blue  (config.h: C_ROUTE_FILL)
#define C_ROUTE_DONE  C_DONE_FILL     // done      – faded blue (config.h: C_DONE_FILL)

// Panel/text aliases so the drawing code below stays readable
#define C_BORDER      C_TEXT_PRIMARY  // near-black separator line
#define C_LABEL       C_TEXT_LABEL    // row label text
#define C_TEXT_PRI    C_TEXT_PRIMARY  // primary value text
#define C_ROW_DIV     C_PANEL_LINE    // subtle row divider
// C_PANEL_BG, C_BAR_TRACK, C_BLUE, C_GREEN, C_AMBER, C_AMBER_DARK,
// C_AMBER_BG → use C_CHIP_AMB_BG, C_BOOT_BG, C_RED  all from config.h directly.
#define C_AMBER_BG    C_CHIP_AMB_BG   // amber warning background

// =============================================================================
// 4bpp sprite palette – only route colours needed (markers drawn direct to TFT)
// index 0 MUST be C_MAP_BG
// =============================================================================

static const uint16_t k4bppPalette[16] = {
    C_MAP_BG,           // 0  map background
    C_ROUTE_REM,        // 1  remaining route (C_ROUTE_FILL from config.h)
    C_ROUTE_DONE,       // 2  done route (C_DONE_FILL from config.h)
    C_TEXT_PRI,         // 3  near-black fallback
    C_BLUE,             // 4
    C_GREEN,            // 5
    C_AMBER,            // 6
    C_RED,              // 7
    TFT_WHITE,          // 8
    RGB(200,200,200),   // 9  spare
    RGB(180,180,180),   // 10 spare
    RGB(150,150,150),   // 11 spare
    RGB(120,120,120),   // 12 spare
    RGB( 90, 90, 90),   // 13 spare
    RGB( 60, 60, 60),   // 14 spare
    RGB( 30, 30, 30),   // 15 spare
};

// =============================================================================
// Panel layout constants
// =============================================================================

#define PANEL_HEADER_H  44
// PANEL_ROW_H comes from config.h (48). 5 rows × 48 = 240 + 44 header + 1 border = 285.
// Last row absorbs the remaining 35px (320 - 285 = 35) via the clearRow height calc.
#define PANEL_ROWS      5

// =============================================================================
// Sprite management
// =============================================================================

void ensureSprite()
{
    if (spriteOk && mapSprite.created()) return;
    if (spriteTried)                     return;
    spriteTried = true;

    // 8bpp first: accepts arbitrary RGB565 colors so all markers (turn dots,
    // start/end pins, YOU disc, heading arrow) can be composited into the
    // sprite and pushed in a single mapPush() call – zero flicker.
    mapSprite.setColorDepth(8);
    mapSprite.createSprite(MAP_W, MAP_H);
    if (mapSprite.created()) {
        spriteOk  = true;
        spriteBpp = 8;
        Serial.printf("[SPRITE] 8bpp ok – heap=%u\n", ESP.getFreeHeap());
        return;
    }

    // 4bpp fallback: lower memory, palette-limited but still composited.
    mapSprite.setColorDepth(4);
    mapSprite.createSprite(MAP_W, MAP_H);
    if (mapSprite.created()) {
        spriteOk  = true;
        spriteBpp = 4;
        mapSprite.createPalette(k4bppPalette, 16);
        Serial.printf("[SPRITE] 4bpp fallback ok – heap=%u\n", ESP.getFreeHeap());
        return;
    }

    spriteOk  = false;
    spriteBpp = 0;
    Serial.printf("[SPRITE] alloc failed – direct TFT – heap=%u\n", ESP.getFreeHeap());
}

void releaseSprite()
{
    if (!mapSprite.created()) { spriteOk = false; return; }
    mapSprite.deleteSprite();
    spriteOk    = false;
    spriteTried = false;
    spriteBpp   = 0;
    Serial.printf("[SPRITE] freed – heap=%u\n", ESP.getFreeHeap());
}

// =============================================================================
// Shared helpers
// =============================================================================

void drawSeparator()
{
    tft.drawFastVLine(INFO_X, 0, SCREEN_H, C_BORDER);
}

// =============================================================================
// Drawing-target abstraction
// =============================================================================

static inline void mapFill(uint16_t color)
{
    if (spriteOk) {
        if (spriteBpp == 4) mapSprite.fillSprite(0);
        else                mapSprite.fillSprite(color);
    } else {
        tft.fillRect(0, 0, MAP_W, MAP_H, color);
    }
}

static inline void mapPush()
{
    if (spriteOk) mapSprite.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Primitive wrappers
// ---------------------------------------------------------------------------

static void thickLine(int x0, int y0, int x1, int y1, int w, uint16_t col)
{
    float dx  = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) {
        if (spriteOk) mapSprite.drawPixel(x0, y0, col);
        else          tft.drawPixel(x0, y0, col);
        return;
    }
    float nx = -dy / len, ny = dx / len;
    int half = w / 2;
    for (int t = -half; t <= half; t++) {
        int ax = x0 + (int)roundf(nx * t), ay = y0 + (int)roundf(ny * t);
        int bx = x1 + (int)roundf(nx * t), by = y1 + (int)roundf(ny * t);
        if (spriteOk) mapSprite.drawLine(ax, ay, bx, by, col);
        else          tft.drawLine(ax, ay, bx, by, col);
    }
}

static inline void mapFillCircle(int x, int y, int r, uint16_t c)
{
    if (spriteOk) mapSprite.fillCircle(x, y, r, c);
    else          tft.fillCircle(x, y, r, c);
}

static inline void mapDrawCircle(int x, int y, int r, uint16_t c)
{
    if (spriteOk) mapSprite.drawCircle(x, y, r, c);
    else          tft.drawCircle(x, y, r, c);
}

static inline void mapFillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c)
{
    if (spriteOk) mapSprite.fillTriangle(x0,y0,x1,y1,x2,y2,c);
    else          tft.fillTriangle(x0,y0,x1,y1,x2,y2,c);
}

static inline void mapFillRect(int x,int y,int w,int h,uint16_t c)
{
    if (spriteOk) mapSprite.fillRect(x,y,w,h,c);
    else          tft.fillRect(x,y,w,h,c);
}

static inline void mapSetTextColor(uint16_t fg)
{
    if (spriteOk) mapSprite.setTextColor(fg);
    else          tft.setTextColor(fg);
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
// Info-panel widget helpers  (always direct TFT)
// =============================================================================

static void drawProgressBar(int x, int y, int w, int h,
                             int pct, uint16_t fillCol)
{
    tft.fillRect(x, y, w, h, C_BAR_TRACK);
    int fw = (int)(w * constrain(pct, 0, 100) / 100.0f);
    if (fw > 0) tft.fillRect(x, y, fw, h, fillCol);
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

    mapFill(C_MAP_BG);

    // ── No-route placeholder ──────────────────────────────────────────────────
    if (!vp.ready || route.size() < 2) {
        mapPush();
        // Text drawn direct to TFT – full 16-bit color, no palette issues
        int cx = MAP_W / 2, cy = MAP_H / 2;
        tft.setTextDatum(MC_DATUM);
        tft.setTextSize(2);
        tft.setTextColor(C_TEXT_PRI, C_MAP_BG);
        tft.drawString("NO ROUTE", cx, cy - 8);
        tft.setTextSize(1);
        tft.setTextColor(C_LABEL, C_MAP_BG);
        tft.drawString("CONNECT APP TO SEND A ROUTE", cx, cy + 14);
        return;
    }

    // ── Route lines into sprite (only blue/green needed in palette) ───────────
    {
        int x0, y0, x1, y1;
        l2p(route[0].lat, route[0].lon, x0, y0);
        for (size_t i = 1; i < route.size(); i++) {
            l2p(route[i].lat, route[i].lon, x1, y1);
            bool done = (nearestIdx >= 0 && (int)i - 1 < nearestIdx);
            thickLine(x0, y0, x1, y1, 5, done ? C_ROUTE_DONE : C_ROUTE_REM);
            x0 = x1; y0 = y1;
        }
    }

    // ── All markers composited into the sprite BEFORE push ───────────────────
    // Drawing markers into the sprite (not directly to TFT) means mapPush()
    // sends one fully-composited frame in a single SPI burst — no flicker.

    // Turn dots — thicker outline (r=6+5), smaller fill (r=4)
    for (int idx : turnIndices) {
        if (idx < 0 || idx >= (int)route.size()) continue;
        int tx, ty;
        l2p(route[idx].lat, route[idx].lon, tx, ty);
        mapFillCircle(tx, ty, 4, TFT_WHITE);   // smaller fill
        mapDrawCircle(tx, ty, 6, C_TEXT_PRI);  // outer outline
        mapDrawCircle(tx, ty, 5, C_TEXT_PRI);  // inner outline → thick ring
    }

    // Start pin — thicker outline (r=9+8), smaller fill (r=7)
    {
        int sx, sy;
        l2p(route.front().lat, route.front().lon, sx, sy);
        mapFillCircle(sx, sy, 7, TFT_WHITE);   // smaller fill
        mapDrawCircle(sx, sy, 9, C_TEXT_PRI);  // outer outline
        mapDrawCircle(sx, sy, 8, C_TEXT_PRI);  // inner outline → thick ring
    }

    // End pin — circle only, red accent, NO teardrop/arrow.
    // Red outline clearly distinguishes destination from start (black outline).
    {
        int ex, ey;
        l2p(route.back().lat, route.back().lon, ex, ey);
        mapFillCircle(ex, ey, 7, TFT_WHITE);  // smaller fill
        mapDrawCircle(ex, ey, 9, C_RED);      // outer outline (red = destination)
        mapDrawCircle(ex, ey, 8, C_RED);      // inner outline → thick ring
    }

    // YOU marker
    if (gpsValid) {
        float drawLat = gpsLat, drawLon = gpsLon;

        // Snap to nearest waypoint, but NOT to the very last one.
        // Snapping to the final waypoint would place the heading arrow
        // directly on top of the end pin, making it look like the end
        // pin has a heading marker attached.
        if (snapped && nearestIdx >= 0 && nearestIdx < (int)route.size() - 1) {
            drawLat = route[nearestIdx].lat;
            drawLon = route[nearestIdx].lon;
        }

        int px, py;
        l2p(drawLat, drawLon, px, py);

        // Heading arrow.
        // Drawn in TWO passes before the disc so the disc sits cleanly on top:
        //   Pass 1: slightly larger white triangle  → visible border/halo
        //   Pass 2: smaller blue triangle           → arrow fill
        // Base vertices are anchored at r=5, well inside the white disc fill
        // (r=7), so the disc outline ring (r=8/9) never clips the arrow wings.
        // The tip protrudes to r=22 / r=20, well outside the disc.
        // Arrow is always drawn whenever gpsValid is true – if your GPS module
        // outputs a stale heading at standstill that is a data-layer concern,
        // not a rendering concern. Filter gpsHeading upstream if needed.
        {
            // Convert heading to radians. gpsHeading must be in degrees [0,360).
            float hRad = gpsHeading * (float)(M_PI / 180.0);

            // Pass 1 – white halo (slightly bigger triangle)
            int ax0 = px + (int)(22.f * sinf(hRad)),         ay0 = py - (int)(22.f * cosf(hRad));
            int ax1 = px + (int)(6.f  * sinf(hRad - 0.65f)), ay1 = py - (int)(6.f  * cosf(hRad - 0.65f));
            int ax2 = px + (int)(6.f  * sinf(hRad + 0.65f)), ay2 = py - (int)(6.f  * cosf(hRad + 0.65f));
            mapFillTriangle(ax0, ay0, ax1, ay1, ax2, ay2, TFT_WHITE);

            // Pass 2 – blue fill (slightly smaller triangle)
            int bx0 = px + (int)(20.f * sinf(hRad)),         by0 = py - (int)(20.f * cosf(hRad));
            int bx1 = px + (int)(5.f  * sinf(hRad - 0.55f)), by1 = py - (int)(5.f  * cosf(hRad - 0.55f));
            int bx2 = px + (int)(5.f  * sinf(hRad + 0.55f)), by2 = py - (int)(5.f  * cosf(hRad + 0.55f));
            mapFillTriangle(bx0, by0, bx1, by1, bx2, by2, C_BLUE);
        }

        // Disc — thicker outline (r=9+8), smaller fill (r=7).
        // Drawn AFTER the arrow so the disc always sits cleanly on top of the
        // arrow base, giving the appearance of the arrow growing out of the disc.
        mapFillCircle(px, py, 7, TFT_WHITE);   // white fill
        mapDrawCircle(px, py, 9, C_TEXT_PRI);  // outer outline ring
        mapDrawCircle(px, py, 8, C_TEXT_PRI);  // inner outline ring → thick border
    }

    // ── Single push: route lines + all markers in one SPI burst ──────────────
    mapPush();
}

// =============================================================================
// Info panel  – fixed-height rows, no flex layout needed
// =============================================================================
//
//  y=0            ┌──────────────────────┐
//                 │  TAHVIA  / Navigator │  44px header
//  y=44           ├──────────────────────┤
//                 │  SPEED               │  55px
//  y=99           ├──────────────────────┤
//                 │  BATTERY             │  55px
//  y=154          ├──────────────────────┤
//                 │  TRIP                │  55px
//  y=209          ├──────────────────────┤
//                 │  ROUTE               │  55px
//  y=264          ├──────────────────────┤
//                 │  BLUETOOTH           │  56px (remainder)
//  y=320          └──────────────────────┘

static uint32_t rowHash[PANEL_ROWS] = {0};

static uint32_t fnv1a(const char *s, uint16_t col)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    h ^= col; h *= 16777619u;
    return h;
}

static uint32_t fnv1aBar(const char *s, int pct, uint16_t col)
{
    uint32_t h = fnv1a(s, col);
    h ^= (uint32_t)pct; h *= 16777619u;
    return h;
}

// Row y-origin (row 0..4)
static inline int rowY(int row)
{
    return PANEL_HEADER_H + 1 + row * PANEL_ROW_H;
}

static void clearRow(int row)
{
    int y = rowY(row);
    int h = (row == PANEL_ROWS - 1) ? (SCREEN_H - y) : PANEL_ROW_H;
    tft.fillRect(INFO_X + 1, y, INFO_W - 1, h, C_PANEL_BG);
    if (row < PANEL_ROWS - 1)
        tft.drawFastHLine(INFO_X + 1, y + h, INFO_W - 1, C_ROW_DIV);
}

// Draws: periwinkle label (size-1) + primary value (size-2)
static void drawRow(int row, const char *label, const char *value,
                    uint16_t valColor = C_TEXT_PRI)
{
    uint32_t h = fnv1a(value, valColor);
    if (rowHash[row] == h) return;
    rowHash[row] = h;

    int y = rowY(row);
    clearRow(row);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_LABEL, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 10, y + 7);

    tft.setTextColor(valColor, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X + 10, y + 22);
}

static void drawRowWithBar(int row, const char *label, const char *value,
                            int pct, uint16_t barCol)
{
    uint32_t h = fnv1aBar(value, pct, barCol);
    if (rowHash[row] == h) return;
    rowHash[row] = h;

    int y = rowY(row);
    clearRow(row);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_LABEL, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 10, y + 6);

    tft.setTextColor(C_TEXT_PRI, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X + 10, y + 19);

    drawProgressBar(INFO_X + 10, y + 43, INFO_W - 18, 3, pct, barCol);
}

static void drawRowWithDot(int row, const char *label, const char *value,
                            uint16_t dotColor)
{
    uint32_t h = fnv1a(value, dotColor);
    if (rowHash[row] == h) return;
    rowHash[row] = h;

    int y = rowY(row);
    clearRow(row);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_LABEL, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 10, y + 7);

    // Status dot
    tft.fillCircle(INFO_X + 13, y + 34, 4, dotColor);

    tft.setTextColor(C_TEXT_PRI, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X + 24, y + 27);
}

void initPanel()
{
    memset(rowHash, 0, sizeof(rowHash));

    tft.fillRect(INFO_X, 0, INFO_W, SCREEN_H, C_PANEL_BG);
    drawSeparator();

    // ── Header ────────────────────────────────────────────────────────────────
    // Brand label (periwinkle, tiny caps)
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_LABEL, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString("TAHVIA", INFO_X + 10, 7);

    // Product name (near-black, size-2)
    tft.setTextColor(C_TEXT_PRI, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString("NAVIGATOR", INFO_X + 10, 20);

    // Header bottom rule
    tft.drawFastHLine(INFO_X + 1, PANEL_HEADER_H, INFO_W - 1, C_BORDER);

    // Pre-draw all row dividers so they're visible before any content update
    for (int r = 0; r < PANEL_ROWS - 1; r++) {
        int divY = rowY(r) + PANEL_ROW_H;
        tft.drawFastHLine(INFO_X + 1, divY, INFO_W - 1, C_ROW_DIV);
    }
}

void updatePanel()
{
    extern float haversineKm(float, float, float, float);
    char buf[32];

    // ── Row 0: Speed ──────────────────────────────────────────────────────────
    {
        float kmh = gpsSpeed * 3.6f;
        if (kmh < 10.0f) snprintf(buf, sizeof(buf), "%.1f KM/H", kmh);
        else              snprintf(buf, sizeof(buf), "%.0f KM/H", kmh);
        drawRow(0, "SPEED", buf);
    }

    // ── Row 1: Battery ────────────────────────────────────────────────────────
    {
        uint16_t barCol = (batPercent > 50) ? C_GREEN
                        : (batPercent > 20) ? C_AMBER
                        :                     RGB(220, 40, 40);
        snprintf(buf, sizeof(buf), "%d%%", batPercent);
        drawRowWithBar(1, "BATTERY", buf, batPercent, barCol);
    }

    // ── Row 2: Trip ───────────────────────────────────────────────────────────
    {
        if (tripKm < 1.0f) snprintf(buf, sizeof(buf), "%.0f M",   tripKm * 1000.0f);
        else                snprintf(buf, sizeof(buf), "%.2f KM",  tripKm);
        drawRow(2, "TRIP", buf);
    }

    // ── Row 3: Route ──────────────────────────────────────────────────────────
    {
        if (routeComplete && !route.empty()) {
            float doneKm = 0.0f, totalKm = 0.0f;
            for (int i = 1; i < (int)route.size(); i++) {
                float seg = haversineKm(route[i-1].lat, route[i-1].lon,
                                        route[i].lat,   route[i].lon);
                totalKm += seg;
                if (i <= nearestIdx) doneKm += seg;
            }
            int pct = (totalKm > 0) ? (int)(100.0f * doneKm / totalKm) : 0;
            if (doneKm < 1.0f)
                snprintf(buf, sizeof(buf), "%.0fM / %.1fKM",
                         doneKm * 1000.0f, totalKm);
            else
                snprintf(buf, sizeof(buf), "%.1F / %.1F KM", doneKm, totalKm);
            drawRowWithBar(3, "ROUTE", buf, pct, C_BLUE);
        } else if (!route.empty()) {
            snprintf(buf, sizeof(buf), "%d / %d", (int)route.size(), routeExpected);
            drawRow(3, "LOADING", buf, C_AMBER);
        } else {
            drawRow(3, "ROUTE", "--");
        }
    }

    // ── Row 4: Bluetooth ──────────────────────────────────────────────────────
    {
        drawRowWithDot(4, "BLUETOOTH",
                       bleCon ? "CONNECTED" : "WAITING",
                       bleCon ? C_GREEN : C_AMBER);
    }

    // ── Re-stamp all row dividers every frame ─────────────────────────────────
    // The hash cache skips clearRow when content hasn't changed, so dividers
    // can be erased by a single dirty redraw and never restored. Stamping them
    // here costs only 4 drawFastHLine calls and guarantees they're always visible.
    for (int r = 0; r < PANEL_ROWS - 1; r++) {
        int divY = rowY(r) + PANEL_ROW_H;
        tft.drawFastHLine(INFO_X + 1, divY, INFO_W - 1, C_ROW_DIV);
    }
}

// =============================================================================
// Waiting screen
// =============================================================================

void drawWaitingScreen()
{
    tft.fillRect(0, 0, MAP_W - 1, MAP_H, C_MAP_BG);

    int cx = MAP_W / 2, cy = MAP_H / 2;

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_TEXT_PRI, C_MAP_BG);
    tft.setTextSize(2);
    tft.drawString("NO CONNECTION", cx, cy - 8);

    tft.setTextSize(1);
    tft.setTextColor(C_LABEL, C_MAP_BG);
    tft.drawString("OPEN APP AND TAP CONNECT DEVICE", cx, cy + 14);

    drawSeparator();
}

// =============================================================================
// Boot screen  – pure wordmark, no progress bar
// =============================================================================

void showBootScreen()
{
    tft.fillScreen(C_BOOT_BG);

    int cx = SCREEN_W / 2;
    int cy = SCREEN_H / 2;

    // "Tah" – near-black, right-aligned to centre
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(C_TEXT_PRI, C_BOOT_BG);
    tft.setTextSize(4);
    tft.drawString("Tah", cx, cy - 8);

    // "via" – electric blue, left-aligned from centre
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_BLUE, C_BOOT_BG);
    tft.setTextSize(4);
    tft.drawString("via", cx, cy - 8);

    // Subtitle
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_LABEL, C_BOOT_BG);
    tft.setTextSize(1);
    tft.drawString("GPS NAVIGATOR", cx, cy + 20);

    // Version – bottom-right, muted
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(C_TEXT_MUTED, C_BOOT_BG);
    tft.setTextSize(1);
    tft.drawString(FW_VERSION, SCREEN_W - 8, SCREEN_H - 8);

    delay(2500);
}

// =============================================================================
// OTA screen  – bold percentage dominates, data is secondary
// =============================================================================
//
//  ┌─────────────────────────────────────────────────────┐  y=0
//  │████████████████████████░░░░░░░░░░  6px blue bar     │  y=6
//  │                                                     │
//  │   68%            CHUNKS   68 / 100                  │
//  │   FIRMWARE       RECEIVED 348 KB                    │
//  │   UPDATE                                            │
//  │                                                     │
//  ├─────────────────────────────────────────────────────┤  y=278
//  │ ● Do not power off or disconnect                    │  y=320
//  └─────────────────────────────────────────────────────┘

void drawOtaScreenHeader()
{
    tft.fillScreen(TFT_WHITE);

    // Top progress bar – 6px, blue track over pale
    tft.fillRect(0, 0, SCREEN_W, 6, C_MAP_BG);

    // Warning footer
    tft.fillRect(0, 278, SCREEN_W, 42, C_AMBER_BG);
    tft.drawFastHLine(0, 278, SCREEN_W, C_AMBER);

    // Warning dot + text
    tft.fillCircle(18, 299, 4, C_AMBER);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_AMBER_DARK, C_AMBER_BG);
    tft.setTextSize(1);
    tft.drawString("Do not power off or disconnect", 30, 299);

    // Vertical divider between pct block and data columns
    tft.drawFastVLine(210, 6, 272, C_ROW_DIV);
    tft.drawFastVLine(211, 6, 272, C_ROW_DIV);
}

void drawOtaScreenDynamic(int chunksRcvd, int chunkTotal,
                           size_t bytesWritten, const char *statusMsg, bool isError)
{
    float    pct      = (chunkTotal > 0) ? (float)chunksRcvd / chunkTotal : 0.0f;
    int      pctI     = (int)(pct * 100.0f);
    uint16_t barCol   = isError ? RGB(220, 40, 40) : C_BLUE;

    // ── Top progress bar ─────────────────────────────────────────────────────
    int barW = (int)(SCREEN_W * constrain(pct, 0.0f, 1.0f));
    tft.fillRect(0, 0, SCREEN_W, 6, C_MAP_BG);
    if (barW > 0) tft.fillRect(0, 0, barW, 6, barCol);

    // ── Left block: giant percentage ─────────────────────────────────────────
    tft.fillRect(0, 6, 210, 272, TFT_WHITE);

    // Number – 72pt equivalent via size-6 (~60px) + size-3 for % sign
    char numBuf[8], pctBuf[4];
    snprintf(numBuf, sizeof(numBuf), "%d", pctI);
    snprintf(pctBuf, sizeof(pctBuf),  "%%");

    // Measure approximate x so percent sign sits flush
    // size-6 = 6*8 = 48px per char width approx
    int numChars = strlen(numBuf);
    int numPixW  = numChars * 48;
    int totalW   = numPixW + 24;   // 24px for '%' at size-3 (18px) + gap
    int blockX   = (210 - totalW) / 2;
    int baseY    = 80;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(isError ? RGB(220,40,40) : C_TEXT_PRI, TFT_WHITE);
    tft.setTextSize(6);
    tft.drawString(numBuf, blockX, baseY);

    // Percent sign in electric blue, smaller, vertically offset
    tft.setTextColor(barCol, TFT_WHITE);
    tft.setTextSize(3);
    tft.drawString(pctBuf, blockX + numPixW + 4, baseY + 6);

    // Label beneath
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_LABEL, TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString("FIRMWARE UPDATE", blockX, baseY + 70);

    // Status message (smaller, below label)
    tft.setTextColor(isError ? RGB(220,40,40) : C_LABEL, TFT_WHITE);
    tft.drawString(statusMsg, blockX, baseY + 86);

    // ── Right block: data fields ──────────────────────────────────────────────
    tft.fillRect(213, 6, SCREEN_W - 213, 272, TFT_WHITE);

    const int rx = 228;

    // Field helper lambda (via nested struct for C++11 compat)
    struct Field {
        static void draw(int x, int y, const char *lbl, const char *val) {
            tft.setTextDatum(TL_DATUM);
            tft.setTextColor(C_LABEL, TFT_WHITE);
            tft.setTextSize(1);
            tft.drawString(lbl, x, y);
            tft.setTextColor(C_TEXT_PRI, TFT_WHITE);
            tft.setTextSize(2);
            tft.drawString(val, x, y + 14);
        }
    };

    char chunkBuf[24], bytesBuf[20];

    snprintf(chunkBuf, sizeof(chunkBuf), "%d / %d", chunksRcvd, chunkTotal);

    if (bytesWritten < 1024)
        snprintf(bytesBuf, sizeof(bytesBuf), "%u B",    (unsigned)bytesWritten);
    else if (bytesWritten < 1048576)
        snprintf(bytesBuf, sizeof(bytesBuf), "%.1f KB", bytesWritten / 1024.0f);
    else
        snprintf(bytesBuf, sizeof(bytesBuf), "%.2f MB", bytesWritten / 1048576.0f);

    Field::draw(rx, 60,  "CHUNKS",   chunkBuf);
    Field::draw(rx, 130, "RECEIVED", bytesBuf);

    // Remaining estimate
    size_t remaining = (chunkTotal > chunksRcvd && chunksRcvd > 0)
                     ? (bytesWritten / chunksRcvd) * (chunkTotal - chunksRcvd)
                     : 0;
    char remBuf[20];
    if (remaining < 1024)
        snprintf(remBuf, sizeof(remBuf), "%u B",    (unsigned)remaining);
    else if (remaining < 1048576)
        snprintf(remBuf, sizeof(remBuf), "%.1f KB", remaining / 1024.0f);
    else
        snprintf(remBuf, sizeof(remBuf), "%.2f MB", remaining / 1048576.0f);
    Field::draw(rx, 200, "REMAINING", remBuf);
}

void drawOtaScreen(int chunksRcvd, int chunkTotal,
                   size_t bytesWritten, const char *statusMsg, bool isError)
{
    if (!otaHeaderDrawn) {
        drawOtaScreenHeader();
        otaHeaderDrawn = true;
    }
    drawOtaScreenDynamic(chunksRcvd, chunkTotal, bytesWritten, statusMsg, isError);
}

// =============================================================================
// OTA abort  (CPU0 only)
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