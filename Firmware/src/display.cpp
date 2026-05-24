/**
 * display.cpp  –  v5
 *
 * Architecture (v5 — rendering fix):
 *  The screen is divided into three INDEPENDENT regions that never share a
 *  drawing target. The sprite ONLY covers the map region; it is pushed to
 *  (0, TOPBAR_H) so it physically cannot overwrite the top bar or overlay.
 *  The top bar and overlay are always drawn direct to TFT via hash-gated
 *  helpers. This eliminates the v4 bug where mapFill() wiped the top bar and
 *  overlay because the sprite covered the full screen.
 *
 *  ┌─────────────────────────────────────────┐  y=0
 *  │  TOP BAR  28px  — TFT-direct always     │
 *  ├─────────────────────────────────────────┤  y=TOPBAR_H (28)
 *  │                                         │
 *  │   MAP SPRITE  (SCREEN_W × MAP_REGION_H) │  pushed to (0, TOPBAR_H)
 *  │                                         │
 *  ├─────────────────────────────────────────┤  y=OVERLAY_Y (272)
 *  │  OVERLAY  48px  — TFT-direct always     │
 *  └─────────────────────────────────────────┘  y=320
 *
 *  Overlay cells (each 120px wide, 48px tall):
 *   [0] SPEED  |  [1] BATTERY  |  [2] ROUTE  |  [3] TRIP
 *
 * Design:
 *  - Pure black / white only — no grey, viewing-angle safe.
 *  - Route colours from config.h (C_ROUTE_FILL / C_DONE_FILL).
 *  - C_BLUE for route progress bar, C_GREEN / C_AMBER / C_RED for battery
 *    value colour only (no bar on battery cell).
 *  - Wordmark "Tah" (near-black) + "via" (blue), size-2, top-left.
 *  - BT pill — coloured rectangle, right side of top bar.
 *
 * Optimisations preserved:
 *  - 8bpp sprite preferred / 4bpp fallback / direct-TFT last resort.
 *  - FNV-1a hash cache per overlay cell and BT pill — redraws only on change.
 *  - Single mapPush() per frame — zero map flicker.
 *  - otaHeaderDrawn guard.
 */

#include "display.h"
#include "geo.h"
#include <Update.h>
#include <math.h>
#include <string.h>

// =============================================================================
// Colour aliases
// =============================================================================

#define C_ROUTE_REM   C_ROUTE_FILL   // remaining route — config.h
#define C_ROUTE_DONE  C_DONE_FILL    // done route      — config.h
#define C_AMBER_BG    C_CHIP_AMB_BG  // OTA footer background

// =============================================================================
// Layout constants
// =============================================================================

#define TOPBAR_H      28             // top bar height (px)
#define MAP_REGION_H  (SCREEN_H - TOPBAR_H - OVERLAY_H)  // sprite height
#define OVERLAY_H     48             // overlay panel height (px)
#define OVERLAY_Y     (SCREEN_H - OVERLAY_H)              // overlay top edge = 272
#define CELL_W        120            // each of the 4 cells (480/4)
#define CELL_PAD      10             // left/right padding inside cell
#define OVERLAY_CELLS 4

#define CELL_X(n)     ((n) * CELL_W)

// Cell interior dimensions (text layout)
// Label  size-1  y+4   (8px)
// Value  size-3  y+14  (24px, bottom at y+38)
// unit sits inline right of value at y+28 (baseline)
// No progress bar on any cell except ROUTE

#define BAR_H         3

// =============================================================================
// 4bpp sprite palette
// =============================================================================

static const uint16_t k4bppPalette[16] = {
    C_MAP_BG,          // 0
    C_ROUTE_REM,       // 1
    C_ROUTE_DONE,      // 2
    C_TEXT_PRIMARY,    // 3
    C_BLUE,            // 4
    C_GREEN,           // 5
    C_AMBER,           // 6
    C_RED,             // 7
    TFT_WHITE,         // 8
    C_TEXT_PRIMARY,    // 9-15 spare
    C_TEXT_PRIMARY, C_TEXT_PRIMARY, C_TEXT_PRIMARY,
    C_TEXT_PRIMARY, C_TEXT_PRIMARY, C_TEXT_PRIMARY,
};

