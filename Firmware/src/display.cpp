/**
 * display.cpp  –  REDESIGNED
 * White-theme, high-contrast, surgical-minimal aesthetic.
 *
 * Design principles:
 *  - Pure white base (#FFFF) everywhere, NO grays.
 *  - Electric cobalt blue (#0055FF) as the single accent.
 *  - Semantic colors: vivid green, amber, red – never muted.
 *  - Info-panel rows use a bold left-border accent strip instead
 *    of filled backgrounds so the white theme breathes.
 *  - Route: thick cobalt shadow + pure white fill on undone segments,
 *    vivid teal on done segments – always readable over white map.
 *  - OTA: white base, oversized ring, crisp data column.
 *  - Boot: large split wordmark with hard blue/black contrast.
 *
 * Optimisations preserved from original:
 *  - 4bpp sprite (palette index 0 = map background for mapFill).
 *  - Dirty-row hash cache on info panel.
 *  - Single pushSprite() per frame – zero flicker.
 *  - otaHeaderDrawn guard for OTA screen.
 */

#include "display.h"
#include "geo.h"
#include <Update.h>
#include <math.h>
#include <string.h>

// =============================================================================
// Color palette  –  white theme, vivid accents, zero grays
// =============================================================================

// Map / route
#define C_MAP_BG        TFT_WHITE           // pure white map canvas
#define C_ROUTE_SHAD    RGB(  0,  60, 220)  // cobalt shadow (dark blue)
#define C_ROUTE_FILL    RGB(255, 255, 255)  // white fill on undone route
#define C_DONE_SHAD     RGB(  0, 160, 120)  // teal shadow (done)
#define C_DONE_FILL     RGB( 80, 230, 190)  // bright teal fill (done)
#define C_TURN_DOT      RGB(  0,  55, 220)  // cobalt dot at turn
#define C_START         RGB( 20, 200,  80)  // vivid green start pin
#define C_END           RGB(230,  30,  30)  // vivid red end pin
#define C_YOU_PULSE     RGB(200, 220, 255)  // light blue aura
#define C_YOU_RING      RGB(  0,  55, 220)  // cobalt ring
#define C_YOU_FILL      TFT_WHITE           // white centre dot

// Panel structural
#define C_PANEL_BG      TFT_WHITE
#define C_PANEL_LINE    RGB(  0,  55, 220)  // cobalt divider lines
#define C_PANEL_ACCENT  RGB(  0,  55, 220)  // left-border accent strip

// Semantic status
#define C_BLUE          RGB(  0,  55, 220)
#define C_GREEN         RGB( 10, 190,  60)
#define C_AMBER         RGB(240, 130,   0)
#define C_RED           RGB(210,  20,  20)

// Progress / bar
#define C_BAR_TRACK     RGB(210, 225, 255)  // pale blue track (white-themed)

// Chip backgrounds  (vivid tint, high contrast with colored text)
#define C_CHIP_BLUE_BG  RGB(210, 225, 255)
#define C_CHIP_GREEN_BG RGB(200, 245, 215)
#define C_CHIP_RED_BG   RGB(255, 210, 210)
#define C_CHIP_AMBER_BG RGB(255, 235, 200)

// Boot
#define C_BOOT_BG       TFT_WHITE

// =============================================================================
// 4bpp palette  –  index 0 MUST be C_MAP_BG
// =============================================================================

static const uint16_t k4bppPalette[16] = {
    C_MAP_BG,               // 0  ← mapFill passes index 0 in 4bpp mode
    C_ROUTE_SHAD,           // 1
    C_ROUTE_FILL,           // 2
    C_DONE_SHAD,            // 3
    C_DONE_FILL,            // 4
    C_TURN_DOT,             // 5
    C_START,                // 6
    C_END,                  // 7
    TFT_WHITE,              // 8
    RGB(180,  40,  30),     // 9  – end-pin dark ring
    C_YOU_PULSE,            // 10
    C_YOU_RING,             // 11
    C_YOU_FILL,             // 12
    C_BLUE,                 // 13
    TFT_BLACK,              // 14
    C_PANEL_BG,             // 15
};