// =============================================================================
// Sprite management — MAP REGION ONLY
// Sprite is always SCREEN_W × MAP_REGION_H and pushed to (0, TOPBAR_H).
// It cannot physically reach the top bar or overlay.
// =============================================================================

void ensureSprite()
{
    if (spriteOk && mapSprite.created()) return;
    if (spriteTried)                     return;
    spriteTried = true;

    mapSprite.setColorDepth(8);
    mapSprite.createSprite(SCREEN_W, MAP_REGION_H);
    if (mapSprite.created()) {
        spriteOk  = true;
        spriteBpp = 8;
        Serial.printf("[SPRITE] 8bpp %dx%d ok heap=%u\n",
                      SCREEN_W, MAP_REGION_H, ESP.getFreeHeap());
        return;
    }

    mapSprite.setColorDepth(4);
    mapSprite.createSprite(SCREEN_W, MAP_REGION_H);
    if (mapSprite.created()) {
        spriteOk  = true;
        spriteBpp = 4;
        mapSprite.createPalette(k4bppPalette, 16);
        Serial.printf("[SPRITE] 4bpp %dx%d ok heap=%u\n",
                      SCREEN_W, MAP_REGION_H, ESP.getFreeHeap());
        return;
    }

    spriteOk  = false;
    spriteBpp = 0;
    Serial.printf("[SPRITE] alloc failed direct-TFT heap=%u\n", ESP.getFreeHeap());
}

void releaseSprite()
{
    if (!mapSprite.created()) { spriteOk = false; return; }
    mapSprite.deleteSprite();
    spriteOk    = false;
    spriteTried = false;
    spriteBpp   = 0;
}

// =============================================================================
// Map drawing-target abstraction
// Coordinate space: (0,0) = top-left of MAP REGION (below top bar).
// Caller always works in map-region coords; mapPush() offsets to (0, TOPBAR_H).
// =============================================================================

static inline void mapFill(uint16_t color)
{
    if (spriteOk) {
        if (spriteBpp == 4) mapSprite.fillSprite(0);
        else                mapSprite.fillSprite(color);
    } else {
        tft.fillRect(0, TOPBAR_H, SCREEN_W, MAP_REGION_H, color);
    }
}

static inline void mapPush()
{
    // Push sprite to (0, TOPBAR_H) — never touches top bar or overlay.
    if (spriteOk) mapSprite.pushSprite(0, TOPBAR_H);
}

// ---------------------------------------------------------------------------
// Map primitives — sprite-or-TFT, always in map-region coords
// TFT fallback adds TOPBAR_H to y so everything lands in the right place.
// ---------------------------------------------------------------------------

static void thickLine(int x0, int y0, int x1, int y1, int w, uint16_t col)
{
    float dx = x1-x0, dy = y1-y0, len = sqrtf(dx*dx+dy*dy);
    if (len < 0.5f) {
        if (spriteOk) mapSprite.drawPixel(x0, y0, col);
        else          tft.drawPixel(x0, y0+TOPBAR_H, col);
        return;
    }
    float nx = -dy/len, ny = dx/len;
    int half = w/2;
    for (int t = -half; t <= half; t++) {
        int ax = x0+(int)roundf(nx*t), ay = y0+(int)roundf(ny*t);
        int bx = x1+(int)roundf(nx*t), by = y1+(int)roundf(ny*t);
        if (spriteOk) mapSprite.drawLine(ax,ay,bx,by,col);
        else          tft.drawLine(ax,ay+TOPBAR_H,bx,by+TOPBAR_H,col);
    }
}

#define MAP_PRIM(sprite_call, tft_call_y_offset) \
    if (spriteOk) { sprite_call; } else { tft_call_y_offset; }