// =============================================================================
// Sprite management  (internal heap, no PSRAM)
// =============================================================================

void ensureSprite()
{
    if (spriteOk && mapSprite.created()) return;
    if (spriteTried)                     return;
    spriteTried = true;

    // 4bpp attempt
    mapSprite.setColorDepth(4);
    mapSprite.createSprite(MAP_W, MAP_H);
    if (mapSprite.created()) {
        spriteOk  = true;
        spriteBpp = 4;
        mapSprite.createPalette(k4bppPalette, 16);
        Serial.printf("[SPRITE] 4bpp ok – heap=%u\n", ESP.getFreeHeap());
        return;
    }

    // 8bpp fallback
    mapSprite.setColorDepth(8);
    mapSprite.createSprite(MAP_W, MAP_H);
    if (mapSprite.created()) {
        spriteOk  = true;
        spriteBpp = 8;
        Serial.printf("[SPRITE] 8bpp ok (4bpp failed) – heap=%u\n", ESP.getFreeHeap());
        return;
    }

    // Direct TFT draw
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
    Serial.printf("[SPRITE] freed for OTA – heap=%u\n", ESP.getFreeHeap());
}

// =============================================================================
// Shared layout helpers
// =============================================================================

void drawSeparator()
{
    // Bold 2-pixel cobalt vertical divider
    tft.drawFastVLine(INFO_X,     0, SCREEN_H, C_PANEL_LINE);
    tft.drawFastVLine(INFO_X + 1, 0, SCREEN_H, C_PANEL_LINE);
}

// =============================================================================
// Drawing-target abstraction  (sprite → TFT fallback)
// =============================================================================