static inline void mapFillCircle(int x,int y,int r,uint16_t c)
{ MAP_PRIM(mapSprite.fillCircle(x,y,r,c), tft.fillCircle(x,y+TOPBAR_H,r,c)) }

static inline void mapDrawCircle(int x,int y,int r,uint16_t c)
{ MAP_PRIM(mapSprite.drawCircle(x,y,r,c), tft.drawCircle(x,y+TOPBAR_H,r,c)) }

static inline void mapFillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c)
{ MAP_PRIM(mapSprite.fillTriangle(x0,y0,x1,y1,x2,y2,c),
           tft.fillTriangle(x0,y0+TOPBAR_H,x1,y1+TOPBAR_H,x2,y2+TOPBAR_H,c)) }

static inline void mapFillRect(int x,int y,int w,int h,uint16_t c)
{ MAP_PRIM(mapSprite.fillRect(x,y,w,h,c), tft.fillRect(x,y+TOPBAR_H,w,h,c)) }

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

    // ── No-route placeholder ─────────────────────────────────────────────────
    if (!vp.ready || route.size() < 2) {
        mapPush();
        int cx = SCREEN_W / 2;
        int cy = MAP_REGION_H / 2;
        tft.setTextDatum(MC_DATUM);
        tft.setTextSize(2);
        tft.setTextColor(C_TEXT_PRIMARY, C_MAP_BG);
        tft.drawString("NO ROUTE", cx, TOPBAR_H + cy - 10);
        tft.setTextSize(1);
        tft.drawString("CONNECT APP TO SEND A ROUTE", cx, TOPBAR_H + cy + 14);
        return;
    }

    // ── Route lines ──────────────────────────────────────────────────────────
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

    // ── Markers ──────────────────────────────────────────────────────────────

    for (int idx : turnIndices) {
        if (idx < 0 || idx >= (int)route.size()) continue;
        int tx, ty; l2p(route[idx].lat, route[idx].lon, tx, ty);
        mapFillCircle(tx, ty, 4, TFT_WHITE);
        mapDrawCircle(tx, ty, 6, C_TEXT_PRIMARY);
        mapDrawCircle(tx, ty, 5, C_TEXT_PRIMARY);
    }

    {
        int sx, sy; l2p(route.front().lat, route.front().lon, sx, sy);
        mapFillCircle(sx, sy, 7, TFT_WHITE);
        mapDrawCircle(sx, sy, 9, C_TEXT_PRIMARY);
        mapDrawCircle(sx, sy, 8, C_TEXT_PRIMARY);
    }

    {
        int ex, ey; l2p(route.back().lat, route.back().lon, ex, ey);
        mapFillCircle(ex, ey, 7, TFT_WHITE);
        mapDrawCircle(ex, ey, 9, C_RED);
        mapDrawCircle(ex, ey, 8, C_RED);
    }

    if (gpsValid) {
        float drawLat = gpsLat, drawLon = gpsLon;
        if (snapped && nearestIdx >= 0 && nearestIdx < (int)route.size() - 1) {
            drawLat = route[nearestIdx].lat;
            drawLon = route[nearestIdx].lon;
        }
        int px, py; l2p(drawLat, drawLon, px, py);

        float hRad = gpsHeading * (float)(M_PI / 180.0);
        mapFillTriangle(
            px+(int)(22.f*sinf(hRad)),         py-(int)(22.f*cosf(hRad)),
            px+(int)(6.f *sinf(hRad-0.65f)),   py-(int)(6.f *cosf(hRad-0.65f)),
            px+(int)(6.f *sinf(hRad+0.65f)),   py-(int)(6.f *cosf(hRad+0.65f)),
            TFT_WHITE);
        mapFillTriangle(
            px+(int)(20.f*sinf(hRad)),         py-(int)(20.f*cosf(hRad)),
            px+(int)(5.f *sinf(hRad-0.55f)),   py-(int)(5.f *cosf(hRad-0.55f)),
            px+(int)(5.f *sinf(hRad+0.55f)),   py-(int)(5.f *cosf(hRad+0.55f)),
            C_BLUE);

        mapFillCircle(px, py, 7, TFT_WHITE);
        mapDrawCircle(px, py, 9, C_TEXT_PRIMARY);
        mapDrawCircle(px, py, 8, C_TEXT_PRIMARY);
    }

    mapPush();  // single SPI burst for the entire map region
}

// =============================================================================
// Top bar — TFT-direct, drawn once at init; BT pill refreshes each frame
// =============================================================================

static uint16_t lastBtCol = 0xFFFF;

static void _drawBtPill()
{
    uint16_t pillCol = bleCon ? C_GREEN : C_AMBER;
    if (lastBtCol == pillCol) return;
    lastBtCol = pillCol;

    const char *label = bleCon ? "CONNECTED" : "WAITING";
    int px = SCREEN_W - 96, py = 5, pw = 88, ph = 18;
    tft.fillRect(px, py, pw, ph, pillCol);
    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, pillCol);
    tft.drawString(label, px + 7, py + 9);
}

// =============================================================================
// Overlay — TFT-direct, hash-gated per cell
// =============================================================================

static uint32_t cellHash[OVERLAY_CELLS] = {0};

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

// Draw one overlay cell, TFT-direct.
// pct < 0  -> no progress bar.
// Layout inside 48px cell:
//   +4   label  size-1  (8px)
//   +14  value  size-3  (24px tall, baseline at +38)
//   unit inline right of value, size-1, y=+28 (mid-cap height)
//   +42  bar    3px     (only when pct >= 0)
static void _drawCell(int cell, const char *label, const char *value,
                      const char *unit, uint16_t valCol,
                      int pct, uint16_t barCol)
{
    uint32_t h = (pct >= 0) ? fnv1aBar(value, pct, valCol)
                             : fnv1a(value, valCol);
    if (cellHash[cell] == h) return;
    cellHash[cell] = h;

    int x  = CELL_X(cell);
    int cx = x + CELL_PAD;
    int cy = OVERLAY_Y;

    // Clear cell interior (leave border pixels intact)
    tft.fillRect(x + 1, cy + 1, CELL_W - 1, OVERLAY_H - 1, TFT_WHITE);

    // Label
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_TEXT_PRIMARY, TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString(label, cx, cy + 4);

    // Value
    tft.setTextColor(valCol, TFT_WHITE);
    tft.setTextSize(3);
    tft.drawString(value, cx, cy + 14);

    // Unit — inline, right of value
    if (unit && unit[0]) {
        // size-3 glyph: 6px base * 3 = 18px per char
        int unitX = cx + (int)strlen(value) * 18 + 3;
        tft.setTextColor(C_TEXT_PRIMARY, TFT_WHITE);
        tft.setTextSize(1);
        tft.drawString(unit, unitX, cy + 28);
    }

    // Progress bar (ROUTE cell only)
    if (pct >= 0) {
        int bx = cx, by = cy + 42, bw = CELL_W - CELL_PAD * 2;
        tft.fillRect(bx, by, bw, BAR_H, TFT_WHITE);
        tft.drawRect(bx, by, bw, BAR_H, C_TEXT_PRIMARY);
        int fw = (int)(bw * constrain(pct, 0, 100) / 100.0f);
        if (fw > 0) tft.fillRect(bx, by, fw, BAR_H, barCol);
    }
}

// Re-stamp the chrome lines (top border + vertical dividers).
// Called every frame — cheap, ensures a dirty cell redraw never eats them.
static void _stampOverlayChrome()
{
    tft.drawFastHLine(0, OVERLAY_Y, SCREEN_W, C_TEXT_PRIMARY);
    for (int n = 1; n < OVERLAY_CELLS; n++)
        tft.drawFastVLine(CELL_X(n), OVERLAY_Y, OVERLAY_H, C_TEXT_PRIMARY);
}