static inline void mapFill(uint16_t color)
{
    if (spriteOk) {
        // In 4bpp mode fillSprite needs a palette INDEX; index 0 = C_MAP_BG
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

static inline void mapDrawCircle(int x, int y, int r, uint16_t c)
{
    if (spriteOk) mapSprite.drawCircle(x, y, r, c);
    else          tft.drawCircle(x, y, r, c);
}

static inline void mapFillTriangle(int x0, int y0,
                                    int x1, int y1,
                                    int x2, int y2, uint16_t c)
{
    if (spriteOk) mapSprite.fillTriangle(x0, y0, x1, y1, x2, y2, c);
    else          tft.fillTriangle(x0, y0, x1, y1, x2, y2, c);
}

static inline void mapFillRect(int x, int y, int w, int h, uint16_t c)
{
    if (spriteOk) mapSprite.fillRect(x, y, w, h, c);
    else          tft.fillRect(x, y, w, h, c);
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
// UI widget helpers  (info panel – always direct TFT)
// =============================================================================

static inline void tftRoundRect(int x, int y, int w, int h, int r, uint16_t col)
{
    tft.fillRoundRect(x, y, w, h, r, col);
}

// Progress bar: white-theme track, vivid fill, sharp corners → rectangular
static void drawProgressBar(int x, int y, int w, int h,
                             int pct, uint16_t fillCol, uint16_t trackCol)
{
    // Track
    tft.fillRect(x, y, w, h, trackCol);
    // Fill
    int fillW = (int)(w * constrain(pct, 0, 100) / 100.0f);
    if (fillW > 0) tft.fillRect(x, y, fillW, h, fillCol);
    // Border outline – cobalt, 1px
    tft.drawRect(x, y, w, h, C_PANEL_LINE);
}

// Chip: rounded rectangle badge
static void drawChip(int x, int y, int w, int h, int r,
                     uint16_t bgCol, uint16_t txtCol,
                     const char *text, uint8_t sz = 1)
{
    tftRoundRect(x, y, w, h, r, bgCol);
    // Outline for crispness
    tft.drawRoundRect(x, y, w, h, r, txtCol);
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

    mapFill(C_MAP_BG);

    // ── No-route placeholder ─────────────────────────────────────────────────
    if (!vp.ready || route.size() < 2) {
        int cx = MAP_W / 2, cy = MAP_H / 2;

        // Compass rose decoration – concentric rings in cobalt
        mapFillCircle(cx, cy - 26, 22, C_BLUE);
        mapFillCircle(cx, cy - 26, 17, TFT_WHITE);
        mapFillCircle(cx, cy - 26, 12, C_BLUE);
        mapFillCircle(cx, cy - 26,  7, TFT_WHITE);
        mapFillCircle(cx, cy - 26,  3, C_BLUE);

        // Cardinal tick marks
        for (int deg = 0; deg < 360; deg += 90) {
            float rad = deg * DEG_TO_RAD;
            int tx = cx + (int)(19 * sinf(rad));
            int ty = (cy - 26) - (int)(19 * cosf(rad));
            mapFillCircle(tx, ty, 2, TFT_WHITE);
        }

        // Bold divider line under text
        mapFillRect(cx - 70, cy + 0, 140, 2, C_BLUE);

        mapSetTextColor(TFT_BLACK, TFT_WHITE);
        mapSetTextDatum(MC_DATUM);
        mapSetTextSize(2);
        mapDrawString("NO ROUTE LOADED", cx, cy + 12);
        mapSetTextSize(1);
        mapSetTextColor(C_BLUE, TFT_WHITE);
        mapDrawString("CONNECT APP TO SEND A ROUTE", cx, cy + 32);
        mapPush();
        return;
    }

    // ── Route – 2 passes: shadow (thick) then fill (thinner) ─────────────────
    for (int pass = 0; pass < 2; pass++) {
        int  lw = (pass == 0) ? 9 : 5;   // wider shadow for bold look on white
        int  x0, y0, x1, y1;
        l2p(route[0].lat, route[0].lon, x0, y0);
        for (size_t i = 1; i < route.size(); i++) {
            l2p(route[i].lat, route[i].lon, x1, y1);
            bool     done = (nearestIdx >= 0 && (int)i <= nearestIdx);
            uint16_t col;
            if (pass == 0) col = done ? C_DONE_SHAD  : C_ROUTE_SHAD;
            else           col = done ? C_DONE_FILL   : C_ROUTE_FILL;
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
        mapFillCircle(tx, ty, 8, done ? C_DONE_SHAD  : C_ROUTE_SHAD);
        mapFillCircle(tx, ty, 5, done ? C_DONE_FILL   : TFT_WHITE);
        mapFillCircle(tx, ty, 3, C_TURN_DOT);
    }

    // ── Start dot  (vivid green, white centre) ────────────────────────────────
    {
        int sx, sy;
        l2p(route.front().lat, route.front().lon, sx, sy);
        mapFillCircle(sx, sy, 10, TFT_BLACK);   // hard black outer ring
        mapFillCircle(sx, sy,  8, C_START);
        mapFillCircle(sx, sy,  3, TFT_WHITE);
    }

    // ── End pin  (vivid red teardrop) ─────────────────────────────────────────
    {
        int ex, ey;
        l2p(route.back().lat, route.back().lon, ex, ey);
        // Outer black ring for contrast on white map
        mapFillCircle(ex, ey - 5, 10, TFT_BLACK);
        mapFillCircle(ex, ey - 5,  8, C_END);
        mapFillCircle(ex, ey - 5,  3, TFT_WHITE);
        // Pin tip
        mapFillTriangle(ex - 5, ey + 1, ex + 5, ey + 1, ex, ey + 10, TFT_BLACK);
        mapFillTriangle(ex - 3, ey + 1, ex + 3, ey + 1, ex, ey + 7,  C_END);
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

        // Pulse aura
        mapFillCircle(px, py, 20, C_YOU_PULSE);

        // Heading arrow (cobalt fill, black outline)
        float hRad = gpsHeading * DEG_TO_RAD;
        int ctx = px + (int)(30 * sinf(hRad));
        int cty = py - (int)(30 * cosf(hRad));
        int clx = px + (int)(11 * sinf(hRad - 0.48f));
        int cly = py - (int)(11 * cosf(hRad - 0.48f));
        int crx = px + (int)(11 * sinf(hRad + 0.48f));
        int cry = py - (int)(11 * cosf(hRad + 0.48f));
        mapFillTriangle(ctx, cty, clx, cly, crx, cry, TFT_BLACK);   // outline
        // Slightly inset arrow
        int ctx2 = px + (int)(28 * sinf(hRad));
        int cty2 = py - (int)(28 * cosf(hRad));
        int clx2 = px + (int)(9 * sinf(hRad - 0.44f));
        int cly2 = py - (int)(9 * cosf(hRad - 0.44f));
        int crx2 = px + (int)(9 * sinf(hRad + 0.44f));
        int cry2 = py - (int)(9 * cosf(hRad + 0.44f));
        mapFillTriangle(ctx2, cty2, clx2, cly2, crx2, cry2, C_YOU_RING);

        // Centre disc
        mapFillCircle(px, py, 12, TFT_BLACK);   // black outer ring
        mapFillCircle(px, py, 10, C_YOU_RING);  // cobalt disc
        mapFillCircle(px, py,  5, TFT_WHITE);   // white centre
    }

    mapPush();
}

// =============================================================================
// Info panel  –  redesigned with left-border accent rows
// =============================================================================
//
// Each row: 4px cobalt left-border accent strip, white background,
// label in cobalt size-1, value in black size-2. High contrast throughout.

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

// Clear row to white, redraw bottom divider (thin cobalt line)
static void clearRow(int row)
{
    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    tft.fillRect(INFO_X + 2, y, INFO_W - 2, PANEL_ROW_H, C_PANEL_BG);
    tft.drawFastHLine(INFO_X + 2, y + PANEL_ROW_H - 1, INFO_W - 2, C_PANEL_LINE);
}

// Draw the 4px left accent strip in the given color
static inline void drawAccentStrip(int row, uint16_t accentCol)
{
    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    tft.fillRect(INFO_X + 2, y, 4, PANEL_ROW_H - 1, accentCol);
}

static void drawRow(int row, const char *label, const char *value,
                    uint16_t valColor = TFT_BLACK)
{
    uint32_t h = fnv1a(value, valColor);
    if (rowHash[row] == h) return;
    rowHash[row] = h;

    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    clearRow(row);
    drawAccentStrip(row, valColor == TFT_BLACK ? C_PANEL_ACCENT : valColor);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 10, y + 5);

    tft.setTextColor(valColor, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X + 10, y + 18);
}

static void drawRowWithBar(int row, const char *label, const char *value,
                            int pct, uint16_t barCol)
{
    uint32_t h = fnv1aBar(value, pct, barCol);
    if (rowHash[row] == h) return;
    rowHash[row] = h;

    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    clearRow(row);
    drawAccentStrip(row, barCol);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 10, y + 4);

    tft.setTextColor(TFT_BLACK, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X + 10, y + 16);

    drawProgressBar(INFO_X + 10, y + 36, INFO_W - 16, 7, pct, barCol, C_BAR_TRACK);
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
    drawAccentStrip(row, chipTxt);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X + 10, y + 5);

    drawChip(INFO_X + 10, y + 19, INFO_W - 16, 22, 4, chipBg, chipTxt, chipText, 1);
}

static void drawBleRow(bool connected)
{
    uint32_t h = connected ? 0xBEEF0001u : 0xBEEF0000u;
    if (rowHash[5] == h) return;
    rowHash[5] = h;

    int row = 5;
    int y   = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    int hh  = min(SCREEN_H - y, PANEL_ROW_H);
    tft.fillRect(INFO_X + 2, y, INFO_W - 2, hh, C_PANEL_BG);

    uint16_t dotCol  = connected ? C_GREEN : C_AMBER;
    uint16_t txtCol  = connected ? C_GREEN : C_AMBER;

    // Accent strip
    tft.fillRect(INFO_X + 2, y, 4, hh, dotCol);

    // Status dot
    tft.fillCircle(INFO_X + 18, y + hh / 2, 5, dotCol);
    tft.drawCircle(INFO_X + 18, y + hh / 2, 5, TFT_BLACK);  // crisp outline

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(txtCol, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(connected ? "CONNECTED" : "WAITING...", INFO_X + 28, y + hh / 2);
}

// Panel header: pure white, bold cobalt wordmark + live indicator dot
void initPanel()
{
    memset(rowHash, 0, sizeof(rowHash));

    tft.fillRect(INFO_X, 0, INFO_W, SCREEN_H, C_PANEL_BG);
    drawSeparator();

    // Header bar background
    tft.fillRect(INFO_X + 2, 0, INFO_W - 2, 29, C_BLUE);

    // "NAVIGATOR" wordmark in white on cobalt
    tft.setTextColor(TFT_WHITE, C_BLUE);
    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.drawString("NAVIGATOR", INFO_X + 8, 14);

    // Small live-indicator dot (white on cobalt)
    tft.fillCircle(INFO_X + INFO_W - 10, 14, 4, TFT_WHITE);
    tft.drawCircle(INFO_X + INFO_W - 10, 14, 4, C_BLUE);   // invis border, for shape

    // Header bottom rule
    tft.drawFastHLine(INFO_X + 2, 29, INFO_W - 2, C_PANEL_LINE);
}

void updatePanel()
{
    extern float haversineKm(float, float, float, float);
    char buf[32];

    // ── Row 0: Speed ──────────────────────────────────────────────────────────
    {
        float    kmh = gpsSpeed * 3.6f;
        uint16_t col = (kmh > 1.0f) ? TFT_BLACK : C_BLUE;
        if (kmh < 10.0f) snprintf(buf, sizeof(buf), "%.1f KM/H", kmh);
        else              snprintf(buf, sizeof(buf), "%.0f KM/H", kmh);
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
        if (tripKm < 1.0f) snprintf(buf, sizeof(buf), "%.0f M",  tripKm * 1000.0f);
        else                snprintf(buf, sizeof(buf), "%.2f KM", tripKm);
        drawRow(2, "TRIP", buf, TFT_BLACK);
    }

    // ── Row 3: Route progress ─────────────────────────────────────────────────
    {
        if (routeComplete && !route.empty()) {
            if (nearestIdx == 0) {
                drawRowWithChip(3, "ROUTE", "ON ROUTE", C_CHIP_GREEN_BG, C_GREEN);
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
                    snprintf(buf, sizeof(buf), "%.0fM/%.1fKM",
                             doneKm * 1000.0f, totalKm);
                else
                    snprintf(buf, sizeof(buf), "%.1f/%.1fKM", doneKm, totalKm);
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
                snprintf(buf, sizeof(buf), "%.0f M", nearestDistM);
            else
                snprintf(buf, sizeof(buf), "%.1f KM", nearestDistM / 1000.0f);
            drawRowWithChip(4, "NEAREST", buf, C_CHIP_GREEN_BG, C_GREEN);
        } else {
            drawRow(4, "NEAREST", "--", C_BLUE);
        }
    }

    // ── Row 5: BLE ────────────────────────────────────────────────────────────
    drawBleRow(bleCon);
}

// =============================================================================
// OTA Screen  –  white theme redesign
// =============================================================================
//
// Layout (480 × 320 landscape):
//
//  ┌──────────────────────────────────────────────────────┐  y=0
//  │  [blue bar]  TAHVIA · FIRMWARE UPDATE    [chip ver] │
//  ├──────────────────────────────────────────────────────┤  y=48
//  │                     │                               │
//  │   progress ring     │  [status chip]                │
//  │   (cobalt on wht)   │  ─────────────────────────    │
//  │        pct%         │  TRANSFER  [======      ]     │
//  │                     │  ─────────────────────────    │
//  │                     │  CHUNKS   x / y               │
//  │                     │  RECEIVED xx KB               │
//  ├──────────────────────────────────────────────────────┤  y=266
//  │  [amber bar]  ⚠  DO NOT POWER OFF OR DISCONNECT     │
//  └──────────────────────────────────────────────────────┘  y=319

#define C_OTA_BG     TFT_WHITE
#define C_OTA_DIVID  RGB(  0,  55, 220)   // cobalt dividers
#define C_OTA_TRACK  RGB(210, 225, 255)   // pale blue ring track / bar track
#define C_OTA_WARN   RGB(240, 130,   0)   // amber warning strip
#define C_OTA_WBG   RGB(255, 245, 225)   // very pale amber for text bg in strip

#define RING_CX    96
#define RING_CY    157
#define RING_R      64
#define RING_THICK  16

// Thick arc: stamp filled circles along arc path. 0° = top, clockwise.
static void drawArc(int cx, int cy, int r, int thick,
                    int startDeg, int endDeg, uint16_t col)
{
    if (endDeg <= startDeg) return;
    int sweep = endDeg - startDeg;
    for (int d = 0; d <= sweep; d += 2) {
        float rad = ((startDeg + d) - 90) * DEG_TO_RAD;
        int x = cx + (int)(r * cosf(rad));
        int y = cy + (int)(r * sinf(rad));
        tft.fillCircle(x, y, thick / 2, col);
    }
}

static void drawOtaScreenHeader()
{
    tft.fillScreen(C_OTA_BG);

    // ── Header bar: full-width cobalt ────────────────────────────────────────
    tft.fillRect(0, 0, SCREEN_W, 48, C_BLUE);

    // Brand wordmark
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, C_BLUE);
    tft.setTextSize(2);
    tft.drawString("TAHVIA", 14, 24);

    // Separator dot (white on cobalt)
    tft.fillCircle(88, 24, 3, TFT_WHITE);

    // Screen title
    tft.setTextColor(RGB(180, 210, 255), C_BLUE);  // lighter blue text
    tft.drawString("FIRMWARE UPDATE", 100, 24);

    // Version chip: white outline chip on cobalt
    tft.drawRoundRect(SCREEN_W - 70, 10, 62, 26, 6, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, C_BLUE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.drawString(FW_VERSION, SCREEN_W - 39, 24);

    // ── Vertical divider (cobalt) between ring and data columns ──────────────
    tft.drawFastVLine(185, 48, 218, C_OTA_DIVID);
    tft.drawFastVLine(186, 48, 218, C_OTA_DIVID);

    // ── Warning strip ─────────────────────────────────────────────────────────
    tft.fillRect(0, 267, SCREEN_W, 53, C_OTA_WARN);
    tft.drawFastHLine(0, 267, SCREEN_W, RGB(180, 90, 0));  // darker top rule

    // Warning triangle (black outline / amber filled)
    tft.fillTriangle(22, 307, 38, 277, 54, 307, TFT_BLACK);
    tft.fillTriangle(25, 304, 38, 281, 51, 304, C_OTA_WARN);
    tft.fillRect(37, 288, 3, 9, TFT_BLACK);
    tft.fillCircle(38, 301, 2, TFT_BLACK);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_BLACK, C_OTA_WARN);
    tft.setTextSize(2);
    tft.drawString("DO NOT POWER OFF OR DISCONNECT", 62, 291);
}

void drawOtaScreenDynamic(int chunksRcvd, int chunkTotal,
                           size_t bytesWritten, const char *statusMsg, bool isError)
{
    // Clear only the content zone (leave header and warning strip)
    tft.fillRect(0,   48, 186, 219, C_OTA_BG);          // ring column
    tft.fillRect(187, 48, SCREEN_W - 187, 219, C_OTA_BG); // data column

    float    pct       = (chunkTotal > 0) ? (float)chunksRcvd / (float)chunkTotal : 0.0f;
    int      pctI      = (int)(pct * 100.0f);
    uint16_t accentCol = isError ? C_RED : C_BLUE;

    // ── Left column: progress ring ────────────────────────────────────────────

    // Track arc (full circle, pale blue)
    drawArc(RING_CX, RING_CY, RING_R, RING_THICK, 0, 360, C_OTA_TRACK);

    // Progress arc (cobalt or red)
    if (pctI > 0) {
        int sweepDeg = (int)(3.6f * constrain(pctI, 0, 100));
        drawArc(RING_CX, RING_CY, RING_R, RING_THICK, 0, sweepDeg, accentCol);
    }

    // End-cap dot at current progress position (crisp terminator)
    if (pctI > 0 && pctI < 100) {
        float capRad = ((pctI * 3.6f) - 90.0f) * DEG_TO_RAD;
        int cx2 = RING_CX + (int)(RING_R * cosf(capRad));
        int cy2 = RING_CY + (int)(RING_R * sinf(capRad));
        tft.fillCircle(cx2, cy2, RING_THICK / 2 + 2, TFT_WHITE);
        tft.fillCircle(cx2, cy2, RING_THICK / 2,     accentCol);
    }

    // Hollow interior (white fill to restore background)
    tft.fillCircle(RING_CX, RING_CY, RING_R - RING_THICK / 2 - 1, C_OTA_BG);

    // Percentage text inside ring
    char heroBuf[8];
    snprintf(heroBuf, sizeof(heroBuf), "%d%%", pctI);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(accentCol, C_OTA_BG);
    tft.setTextSize(3);
    tft.drawString(heroBuf, RING_CX, RING_CY - 8);

    tft.setTextSize(1);
    tft.setTextColor(C_OTA_DIVID, C_OTA_BG);
    tft.drawString("PROGRESS", RING_CX, RING_CY + 14);

    // ── Right column ─────────────────────────────────────────────────────────
    const int rx = 198;
    const int rw = SCREEN_W - rx - 10;

    // Status chip
    uint16_t chipBg, chipTxt;
    if (isError) {
        chipBg = C_CHIP_RED_BG;   chipTxt = C_RED;
    } else if (pctI >= 100) {
        chipBg = C_CHIP_GREEN_BG; chipTxt = C_GREEN;
    } else {
        chipBg = C_CHIP_BLUE_BG;  chipTxt = C_BLUE;
    }
    tftRoundRect(rx, 58, rw, 28, 5, chipBg);
    tft.drawRoundRect(rx, 58, rw, 28, 5, chipTxt);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(chipTxt, chipBg);
    tft.setTextSize(1);
    tft.drawString(statusMsg, rx + 10, 72);

    // Section divider rule
    auto otaRule = [&](int y) {
        tft.drawFastHLine(rx, y, rw, C_OTA_TRACK);
    };

    // Transfer bar
    otaRule(96);
    tft.setTextColor(C_BLUE, C_OTA_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    tft.drawString("TRANSFER", rx, 103);
    drawProgressBar(rx, 116, rw, 10, pctI, accentCol, C_OTA_TRACK);

    // Chunks
    otaRule(137);
    tft.setTextColor(C_BLUE, C_OTA_BG);
    tft.setTextSize(1);
    tft.drawString("CHUNKS", rx, 144);
    char chunkBuf[24];
    snprintf(chunkBuf, sizeof(chunkBuf), "%d / %d", chunksRcvd, chunkTotal);
    tft.setTextColor(TFT_BLACK, C_OTA_BG);
    tft.setTextSize(2);
    tft.drawString(chunkBuf, rx, 156);

    // Bytes
    otaRule(188);
    tft.setTextColor(C_BLUE, C_OTA_BG);
    tft.setTextSize(1);
    tft.drawString("RECEIVED", rx, 195);
    char bytesBuf[20];
    if (bytesWritten < 1024)
        snprintf(bytesBuf, sizeof(bytesBuf), "%u B",    (unsigned)bytesWritten);
    else if (bytesWritten < 1048576)
        snprintf(bytesBuf, sizeof(bytesBuf), "%.1f KB", bytesWritten / 1024.0f);
    else
        snprintf(bytesBuf, sizeof(bytesBuf), "%.2f MB", bytesWritten / 1048576.0f);
    tft.setTextColor(TFT_BLACK, C_OTA_BG);
    tft.setTextSize(2);
    tft.drawString(bytesBuf, rx, 208);
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
// Waiting screen  –  white theme
// =============================================================================

void drawWaitingScreen()
{
    tft.fillRect(0, 0, MAP_W - 1, MAP_H, TFT_WHITE);
    drawSeparator();

    int cx = MAP_W / 2;
    int cy = MAP_H / 2;

    // Icon background chip
    tftRoundRect(cx - 26, cy - 68, 52, 52, 10, C_CHIP_BLUE_BG);
    tft.drawRoundRect(cx - 26, cy - 68, 52, 52, 10, C_BLUE);

    // Bluetooth-style signal rings (cobalt)
    tft.drawCircle(cx, cy - 42, 16, C_BLUE);
    tft.drawCircle(cx, cy - 42, 11, C_BLUE);
    tft.fillCircle(cx, cy - 42,  5, C_BLUE);
    tft.fillCircle(cx, cy - 42,  2, TFT_WHITE);

    // Bold horizontal rule under icon
    tft.fillRect(cx - 80, cy - 7, 160, 2, C_BLUE);

    // Primary message
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextSize(2);
    tft.drawString("NO CONNECTION", cx, cy + 8);

    // Subtitle
    tft.setTextSize(1);
    tft.setTextColor(C_BLUE, TFT_WHITE);
    tft.drawString("OPEN THE APP AND HIT 'CONNECT DEVICE'", cx, cy + 28);
}

// =============================================================================
// Boot screen  –  split wordmark on pure white
// =============================================================================

void showBootScreen()
{
    tft.fillScreen(C_BOOT_BG);

    int cx = SCREEN_W / 2;
    int cy = SCREEN_H / 2;

    // Decorative cobalt bar behind the wordmark
    tft.fillRect(0, cy - 46, SCREEN_W, 52, C_BLUE);

    // "TAH" – white on cobalt (right-aligned to centre)
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_WHITE, C_BLUE);
    tft.setTextSize(4);
    tft.drawString("TAH", cx, cy - 20);

    // "VIA" – cobalt on white (left-aligned from centre)
    // White background block to the right of cx on the bar for contrast
    tft.fillRect(cx, cy - 46, SCREEN_W - cx, 52, TFT_WHITE);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_BLUE, TFT_WHITE);
    tft.setTextSize(4);
    tft.drawString("VIA", cx, cy - 20);

    // Thin cobalt rule below wordmark
    tft.drawFastHLine(cx - 80, cy + 10, 160, C_BLUE);

    // Subtitle
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, C_BOOT_BG);
    tft.setTextSize(1);
    tft.drawString("GPS NAVIGATOR", cx, cy + 22);

    // Version chip
    drawChip(cx - 40, cy + 34, 80, 20, 5, C_CHIP_BLUE_BG, C_BLUE, FW_VERSION, 1);

    delay(2500);
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