// =============================================================================
// initPanel — called once at boot (and from waiting/no-route screens)
// =============================================================================

void initPanel()
{
    memset(cellHash, 0, sizeof(cellHash));
    lastBtCol = 0xFFFF;  // force BT pill redraw

    // Top bar
    tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, TFT_WHITE);
    tft.drawFastHLine(0, TOPBAR_H - 1, SCREEN_W, C_TEXT_PRIMARY);

    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(2);
    tft.setTextColor(C_TEXT_PRIMARY, TFT_WHITE);
    tft.drawString("Tah", 12, 7);
    tft.setTextColor(C_BLUE, TFT_WHITE);
    tft.drawString("via", 12 + 24, 7);

    // Overlay background + chrome
    tft.fillRect(0, OVERLAY_Y, SCREEN_W, OVERLAY_H, TFT_WHITE);
    _stampOverlayChrome();
}

// =============================================================================
// updatePanel — called every frame from the main loop
// =============================================================================

void updatePanel()
{
    extern float haversineKm(float, float, float, float);

    // BT pill (hash-gated inside _drawBtPill)
    _drawBtPill();

    char buf[32];

    // Cell 0: SPEED
    {
        float kmh = gpsSpeed * 3.6f;
        if (kmh < 10.0f) snprintf(buf, sizeof(buf), "%.1f", kmh);
        else              snprintf(buf, sizeof(buf), "%.0f", kmh);
        _drawCell(0, "SPEED", buf, "KM/H", C_TEXT_PRIMARY, -1, 0);
    }

    // Cell 1: BATTERY — value colour reflects charge level, no bar
    {
        uint16_t col = (batPercent > 50) ? C_GREEN
                     : (batPercent > 20) ? C_AMBER
                     :                     C_RED;
        snprintf(buf, sizeof(buf), "%d", batPercent);
        _drawCell(1, "BATTERY", buf, "%", col, -1, 0);
    }

    // Cell 2: ROUTE
    // Value = remaining distance only (max 4 chars at size-3 = 72px, fits 100px usable).
    // Progress bar shows overall completion. Label = "REMAIN" when active.
    {
        if (routeComplete && !route.empty()) {
            float doneKm = 0.0f, totalKm = 0.0f;
            for (int i = 1; i < (int)route.size(); i++) {
                float seg = haversineKm(route[i-1].lat, route[i-1].lon,
                                        route[i].lat,   route[i].lon);
                totalKm += seg;
                if (i <= nearestIdx) doneKm += seg;
            }
            float remKm = totalKm - doneKm;
            int   pct   = (totalKm > 0) ? (int)(100.0f * doneKm / totalKm) : 0;
            // <=4 chars in buf; unit at size-1 inline
            const char *unit;
            if (remKm < 1.0f) {
                snprintf(buf, sizeof(buf), "%.0f", remKm * 1000.0f);
                unit = "M";
            } else if (remKm < 10.0f) {
                snprintf(buf, sizeof(buf), "%.1f", remKm);
                unit = "KM";
            } else {
                snprintf(buf, sizeof(buf), "%.0f", remKm);
                unit = "KM";
            }
            _drawCell(2, "REMAIN", buf, unit, C_TEXT_PRIMARY, pct, C_BLUE);
        } else if (!route.empty()) {
            // Loading: show % of waypoints received
            int pct = (routeExpected > 0)
                    ? (int)(100.0f * route.size() / routeExpected) : 0;
            snprintf(buf, sizeof(buf), "%d", pct);
            _drawCell(2, "LOADING", buf, "%", C_AMBER, -1, 0);
        } else {
            _drawCell(2, "ROUTE", "--", "", C_TEXT_PRIMARY, -1, 0);
        }
    }

    // Cell 3: TRIP
    {
        if (tripKm < 1.0f) snprintf(buf, sizeof(buf), "%.0f", tripKm * 1000.0f);
        else                snprintf(buf, sizeof(buf), "%.2f", tripKm);
        const char *unit = (tripKm < 1.0f) ? "M" : "KM";
        _drawCell(3, "TRIP", buf, unit, C_TEXT_PRIMARY, -1, 0);
    }

    _stampOverlayChrome();
}

// =============================================================================
// Waiting screen
// =============================================================================

void drawWaitingScreen()
{
    tft.fillRect(0, TOPBAR_H, SCREEN_W, MAP_REGION_H, C_MAP_BG);

    int cx = SCREEN_W / 2;
    int cy = TOPBAR_H + MAP_REGION_H / 2;

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_TEXT_PRIMARY, C_MAP_BG);
    tft.setTextSize(2);
    tft.drawString("NO CONNECTION", cx, cy - 10);
    tft.setTextSize(1);
    tft.drawString("OPEN APP AND TAP CONNECT DEVICE", cx, cy + 14);

    initPanel();
}

// =============================================================================
// Boot screen — wordmark + max-value sweep
// =============================================================================

void showBootScreen()
{
    tft.fillScreen(TFT_WHITE);

    int cx = SCREEN_W / 2, cy = SCREEN_H / 2;

    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(C_TEXT_PRIMARY, TFT_WHITE);
    tft.setTextSize(4);
    tft.drawString("Tah", cx, cy - 8);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_BLUE, TFT_WHITE);
    tft.setTextSize(4);
    tft.drawString("via", cx, cy - 8);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_TEXT_PRIMARY, TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString("GPS NAVIGATOR", cx, cy + 20);

    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(C_TEXT_PRIMARY, TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString(FW_VERSION, SCREEN_W - 8, SCREEN_H - 8);

    delay(2500);

    // ── Max-value instrument sweep ────────────────────────────────────────────
    // Draw overlay chrome then flash max values for 2 s, then zero for 300 ms.
    // Uses _drawCell directly (sprite not yet initialised).

    tft.fillRect(0, OVERLAY_Y, SCREEN_W, OVERLAY_H, TFT_WHITE);
    tft.drawFastHLine(0, OVERLAY_Y, SCREEN_W, C_TEXT_PRIMARY);
    for (int n = 1; n < OVERLAY_CELLS; n++)
        tft.drawFastVLine(CELL_X(n), OVERLAY_Y, OVERLAY_H, C_TEXT_PRIMARY);

    // Force hash miss so every cell draws
    memset(cellHash, 0xFF, sizeof(cellHash));

    // Max
    _drawCell(0, "SPEED",   "999",  "KM/H", C_TEXT_PRIMARY, -1, 0);
    _drawCell(1, "BATTERY", "100",  "%",     C_GREEN,        -1, 0);
    _drawCell(2, "ROUTE",   "99.9", "KM",    C_TEXT_PRIMARY, 100, C_BLUE);
    _drawCell(3, "TRIP",    "999",  "KM",    C_TEXT_PRIMARY, -1, 0);
    delay(2000);

    // Zero
    memset(cellHash, 0xFF, sizeof(cellHash));
    _drawCell(0, "SPEED",   "0",  "KM/H", C_TEXT_PRIMARY, -1, 0);
    _drawCell(1, "BATTERY", "0",  "%",     C_RED,          -1, 0);
    _drawCell(2, "ROUTE",   "--", "",      C_TEXT_PRIMARY,  0, C_BLUE);
    _drawCell(3, "TRIP",    "0",  "KM",    C_TEXT_PRIMARY, -1, 0);
    delay(300);
}

// =============================================================================
// OTA screen
// =============================================================================

void drawOtaScreenHeader()
{
    tft.fillScreen(TFT_WHITE);
    tft.drawRect(0, 0, SCREEN_W, 6, C_TEXT_PRIMARY);

    tft.fillRect(0, 278, SCREEN_W, 42, C_AMBER_BG);
    tft.drawFastHLine(0, 278, SCREEN_W, C_AMBER);
    tft.fillCircle(18, 299, 4, C_AMBER);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_AMBER_DARK, C_AMBER_BG);
    tft.setTextSize(1);
    tft.drawString("Do not power off or disconnect", 30, 299);

    tft.drawFastVLine(210, 6, 272, C_TEXT_PRIMARY);
}

void drawOtaScreenDynamic(int chunksRcvd, int chunkTotal,
                           size_t bytesWritten, const char *statusMsg, bool isError)
{
    float    pct    = (chunkTotal > 0) ? (float)chunksRcvd / chunkTotal : 0.0f;
    int      pctI   = (int)(pct * 100.0f);
    uint16_t barCol = isError ? C_RED : C_BLUE;

    // Progress bar
    int barW = (int)(SCREEN_W * constrain(pct, 0.0f, 1.0f));
    tft.fillRect(1, 1, SCREEN_W - 2, 4, TFT_WHITE);
    if (barW > 2) tft.fillRect(1, 1, barW - 2, 4, barCol);

    // Left: giant percentage
    tft.fillRect(0, 6, 210, 272, TFT_WHITE);
    char numBuf[8]; snprintf(numBuf, sizeof(numBuf), "%d", pctI);
    int numPixW = (int)strlen(numBuf) * 48;
    int blockX  = (210 - (numPixW + 24)) / 2;
    int baseY   = 80;

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(isError ? C_RED : C_TEXT_PRIMARY, TFT_WHITE);
    tft.setTextSize(6);
    tft.drawString(numBuf, blockX, baseY);
    tft.setTextColor(barCol, TFT_WHITE);
    tft.setTextSize(3);
    tft.drawString("%", blockX + numPixW + 4, baseY + 6);

    tft.setTextColor(C_TEXT_PRIMARY, TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString("FIRMWARE UPDATE", blockX, baseY + 70);
    tft.setTextColor(isError ? C_RED : C_TEXT_PRIMARY, TFT_WHITE);
    tft.drawString(statusMsg, blockX, baseY + 86);

    // Right: data fields
    tft.fillRect(213, 6, SCREEN_W - 213, 272, TFT_WHITE);
    const int rx = 228;

    struct Field {
        static void draw(int x, int y, const char *lbl, const char *val) {
            tft.setTextDatum(TL_DATUM);
            tft.setTextColor(C_TEXT_PRIMARY, TFT_WHITE);
            tft.setTextSize(1); tft.drawString(lbl, x, y);
            tft.setTextSize(2); tft.drawString(val, x, y + 14);
        }
    };

    char chunkBuf[24], bytesBuf[20], remBuf[20];
    snprintf(chunkBuf, sizeof(chunkBuf), "%d / %d", chunksRcvd, chunkTotal);

    auto fmtBytes = [](char *b, size_t sz, size_t bytes) {
        if      (bytes < 1024)    snprintf(b, sz, "%u B",    (unsigned)bytes);
        else if (bytes < 1048576) snprintf(b, sz, "%.1f KB", bytes/1024.0f);
        else                      snprintf(b, sz, "%.2f MB", bytes/1048576.0f);
    };
    fmtBytes(bytesBuf, sizeof(bytesBuf), bytesWritten);

    size_t remaining = (chunkTotal > chunksRcvd && chunksRcvd > 0)
                     ? (bytesWritten / chunksRcvd) * (chunkTotal - chunksRcvd) : 0;
    fmtBytes(remBuf, sizeof(remBuf), remaining);

    Field::draw(rx,  60, "CHUNKS",    chunkBuf);
    Field::draw(rx, 130, "RECEIVED",  bytesBuf);
    Field::draw(rx, 200, "REMAINING", remBuf);
}

void drawOtaScreen(int chunksRcvd, int chunkTotal,
                   size_t bytesWritten, const char *statusMsg, bool isError)
{
    if (!otaHeaderDrawn) { drawOtaScreenHeader(); otaHeaderDrawn = true; }
    drawOtaScreenDynamic(chunksRcvd, chunkTotal, bytesWritten, statusMsg, isError);
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