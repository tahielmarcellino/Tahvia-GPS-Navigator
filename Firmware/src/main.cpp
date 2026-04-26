/**
 * gps_navigator.cpp  -  v2.7.3
 *
 * ESP32 GPS Navigator . 480x320 . TFT_eSPI
 *
 * CHANGES vs v2.7.2:
 *  [FIX A] Map flicker with route loaded / during navigation
 *           → routeComplete (always-true state flag) was in the draw condition,
 *             causing unconditional 20fps full redraws. Replaced with a
 *             one-shot routeJustCompleted edge flag that clears after drawMap().
 *  [FIX B] Waiting screen "Advertising:" chip overflowed and removed
 *           → DEVICE_NAME string exceeded chip width; chip removed entirely
 *             as it adds no user value (app handles BLE discovery itself)
 *  [FIX C] Boot screen progress bar removed per UX request
 *  [FIX D] OTA screen too empty (110px dead zone between chip and warning)
 *           → Redesigned layout: meta row compact at top (below header),
 *             taller progress bar, large hero percentage numeral fills the
 *             centre, status chip moved to bottom above warning line.
 *             No data added — existing values repositioned for visual balance.
 *
 * BLE protocol (from app):
 *   route_start     {"type":"route_start","totalWaypoints":N,...}
 *   route_chunk     {"type":"route_chunk","chunkIndex":I,"waypoints":[...]}
 *   route_complete  {"type":"route_complete","totalWaypoints":N}
 *   gps update      {"coordinates":{"latitude":..,"longitude":..},"speed":..,"heading":..}
 *   start_fw_update {"type":"start_fw_update","chunkCount":N}
 *   fw_chunk        {"type":"fw_chunk","index":I,"size":S,"data":"<base64>"}
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <vector>
#include <math.h>
#include <Update.h>

// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------
TFT_eSPI    tft;
TFT_eSprite mapSprite(&tft);

#define FW_VERSION  "V1.1.5"
#define SCREEN_W    480
#define SCREEN_H    320
#define MAP_W       320
#define MAP_H       320
#define INFO_X      320
#define INFO_W      160

static bool spriteOk    = false;
static bool spriteTried = false;

// -----------------------------------------------------------------------------
// Palette  (White / Black / pure named colors — no grayscale)
// -----------------------------------------------------------------------------
#define RGB(r,g,b) ((uint16_t)(((uint16_t)((r)>>3)<<11)|((uint16_t)((g)>>2)<<5)|((uint16_t)((b)>>3))))

// ── Map colors
#define C_MAP_BG      RGB(242,239,233)   // warm parchment
#define C_ROAD_FILL   TFT_WHITE
#define C_ROAD_SHAD   RGB(220,216,210)
#define C_ROUTE_SHAD  RGB(  8, 98,185)
#define C_ROUTE_FILL  RGB( 26,133,255)   // Google Blue
#define C_DONE_SHAD   RGB( 80,110,200)
#define C_DONE_FILL   RGB(130,160,235)
#define C_START       RGB( 26,133,255)
#define C_END         RGB(234, 67, 53)   // Google Red
#define C_YOU_RING    TFT_WHITE
#define C_YOU_FILL    RGB( 26,133,255)
#define C_YOU_SNAP    RGB( 26,133,255)
#define C_YOU_PULSE   RGB(161,204,255)
#define C_TURN_DOT    TFT_WHITE
#define C_NOROUTE     RGB( 26,133,255)

// ── Panel colors (white palette, no gray)
#define C_PANEL_BG    TFT_WHITE
#define C_PANEL_LINE  RGB(232,234,237)   // very light blue-tinted white
#define C_HEADER_BG   TFT_WHITE
#define C_HEADER_LINE RGB(232,234,237)

// ── Text colors (black + semantic)
#define C_TEXT_PRIMARY   TFT_BLACK
#define C_TEXT_LABEL     RGB( 32, 33, 36)   // near-black for labels
#define C_TEXT_MUTED     RGB(128, 68,255)   // use blue-purple for "muted" context
#define C_TEXT_UNIT      RGB( 26,133,255)   // blue for units

// ── Semantic status colors
#define C_BLUE        RGB( 26,133,255)
#define C_GREEN       RGB( 52,168, 83)
#define C_RED         RGB(234, 67, 53)
#define C_AMBER       RGB(251,188,  4)
#define C_AMBER_DARK  RGB(196,117,  0)

// ── Chip / badge backgrounds
#define C_CHIP_BLUE_BG  RGB(232,242,255)
#define C_CHIP_GREEN_BG RGB(230,244,234)
#define C_CHIP_RED_BG   RGB(253,232,230)
#define C_CHIP_AMB_BG   RGB(254,247,224)
#define C_CHIP_AMB_TXT  RGB(121, 78,  0)

// ── OTA specific
#define C_OTA_BG      TFT_WHITE
#define C_OTA_ICON_BG RGB(232,242,255)

// ── Progress bar track (pure white with border)
#define C_BAR_TRACK   RGB(232,234,237)

// ── Boot screen
#define C_BOOT_BG     RGB(248,249,250)   // near-white tinted

// -----------------------------------------------------------------------------
// BLE
// -----------------------------------------------------------------------------
#define DEVICE_NAME         "ESP32_Speedometer"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

static BLEServer         *pServer    = nullptr;
static BLECharacteristic *pChar      = nullptr;
static volatile bool      bleCon     = false;
static bool               bleWasCon  = false;
// FIX 5: non-blocking reconnect timer replaces delay(200)
static bool               bleAdvPending   = false;
static uint32_t           bleAdvPendingMs = 0;

// -----------------------------------------------------------------------------
// Lock-free SPSC ring buffer
// -----------------------------------------------------------------------------
#define BLE_CHUNK_DATA  512
#define RING_SLOTS      8

struct RingSlot {
    char     data[BLE_CHUNK_DATA];
    uint16_t len;
};

static RingSlot          ring[RING_SLOTS];
static volatile uint32_t ringHead = 0;
static volatile uint32_t ringTail = 0;
static volatile bool     bleJustConnected = false;
static TaskHandle_t      bleProcessTaskHandle = nullptr;

// -----------------------------------------------------------------------------
// JSON accumulation buffer
// -----------------------------------------------------------------------------
#define ACCUM_BUF 1024
static char accumBuf[ACCUM_BUF];
static int  accumLen = 0;

// -----------------------------------------------------------------------------
// OTA queues / state
// -----------------------------------------------------------------------------
struct OtaStartMsg { int chunkCount; };
static QueueHandle_t otaStartQueue = nullptr;

#define OTA_BUF_SIZE 192
struct OtaBlock {
    uint8_t  data[OTA_BUF_SIZE];
    uint16_t len;
    bool     isLast;
};
static QueueHandle_t otaWriteQueue = nullptr;

enum OtaState { OTA_IDLE, OTA_RECEIVING, OTA_APPLYING, OTA_DONE, OTA_ERROR };
static OtaState      otaState               = OTA_IDLE;
static volatile bool otaStartQueued         = false;
static int           otaChunkCount          = 0;
static int           otaChunksRcvd          = 0;
static size_t        otaTotalBytes          = 0;
static bool          otaIsError             = false;
static char          otaStatusMsg[64]       = "Receiving firmware...";
static int           cpu1OtaChunkCount      = 0;
static int           chunksQueued           = 0;

// FIX 7: draw OTA header only once; subsequent calls refresh dynamic regions
static bool          otaHeaderDrawn         = false;

// -----------------------------------------------------------------------------
// Route / GPS
// -----------------------------------------------------------------------------
struct WP { float lat, lon; };
static std::vector<WP> route;
static int   routeExpected = 0;
static bool  routeComplete = false;
static volatile bool routeChanged        = false;
static volatile bool gpsChanged          = false;
static volatile bool doResetAll          = false;
// routeComplete stays true as state; routeJustCompleted is a one-shot draw
// trigger that clears after drawMap() consumes it — stops unconditional
// 20 fps redraws that cause map flicker when a route is loaded/navigating.
static volatile bool routeJustCompleted  = false;

static float gpsLat = 0, gpsLon = 0;
static float gpsSpeed = 0, gpsHeading = 0;
static bool  gpsValid = false;

static bool     zoomedMode  = false;
static float    zoomRadiusM = 300.0f;
static uint32_t lastGpsMs   = 0;
#define GPS_TIMEOUT_MS 2000

static std::vector<int> turnIndices;
#define TURN_DEG 45.0f

struct VP { float minLat,maxLat,minLon,maxLon; bool ready=false; };
static VP vp;

static int   nearestIdx   = -1;
static float nearestDistM = 1e9f;
static bool  snapped      = false;
#define SNAP_M 15.0f

static float tripKm        = 0;
static float lastTripLat   = 0, lastTripLon = 0;
static bool  lastTripValid = false;

// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------
#define BAT_PIN         34
#define BAT_SAMPLES     50
#define BAT_MAX_V       4.2f
#define BAT_MIN_V       3.0f
#define BAT_NOM_V       3.7f
#define BAT_R1          38000.0f
#define BAT_R2          100000.0f
#define BAT_DIV_RATIO   (BAT_R2 / (BAT_R1 + BAT_R2))
#define BAT_ADC_REF     3.3f
#define BAT_ADC_MAX     4095.0f
#define BAT_INTERVAL_MS 5000
#define BAT_GRACE_MS    5000

static float    batVoltage = BAT_NOM_V;
static int      batPercent = -1;
static uint32_t lastBatMs  = 0;
static uint32_t batStartMs = 0;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
static void processJSON(const char *raw, size_t len);
static void drawOtaScreen(int chunksRcvd, int chunkTotal,
                           size_t bytesWritten, const char *statusMsg,
                           bool isError);
static void drawOtaScreenDynamic(int chunksRcvd, int chunkTotal,
                                  size_t bytesWritten, const char *statusMsg,
                                  bool isError);
static void otaAbortCPU0(const char *reason);
static void ensureSprite();
static void drawSeparator();   // FIX 3: shared helper

// -----------------------------------------------------------------------------
// Sprite helpers
// -----------------------------------------------------------------------------
static void ensureSprite()
{
    if(spriteOk && mapSprite.created()) return;
    if(spriteTried) return;
    spriteTried = true;
    mapSprite.setColorDepth(8);
    mapSprite.createSprite(MAP_W, MAP_H);
    if(mapSprite.created()){
        spriteOk = true;
        Serial.printf("[SPRITE] 8bpp allocated - heap=%u  psram=%u\n",
                      ESP.getFreeHeap(), ESP.getFreePsram());
        return;
    }
    spriteOk = false;
    Serial.printf("[SPRITE] alloc failed (no PSRAM, heap=%u) - "
                  "direct TFT draw active\n", ESP.getFreeHeap());
}

static void releaseSprite()
{
    if(!mapSprite.created()){ spriteOk=false; return; }
    mapSprite.deleteSprite();
    spriteOk    = false;
    spriteTried = false;
    Serial.printf("[SPRITE] freed for OTA - heap=%u\n", ESP.getFreeHeap());
}

// -----------------------------------------------------------------------------
// FIX 3: shared separator helper — always draws the vertical divider between
//         the map area and the info panel so any screen that fills the map
//         area can restore it without knowing panel internals.
// -----------------------------------------------------------------------------
static void drawSeparator()
{
    tft.drawFastVLine(INFO_X, 0, SCREEN_H, C_PANEL_LINE);
}

// -----------------------------------------------------------------------------
// Drawing target abstraction
// -----------------------------------------------------------------------------
static inline TFT_eSprite* spr() { return &mapSprite; }

static void mapFill(uint16_t color)
{
    if(spriteOk) mapSprite.fillSprite(color);
    else         tft.fillRect(0, 0, MAP_W, MAP_H, color);
}

static void mapPush()
{
    if(spriteOk) mapSprite.pushSprite(0, 0);
}

// -----------------------------------------------------------------------------
// Geo helpers
// -----------------------------------------------------------------------------
static float haversineM(float la1, float lo1, float la2, float lo2)
{
    const float R = 6371000.0f;
    float dLa = (la2-la1)*DEG_TO_RAD, dLo = (lo2-lo1)*DEG_TO_RAD;
    float a = sinf(dLa/2)*sinf(dLa/2)
            + cosf(la1*DEG_TO_RAD)*cosf(la2*DEG_TO_RAD)*sinf(dLo/2)*sinf(dLo/2);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f-a));
}

static float haversineKm(float la1, float lo1, float la2, float lo2)
{ return haversineM(la1,lo1,la2,lo2) / 1000.0f; }

static void l2p(float lat, float lon, int &px, int &py)
{
    if(!vp.ready){ px=MAP_W/2; py=MAP_H/2; return; }
    float laR = vp.maxLat-vp.minLat, loR = vp.maxLon-vp.minLon;
    if(laR<1e-7f) laR=1e-7f;
    if(loR<1e-7f) loR=1e-7f;
    px = (int)((lon-vp.minLon)/loR*(MAP_W-1));
    py = (int)((1.0f-(lat-vp.minLat)/laR)*(MAP_H-1));
    px = constrain(px,0,MAP_W-1);
    py = constrain(py,0,MAP_H-1);
}

static float bearingDeg(const WP &a, const WP &b)
{
    float dLo = (b.lon-a.lon)*DEG_TO_RAD;
    float laA = a.lat*DEG_TO_RAD, laB = b.lat*DEG_TO_RAD;
    float y = sinf(dLo)*cosf(laB);
    float x = cosf(laA)*sinf(laB) - sinf(laA)*cosf(laB)*cosf(dLo);
    return fmodf(atan2f(y,x)*RAD_TO_DEG + 360.0f, 360.0f);
}

static float angleDiff(float a, float b)
{
    float d = fmodf(fabsf(a-b), 360.0f);
    return d > 180.0f ? 360.0f-d : d;
}

// -----------------------------------------------------------------------------
// Viewport builders
// -----------------------------------------------------------------------------
static void buildVP()
{
    if(route.size() < 2) return;
    float mnLa=1e9f, mxLa=-1e9f, mnLo=1e9f, mxLo=-1e9f;
    for(auto &w : route){
        mnLa=fminf(mnLa,w.lat); mxLa=fmaxf(mxLa,w.lat);
        mnLo=fminf(mnLo,w.lon); mxLo=fmaxf(mxLo,w.lon);
    }
    float cLa = (mnLa+mxLa)*0.5f, cLo = (mnLo+mxLo)*0.5f;
    float cosC = fmaxf(cosf(cLa*DEG_TO_RAD), 0.01f);
    float laM   = (mxLa-mnLa)*111320.0f;
    float loM   = (mxLo-mnLo)*111320.0f*cosC;
    float sideM = fmaxf(fmaxf(laM,loM), 50.0f)*1.5f;
    float halfLa = sideM*0.5f/111320.0f;
    float halfLo = sideM*0.5f/(111320.0f*cosC);
    vp.minLat=cLa-halfLa; vp.maxLat=cLa+halfLa;
    vp.minLon=cLo-halfLo; vp.maxLon=cLo+halfLo;
    vp.ready=true;
}

static void buildZoomedVP(float centerLat, float centerLon)
{
    float cosC   = fmaxf(cosf(centerLat*DEG_TO_RAD), 0.01f);
    float halfLa = zoomRadiusM/111320.0f;
    float halfLo = zoomRadiusM/(111320.0f*cosC);
    vp.minLat=centerLat-halfLa; vp.maxLat=centerLat+halfLa;
    vp.minLon=centerLon-halfLo; vp.maxLon=centerLon+halfLo;
    vp.ready=true;
}

static void buildTurns()
{
    turnIndices.clear();
    if(route.size() < 3) return;
    for(int i=1; i<(int)route.size()-1; i++){
        float bIn  = bearingDeg(route[i-1], route[i]);
        float bOut = bearingDeg(route[i],   route[i+1]);
        if(angleDiff(bIn,bOut) >= TURN_DEG)
            turnIndices.push_back(i);
    }
}

// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------
static void readBattery()
{
    uint32_t sum = 0;
    for(int i=0; i<BAT_SAMPLES; i++){ sum += analogRead(BAT_PIN); delayMicroseconds(200); }
    float raw   = (float)(sum/BAT_SAMPLES);
    float vAdc  = (raw/BAT_ADC_MAX)*BAT_ADC_REF;
    batVoltage  = vAdc/BAT_DIV_RATIO;
    float v     = constrain(batVoltage, BAT_MIN_V, BAT_MAX_V);
    int newPct  = (int)(((v-BAT_MIN_V)/(BAT_MAX_V-BAT_MIN_V))*100.0f);
    newPct      = constrain(newPct, 0, 100);
    bool inGrace = (batStartMs==0)||((millis()-batStartMs)<BAT_GRACE_MS);
    if(batPercent<0 || inGrace) batPercent = newPct;
    else if(newPct < batPercent) batPercent = newPct;
}

// -----------------------------------------------------------------------------
// Thick-line helper
// -----------------------------------------------------------------------------
static void thickLine(int x0,int y0,int x1,int y1, int w, uint16_t col)
{
    float dx=(float)(x1-x0), dy=(float)(y1-y0);
    float len=sqrtf(dx*dx+dy*dy);
    if(len<0.5f){
        if(spriteOk) mapSprite.drawPixel(x0,y0,col);
        else         tft.drawPixel(x0,y0,col);
        return;
    }
    float nx=-dy/len, ny=dx/len;
    int half=w/2;
    for(int t=-half;t<=half;t++){
        int ax=x0+(int)roundf(nx*t), ay=y0+(int)roundf(ny*t);
        int bx=x1+(int)roundf(nx*t), by=y1+(int)roundf(ny*t);
        if(spriteOk) mapSprite.drawLine(ax,ay,bx,by,col);
        else         tft.drawLine(ax,ay,bx,by,col);
    }
}

static void mapFillCircle(int x,int y,int r,uint16_t c){
    if(spriteOk) mapSprite.fillCircle(x,y,r,c);
    else         tft.fillCircle(x,y,r,c);
}
static void mapFillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){
    if(spriteOk) mapSprite.fillTriangle(x0,y0,x1,y1,x2,y2,c);
    else         tft.fillTriangle(x0,y0,x1,y1,x2,y2,c);
}
static void mapSetTextColor(uint16_t fg,uint16_t bg){
    if(spriteOk) mapSprite.setTextColor(fg,bg);
    else         tft.setTextColor(fg,bg);
}
static void mapSetTextDatum(uint8_t d){
    if(spriteOk) mapSprite.setTextDatum(d);
    else         tft.setTextDatum(d);
}
static void mapSetTextSize(uint8_t s){
    if(spriteOk) mapSprite.setTextSize(s);
    else         tft.setTextSize(s);
}
static void mapDrawString(const char *str,int x,int y){
    if(spriteOk) mapSprite.drawString(str,x,y);
    else         tft.drawString(str,x,y);
}

// -----------------------------------------------------------------------------
// Map drawing
// FIX 2: ensureSprite() removed from here — called once in setup() only.
//         drawMap() relies on spriteOk already being set correctly.
// -----------------------------------------------------------------------------
static void drawMap()
{
    // FIX 2: do NOT call ensureSprite() here

    if(zoomedMode && gpsValid)
        buildZoomedVP(gpsLat, gpsLon);
    else if(routeComplete || route.size()>=2)
        buildVP();

    mapFill(C_MAP_BG);

    if(!vp.ready || route.size()<2){
        // ── "No route loaded" state ──────────────────────────────────────────
        // FIX 1: this branch now also reached after BLE connect because
        //        SrvCB::onConnect sets routeChanged=true, triggering drawMap().
        int cx=MAP_W/2, cy=MAP_H/2;

        mapFillCircle(cx, cy-22, 18, C_BLUE);
        mapFillCircle(cx, cy-22, 12, C_MAP_BG);
        mapFillCircle(cx, cy-22, 7,  C_BLUE);
        mapFillCircle(cx, cy-22, 3,  C_MAP_BG);
        mapFillCircle(cx, cy-22, 2,  C_BLUE);

        mapSetTextColor(TFT_BLACK, C_MAP_BG);
        mapSetTextDatum(MC_DATUM);
        mapSetTextSize(2);
        mapDrawString("No route loaded", cx, cy+6);
        mapSetTextSize(1);
        mapSetTextColor(C_BLUE, C_MAP_BG);
        mapDrawString("Connect app to send a route", cx, cy+26);
        mapPush();
        return;
    }

    // Route - 2 passes: shadow then fill
    for(int pass=0; pass<2; pass++){
        int lw=(pass==0)?7:4;
        int x0,y0,x1,y1;
        l2p(route[0].lat,route[0].lon,x0,y0);
        for(size_t i=1; i<route.size(); i++){
            l2p(route[i].lat,route[i].lon,x1,y1);
            bool done=(nearestIdx>=0&&(int)i<=nearestIdx);
            uint16_t col;
            if(pass==0) col=done?C_DONE_SHAD:C_ROUTE_SHAD;
            else        col=done?C_DONE_FILL:C_ROUTE_FILL;
            thickLine(x0,y0,x1,y1,lw,col);
            x0=x1; y0=y1;
        }
    }

    // Turn dots
    for(int idx : turnIndices){
        if(idx<0||idx>=(int)route.size()) continue;
        int tx,ty; l2p(route[idx].lat,route[idx].lon,tx,ty);
        bool done=(nearestIdx>=0&&idx<=nearestIdx);
        mapFillCircle(tx,ty,7,done?C_DONE_SHAD:C_ROUTE_SHAD);
        mapFillCircle(tx,ty,5,done?C_DONE_FILL:C_ROUTE_FILL);
        mapFillCircle(tx,ty,3,C_TURN_DOT);
    }

    // Start dot
    { int sx,sy; l2p(route.front().lat,route.front().lon,sx,sy);
      mapFillCircle(sx,sy,9,C_ROUTE_SHAD);
      mapFillCircle(sx,sy,7,C_START);
      mapFillCircle(sx,sy,3,TFT_WHITE); }

    // End pin
    { int ex,ey; l2p(route.back().lat,route.back().lon,ex,ey);
      mapFillCircle(ex,ey-4,9,RGB(180,40,30));
      mapFillCircle(ex,ey-4,7,C_END);
      mapFillCircle(ex,ey-4,3,TFT_WHITE);
      mapFillTriangle(ex-4,ey,ex+4,ey,ex,ey+8,RGB(180,40,30));
      mapFillTriangle(ex-3,ey,ex+3,ey,ex,ey+6,C_END); }

    // YOU marker
    if(gpsValid){
        float drawLat=gpsLat, drawLon=gpsLon;
        if(snapped&&nearestIdx>=0){ drawLat=route[nearestIdx].lat; drawLon=route[nearestIdx].lon; }
        int px,py; l2p(drawLat,drawLon,px,py);
        mapFillCircle(px,py,18,C_YOU_PULSE);
        float hRad=gpsHeading*DEG_TO_RAD;
        int ctx=px+(int)(28*sinf(hRad)),     cty=py-(int)(28*cosf(hRad));
        int clx=px+(int)(10*sinf(hRad-0.45f)),cly=py-(int)(10*cosf(hRad-0.45f));
        int crx=px+(int)(10*sinf(hRad+0.45f)),cry=py-(int)(10*cosf(hRad+0.45f));
        mapFillTriangle(ctx,cty,clx,cly,crx,cry,C_YOU_PULSE);
        mapFillCircle(px,py,11,C_YOU_RING);
        mapFillCircle(px,py, 9,C_ROUTE_SHAD);
        mapFillCircle(px,py, 8,C_YOU_FILL);
    }

    mapPush();
}

// -----------------------------------------------------------------------------
// UI helpers — draw a filled rounded rect on the TFT
// -----------------------------------------------------------------------------
static void tftRoundRect(int x, int y, int w, int h, int r, uint16_t col)
{
    tft.fillRect(x+r,   y,     w-2*r, h,     col);
    tft.fillRect(x,     y+r,   w,     h-2*r, col);
    tft.fillCircle(x+r,     y+r,     r, col);
    tft.fillCircle(x+w-1-r, y+r,     r, col);
    tft.fillCircle(x+r,     y+h-1-r, r, col);
    tft.fillCircle(x+w-1-r, y+h-1-r, r, col);
}

static void drawProgressBar(int x, int y, int w, int h,
                             int pct, uint16_t fillCol, uint16_t trackCol)
{
    tftRoundRect(x, y, w, h, h/2, trackCol);
    int fillW = (int)(w * constrain(pct, 0, 100) / 100.0f);
    if(fillW >= h)
        tftRoundRect(x, y, fillW, h, h/2, fillCol);
    else if(fillW > 0)
        tft.fillRect(x, y, fillW, h, fillCol);
}

static void drawChip(int x, int y, int w, int h, int r,
                     uint16_t bgCol, uint16_t txtCol,
                     const char *text, uint8_t sz=1)
{
    tftRoundRect(x, y, w, h, r, bgCol);
    tft.setTextColor(txtCol, bgCol);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(sz);
    tft.drawString(text, x+w/2, y+h/2);
}

// -----------------------------------------------------------------------------
// Info panel  - MUST only be called from CPU0 (loop)
// -----------------------------------------------------------------------------
static void initPanel()
{
    tft.fillRect(INFO_X, 0, INFO_W, SCREEN_H, C_PANEL_BG);

    // Separator line (left edge)
    drawSeparator();   // FIX 3: use shared helper

    // ── Header bar ──
    tft.fillRect(INFO_X+1, 0, INFO_W-1, 28, C_PANEL_BG);
    tft.drawFastHLine(INFO_X+1, 28, INFO_W-1, C_PANEL_LINE);

    tft.setTextColor(TFT_BLACK, C_PANEL_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.drawString("NAVIGATOR", INFO_X+8, 14);

    tft.fillCircle(INFO_X+INFO_W-10, 14, 4, C_BLUE);
}

// Row layout: 6 rows, each 48px tall, starting at y=29
#define PANEL_ROW_H  48
#define PANEL_ROW_Y0 29

static void clearRow(int row)
{
    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    tft.fillRect(INFO_X+1, y, INFO_W-1, PANEL_ROW_H-1, C_PANEL_BG);
    tft.drawFastHLine(INFO_X+1, y + PANEL_ROW_H - 1, INFO_W-1, C_PANEL_LINE);
}

static void drawRow(int row, const char *label, const char *value,
                    uint16_t valColor=TFT_BLACK)
{
    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    clearRow(row);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X+7, y+6);

    tft.setTextColor(valColor, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X+7, y+20);
}

static void drawRowWithBar(int row, const char *label, const char *value,
                            int pct, uint16_t barCol)
{
    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    clearRow(row);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X+7, y+5);

    tft.setTextColor(TFT_BLACK, C_PANEL_BG);
    tft.setTextSize(2);
    tft.drawString(value, INFO_X+7, y+17);

    drawProgressBar(INFO_X+7, y+36, INFO_W-14, 6, pct, barCol, C_BAR_TRACK);
}

static void drawRowWithChip(int row, const char *label,
                              const char *chipText,
                              uint16_t chipBg, uint16_t chipTxt)
{
    int y = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    clearRow(row);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_BLUE, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(label, INFO_X+7, y+6);

    drawChip(INFO_X+7, y+20, INFO_W-14, 20, 5,
             chipBg, chipTxt, chipText, 1);
}

// BLE status row (last row)
// FIX 9: height capped to min(SCREEN_H - y, PANEL_ROW_H) to avoid overflow
static void drawBleRow(bool connected)
{
    int row = 5;
    int y   = PANEL_ROW_Y0 + row * PANEL_ROW_H;
    int h   = min(SCREEN_H - y, PANEL_ROW_H);  // FIX 9
    tft.fillRect(INFO_X+1, y, INFO_W-1, h, C_PANEL_BG);

    uint16_t dotCol = connected ? C_GREEN : C_AMBER;
    tft.fillCircle(INFO_X+12, y+14, 4, dotCol);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_BLACK, C_PANEL_BG);
    tft.setTextSize(1);
    tft.drawString(connected ? "Connected" : "Waiting...",
                   INFO_X+21, y+14);
}

// -----------------------------------------------------------------------------
// updatePanel - full panel refresh
// -----------------------------------------------------------------------------
static void updatePanel()
{
    char buf[32];

    // ── Row 0: Speed ──
    {
        float kmh = gpsSpeed * 3.6f;
        if(kmh < 10.0f) snprintf(buf, sizeof(buf), "%.1f km/h", kmh);
        else             snprintf(buf, sizeof(buf), "%.0f km/h", kmh);
        uint16_t col = (kmh > 1.0f) ? TFT_BLACK : C_BLUE;
        drawRow(0, "SPEED", buf, col);
    }

    // ── Row 1: Battery (with bar) ──
    {
        uint16_t barCol = (batPercent > 50) ? C_GREEN
                        : (batPercent > 20) ? C_AMBER
                        : C_RED;
        snprintf(buf, sizeof(buf), "%d%%", batPercent);
        drawRowWithBar(1, "BATTERY", buf, batPercent, barCol);
    }

    // ── Row 2: Trip distance ──
    {
        if(tripKm < 1.0f) snprintf(buf, sizeof(buf), "%.0f m",  tripKm * 1000.0f);
        else               snprintf(buf, sizeof(buf), "%.2f km", tripKm);
        drawRow(2, "TRIP", buf, TFT_BLACK);
    }

    // ── Row 3: Route progress (with bar) ──
    {
        if(routeComplete && !route.empty()){
            // FIX 10: nearestIdx==0 means user is at the very first waypoint,
            //         show a friendly chip instead of "0m/X.Xkm 0%"
            if(nearestIdx == 0){
                drawRowWithChip(3, "ROUTE", "On route",
                                C_CHIP_GREEN_BG, C_GREEN);
            } else {
                float doneKm = 0.0f;
                for(int i=1; i<=nearestIdx && i<(int)route.size(); i++)
                    doneKm += haversineKm(route[i-1].lat,route[i-1].lon,
                                          route[i].lat,  route[i].lon);
                float totalKm = 0.0f;
                for(int i=1; i<(int)route.size(); i++)
                    totalKm += haversineKm(route[i-1].lat,route[i-1].lon,
                                           route[i].lat,  route[i].lon);
                int pct = (totalKm > 0) ? (int)(100.0f * doneKm / totalKm) : 0;
                if(doneKm < 1.0f)
                    snprintf(buf, sizeof(buf), "%.0fm/%.1fkm", doneKm*1000.0f, totalKm);
                else
                    snprintf(buf, sizeof(buf), "%.1f/%.1fkm", doneKm, totalKm);
                drawRowWithBar(3, "ROUTE", buf, pct, C_BLUE);
            }
        } else if(!route.empty()){
            snprintf(buf, sizeof(buf), "%d/%d", (int)route.size(), routeExpected);
            drawRow(3, "LOADING", buf, C_AMBER);
        } else {
            drawRow(3, "ROUTE", "--", C_BLUE);
        }
    }

    // ── Row 4: Snap / nearest (with chip) ──
    {
        if(snapped && nearestIdx >= 0){
            snprintf(buf, sizeof(buf), "WP #%d", nearestIdx);
            drawRowWithChip(4, "POSITION", buf,
                            C_CHIP_BLUE_BG, C_BLUE);
        } else if(gpsValid && nearestIdx >= 0){
            if(nearestDistM < 1000.0f)
                snprintf(buf, sizeof(buf), "%.0f m away", nearestDistM);
            else
                snprintf(buf, sizeof(buf), "%.1f km", nearestDistM/1000.0f);
            drawRowWithChip(4, "NEAREST", buf,
                            C_CHIP_GREEN_BG, C_GREEN);
        } else {
            drawRow(4, "NEAREST", "--", C_BLUE);
        }
    }

    // ── Row 5: BLE ──
    drawBleRow(bleCon);
}

// -----------------------------------------------------------------------------
// OTA screen - static header (drawn once)
// FIX 7: split into header-once + dynamic-only to eliminate full-screen flicker
// FIX 8: "CHUNKS" meta column removed; replaced by two wider columns
// -----------------------------------------------------------------------------
// OTA screen layout (480×320):
//
//  ┌─────────────────────────────────────────────────────────────────┐
//  │  y=0..72   DARK HEADER BAND (C_BLUE fill)                       │
//  │             icon circle (white) + "FIRMWARE UPDATE" (white)     │
//  │             version chip (white outline, white text)            │
//  │             "Keep powered & BLE connected" muted subtitle        │
//  ├─────────────────────────────────────────────────────────────────┤
//  │  y=80..148  HERO ZONE — giant pct numeral (size 6, C_BLUE)      │  ← dynamic
//  │             "transferred" label underneath (C_BLUE, size 1)     │  ← dynamic
//  ├─────────────────────────────────────────────────────────────────┤
//  │  y=154..170 PROGRESS BAR (16px, full bleed w/ 24px margins)     │  ← dynamic
//  ├─────────────────────────────────────────────────────────────────┤
//  │  y=178..204 DATA ROW — "RECEIVED" label + value, right-aligned  │  ← dynamic
//  ├─────────────────────────────────────────────────────────────────┤
//  │  y=212..242 STATUS CHIP — full width, state colour              │  ← dynamic
//  ├─────────────────────────────────────────────────────────────────┤
//  │  y=248..320 RED WARNING BAND (C_RED fill, white text)           │
//  └─────────────────────────────────────────────────────────────────┘
//
// Static regions: header band (y 0..72), warning band (y 248..320)
// Dynamic regions: y 73..247  — cleared & redrawn per update, no overlap

static void drawOtaScreenHeader()
{
    tft.fillScreen(C_OTA_BG);

    // ── Dark header band (y 0..72) ──
    tft.fillRect(0, 0, SCREEN_W, 73, C_BLUE);

    // Icon: white circle left side, with a simple down-arrow inside
    tft.fillCircle(36, 36, 22, TFT_WHITE);
    // Arrow shaft
    for(int t = -1; t <= 1; t++)
        tft.drawFastVLine(36+t, 22, 16, C_BLUE);
    // Arrowhead
    tft.fillTriangle(24,37, 48,37, 36,50, C_BLUE);

    // Title text
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, C_BLUE);
    tft.setTextSize(2);
    tft.drawString("FIRMWARE UPDATE", 70, 26);

    // Subtitle
    tft.setTextSize(1);
    tft.setTextColor(C_CHIP_BLUE_BG, C_BLUE);   // light blue on dark blue
    tft.drawString("Keep device powered and BLE connected", 70, 46);

    // Version chip — white outline, white text, transparent-ish via track colour
    // Use a white-fill chip with blue text so it pops on the dark band
    drawChip(SCREEN_W-74, 8, 62, 22, 6,
             TFT_WHITE, C_BLUE, FW_VERSION, 1);

    // Thin separator at bottom of header (slightly darker blue line)
    tft.drawFastHLine(0, 72, SCREEN_W, C_ROUTE_SHAD);

    // ── Red warning band (y 248..320, static) ──
    tft.fillRect(0, 248, SCREEN_W, SCREEN_H - 248, C_RED);
    // Warning icon: exclamation in a circle
    tft.fillCircle(30, 284, 16, TFT_WHITE);
    tft.fillRect(28, 274, 4, 10, C_RED);
    tft.fillCircle(30, 288, 2, C_RED);
    // Text
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, C_RED);
    tft.setTextSize(1);
    tft.drawString("Do not power off or disconnect BLE", 54, 284);
}

// Dynamic-only repaint — only y=73..247, no overlap with static bands.
static void drawOtaScreenDynamic(int chunksRcvd, int chunkTotal,
                                  size_t bytesWritten, const char *statusMsg,
                                  bool isError)
{
    // Clear dynamic zone in one shot
    tft.fillRect(0, 73, SCREEN_W, 175, C_OTA_BG);

    float pct = (chunkTotal > 0) ? (float)chunksRcvd / (float)chunkTotal : 0.0f;
    int   pctI = (int)(pct * 100.0f);
    uint16_t accentCol = isError ? C_RED : C_BLUE;

    // ── Hero zone: giant pct numeral (y ≈ 80..148) ──
    char heroBuf[8]; snprintf(heroBuf, sizeof(heroBuf), "%d%%", pctI);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(accentCol, C_OTA_BG);
    tft.setTextSize(6);
    tft.drawString(heroBuf, SCREEN_W/2, 108);

    // "transferred" label beneath the numeral
    tft.setTextSize(1);
    tft.setTextColor(C_BLUE, C_OTA_BG);
    tft.drawString("transferred", SCREEN_W/2, 148);

    // ── Progress bar (y=154, h=16) ──
    uint16_t barFill = isError ? C_RED : C_BLUE;
    drawProgressBar(24, 158, SCREEN_W-48, 12, pctI, barFill, C_BAR_TRACK);

    // ── Data row: RECEIVED label + value, left + right aligned (y=178..204) ──
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_BLUE, C_OTA_BG);
    tft.setTextSize(1);
    tft.drawString("RECEIVED", 24, 180);

    char bytesBuf[20];
    if(bytesWritten < 1024)
        snprintf(bytesBuf, sizeof(bytesBuf), "%u B",    (unsigned)bytesWritten);
    else
        snprintf(bytesBuf, sizeof(bytesBuf), "%.1f KB", bytesWritten/1024.0f);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_BLACK, C_OTA_BG);
    tft.setTextSize(2);
    tft.drawString(bytesBuf, SCREEN_W-24, 182);

    // Thin divider line under data row
    tft.drawFastHLine(24, 200, SCREEN_W-48, C_PANEL_LINE);

    // ── Status chip (y=210..238) ──
    uint16_t chipBg, chipTxt;
    if(isError){
        chipBg  = C_CHIP_RED_BG;
        chipTxt = C_RED;
    } else if(pctI >= 100){
        chipBg  = C_CHIP_GREEN_BG;
        chipTxt = C_GREEN;
    } else {
        chipBg  = C_CHIP_BLUE_BG;
        chipTxt = C_BLUE;
    }
    tftRoundRect(24, 210, SCREEN_W-48, 30, 8, chipBg);
    tft.fillCircle(46, 225, 5, chipTxt);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(chipTxt, chipBg);
    tft.setTextSize(1);
    tft.drawString(statusMsg, 58, 225);
}

// Public entry point for OTA screen
// FIX 7: on first call draws full layout; subsequent calls only refresh
//         the dynamic regions — no tft.fillScreen() after the first frame.
static void drawOtaScreen(int chunksRcvd, int chunkTotal,
                           size_t bytesWritten, const char *statusMsg,
                           bool isError)
{
    if(!otaHeaderDrawn){
        drawOtaScreenHeader();
        otaHeaderDrawn = true;
    }
    drawOtaScreenDynamic(chunksRcvd, chunkTotal, bytesWritten, statusMsg, isError);
}

// -----------------------------------------------------------------------------
// OTA abort - CPU0 only
// -----------------------------------------------------------------------------
static void otaAbortCPU0(const char *reason)
{
    Serial.printf("[OTA] ABORT: %s\n", reason);
    Update.abort();
    otaState       = OTA_ERROR;
    otaIsError     = true;
    otaStartQueued = false;
    otaHeaderDrawn = false;   // reset so a clean error screen can be drawn
    strncpy(otaStatusMsg, reason, sizeof(otaStatusMsg)-1);
    drawOtaScreen(otaChunksRcvd, otaChunkCount, otaTotalBytes, otaStatusMsg, true);
    delay(3000);
    ESP.restart();
}

// -----------------------------------------------------------------------------
// Route / GPS handlers  - called from bleProcessTask (CPU1)
// -----------------------------------------------------------------------------
static void onRouteStart(JsonDocument &d)
{
    route.clear(); routeComplete=false;
    vp.ready=false; tripKm=0; lastTripValid=false;
    nearestIdx=-1; nearestDistM=1e9f; snapped=false;
    routeExpected = d["totalWaypoints"]|0;
    if(routeExpected>0) route.reserve((size_t)routeExpected);
    routeChanged = true;
    Serial.printf("[route_start] expecting %d wp\n",routeExpected);
}

static void onRouteChunk(const char *raw)
{
    JsonDocument doc;
    if(deserializeJson(doc, raw)){
        Serial.println("[JSON] route_chunk parse error");
        return;
    }
    JsonArray arr = doc["waypoints"].as<JsonArray>();
    for(JsonObject wp : arr)
        route.push_back({wp["lat"].as<float>(), wp["lon"].as<float>()});
    buildVP();
    routeChanged = true;
}

static void onRouteComplete(JsonDocument &d)
{
    routeComplete        = true;
    routeJustCompleted   = true;   // one-shot draw trigger; cleared by drawMap()
    buildVP();
    buildTurns();
    gpsChanged   = true;
    routeChanged = true;
    Serial.printf("[route_complete] %d wp\n",(int)route.size());
}

static void onGps(JsonDocument &d)
{
    float la=d["coordinates"]["latitude"] |0.0f;
    float lo=d["coordinates"]["longitude"]|0.0f;
    float sp=d["speed"]   |0.0f;
    float hd=d["heading"] |0.0f;
    if(la==0.0f&&lo==0.0f){ gpsValid=false; return; }

    if(lastTripValid){
        float dist=haversineKm(lastTripLat,lastTripLon,la,lo);
        if(dist>0.0005f&&dist<0.5f) tripKm+=dist;
    }
    lastTripLat=la; lastTripLon=lo; lastTripValid=true;

    if(!route.empty()){
        float best=1e9f; int bestI=0;
        for(int i=0;i<(int)route.size();i++){
            float dist=haversineM(la,lo,route[i].lat,route[i].lon);
            if(dist<best){best=dist;bestI=i;}
        }
        nearestIdx=bestI; nearestDistM=best; snapped=(best<=SNAP_M);
    }

    gpsLat=la; gpsLon=lo; gpsSpeed=sp; gpsHeading=hd;
    gpsValid=true;
    zoomedMode=true;
    lastGpsMs=millis();
    gpsChanged = true;
}

// -----------------------------------------------------------------------------
// OTA handlers
// -----------------------------------------------------------------------------
static void onFwUpdateStart(JsonDocument &d)
{
    int cnt = d["chunkCount"] | 0;
    if(cnt <= 0){ Serial.println("[OTA] invalid chunkCount"); return; }
    cpu1OtaChunkCount = cnt;
    chunksQueued      = 0;
    OtaStartMsg msg;
    msg.chunkCount = cnt;
    if(xQueueSend(otaStartQueue, &msg, 0) != pdTRUE){
        Serial.println("[OTA] otaStartQueue full - ignoring start");
        cpu1OtaChunkCount = 0;
        return;
    }
    otaStartQueued = true;
    Serial.printf("[OTA] start signal queued, %d chunks\n", cnt);
}

static void hexDump(const char *prefix, const char *buf, int len, int maxBytes = 256)
{
    int limit = (len < maxBytes) ? len : maxBytes;
    for(int row = 0; row < limit; row += 16){
        Serial.printf("%s %04x: ", prefix, row);
        for(int col = 0; col < 16; col++){
            if(row+col < limit) Serial.printf("%02x ", (uint8_t)buf[row+col]);
            else                Serial.print("   ");
        }
        Serial.print(" |");
        for(int col = 0; col < 16 && row+col < limit; col++){
            char c = buf[row+col];
            Serial.print((c >= 0x20 && c < 0x7f) ? c : '.');
        }
        Serial.println("|");
    }
    if(len > maxBytes)
        Serial.printf("%s ... (%d bytes total, %d shown)\n", prefix, len, maxBytes);
}

static void onFwChunk(const char *raw)
{
    int pktIndex = -1;
    const char *idxTag = strstr(raw, "\"index\":");
    if(idxTag) pktIndex = atoi(idxTag + 8);

    int pendingChunk = chunksQueued + 1;
    bool inWindow = (pendingChunk >= 19 && pendingChunk <= 25);
    int  rawLen   = (int)strlen(raw);

    Serial.printf("[PKT] seq=%d pkt_index=%d rawLen=%d\n",
                  pendingChunk, pktIndex, rawLen);
    if(inWindow) hexDump("[PKT]", raw, rawLen);

    if(pktIndex >= 0 && pktIndex != (pendingChunk - 1))
        Serial.printf("[PKT] WARNING: expected index %d, got %d\n",
                      pendingChunk - 1, pktIndex);

    if(otaState != OTA_RECEIVING && !otaStartQueued){
        Serial.println("[OTA] chunk received but OTA not started - dropped");
        return;
    }
    if(cpu1OtaChunkCount <= 0){
        Serial.println("[OTA] chunk arrived before valid start - dropped");
        return;
    }

    const char *sizeTag = strstr(raw, "\"size\":");
    if(!sizeTag){
        OtaBlock errBlock{}; errBlock.len = 0xFFFF;
        xQueueSend(otaWriteQueue, &errBlock, 0);
        return;
    }
    int declSize = atoi(sizeTag + 7);
    if(declSize <= 0 || declSize > OTA_BUF_SIZE){
        OtaBlock errBlock{}; errBlock.len = 0xFFFF;
        xQueueSend(otaWriteQueue, &errBlock, 0);
        return;
    }

    const char *dataTag = strstr(raw, "\"data\":\"");
    if(!dataTag){
        OtaBlock errBlock{}; errBlock.len = 0xFFFF;
        xQueueSend(otaWriteQueue, &errBlock, 0);
        return;
    }
    const char *b64start = dataTag + 8;
    const char *b64end   = strchr(b64start, '"');
    if(!b64end){
        OtaBlock errBlock{}; errBlock.len = 0xFFFF;
        xQueueSend(otaWriteQueue, &errBlock, 0);
        return;
    }

    static const char b64t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t inLen  = (size_t)(b64end - b64start);
    size_t outLen = (inLen / 4) * 3;
    if(inLen >= 1 && b64start[inLen-1] == '=') outLen--;
    if(inLen >= 2 && b64start[inLen-2] == '=') outLen--;

    if(outLen > OTA_BUF_SIZE){
        OtaBlock errBlock{}; errBlock.len = 0xFFFF;
        xQueueSend(otaWriteQueue, &errBlock, 0);
        return;
    }

    OtaBlock block{};
    size_t o = 0;
    for(size_t i = 0; i < inLen && o < outLen; i += 4){
        uint32_t s[4];
        for(int j = 0; j < 4; j++){
            const char *p = strchr(b64t, b64start[i+j]);
            s[j] = (b64start[i+j] == '=') ? 0 : (p ? (uint32_t)(p - b64t) : 0);
        }
        uint32_t triple = (s[0]<<18)|(s[1]<<12)|(s[2]<<6)|s[3];
        if(o < outLen) block.data[o++] = (triple >> 16) & 0xFF;
        if(o < outLen) block.data[o++] = (triple >>  8) & 0xFF;
        if(o < outLen) block.data[o++] =  triple        & 0xFF;
    }
    block.len = (uint16_t)o;

    chunksQueued++;
    int chunkNum = chunksQueued;
    block.isLast = (chunkNum >= cpu1OtaChunkCount);
    if(block.isLast){
        Serial.printf("[OTA] chunk %d/%d decoded %u B  *** LAST ***\n",
                      chunkNum, cpu1OtaChunkCount, (unsigned)block.len);
        chunksQueued      = 0;
        cpu1OtaChunkCount = 0;
    } else {
        Serial.printf("[OTA] chunk %d/%d decoded %u B\n",
                      chunkNum, cpu1OtaChunkCount, (unsigned)block.len);
    }

    if(xQueueSend(otaWriteQueue, &block, 0) != pdTRUE)
        Serial.printf("[OTA] write queue full - chunk %d dropped\n", chunkNum);
}

// -----------------------------------------------------------------------------
// processJSON  - called from bleProcessTask (CPU1)
// -----------------------------------------------------------------------------
static void processJSON(const char *raw, size_t /*len*/)
{
    if(strstr(raw, "\"fw_chunk\"")){
        onFwChunk(raw);
        return;
    }

    JsonDocument doc;
    if(deserializeJson(doc, raw)){
        Serial.println("[JSON] parse error");
        return;
    }

    const char *t = doc["type"] | (const char*)nullptr;
    if(t){
        if     (strcmp(t,"route_start")    ==0) onRouteStart(doc);
        else if(strcmp(t,"route_chunk")    ==0) onRouteChunk(raw);
        else if(strcmp(t,"route_complete") ==0) onRouteComplete(doc);
        else if(strcmp(t,"start_fw_update")==0) onFwUpdateStart(doc);
        else if(strcmp(t,"version")        ==0){
            char resp[64];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"version\",\"version\":\"%s\"}", FW_VERSION);
            pChar->setValue(resp);
            pChar->notify();
            Serial.printf("[BLE] version requested -> %s\n", resp);
        }
        else Serial.printf("[JSON] unknown type: %s\n",t);
    } else if(doc["coordinates"].is<JsonObject>()){
        onGps(doc);
    } else {
        Serial.println("[JSON] unrecognised packet");
    }
}

// -----------------------------------------------------------------------------
// BLE server callbacks
// -----------------------------------------------------------------------------
class SrvCB : public BLEServerCallbacks {
    void onConnect(BLEServer*) override {
        bleCon           = true;
        bleJustConnected = true;
        // FIX 1: trigger an immediate map redraw so the "No route loaded"
        //        state is shown as soon as the app connects, replacing the
        //        static waiting screen without needing any data packet first.
        routeChanged     = true;
        Serial.println("[BLE] connected");
    }
    void onDisconnect(BLEServer*) override {
        bleCon = false;
        Serial.println("[BLE] disconnected");
    }
};

class ChrCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        std::string v = c->getValue();
        if(v.empty()) return;
        uint32_t head = ringHead;
        uint32_t next = (head + 1) % RING_SLOTS;
        if(next == ringTail){ Serial.println("[BLE] ring full - packet dropped"); return; }
        size_t len = v.size() < (BLE_CHUNK_DATA - 1) ? v.size() : (BLE_CHUNK_DATA - 1);
        memcpy(ring[head].data, v.c_str(), len);
        ring[head].data[len] = '\0';
        ring[head].len = (uint16_t)len;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        ringHead = next;
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(bleProcessTaskHandle, &woken);
        portYIELD_FROM_ISR(woken);
    }
};

// -----------------------------------------------------------------------------
// BLE processing task - CPU1
// FIX 6: bleJustConnected check moved to TOP of loop body, before ring drain,
//         so any bytes that arrived in the same tick as the connect event are
//         discarded rather than forwarded to processJSON with stale state.
// -----------------------------------------------------------------------------
static void bleProcessTask(void*)
{
    bleProcessTaskHandle = xTaskGetCurrentTaskHandle();
    uint32_t lastHeapLog = 0;

    while(true){
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // FIX 6: reset accum BEFORE draining the ring on a fresh connection
        if(bleJustConnected){
            bleJustConnected = false;
            accumLen    = 0;
            accumBuf[0] = '\0';
            Serial.println("[BLE] accum reset on connect (ring preserved)");
        }

        while(ringTail != ringHead){
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            RingSlot &slot = ring[ringTail];
            ringTail = (ringTail + 1) % RING_SLOTS;

            Serial.printf("[RING] slot %u B  accumBefore=%d\n",
                          (unsigned)slot.len, accumLen);

            int space = (ACCUM_BUF - 1) - accumLen;
            if(space <= 0){
                Serial.printf("[BLE] accum overflow (%d B) - discarding frame\n", accumLen);
                accumLen    = 0;
                accumBuf[0] = '\0';
                space       = ACCUM_BUF - 1;
            }

            size_t copy = ((int)slot.len < space) ? slot.len : (size_t)space;
            memcpy(accumBuf + accumLen, slot.data, copy);
            accumLen += (int)copy;
            accumBuf[accumLen] = '\0';

            int  depth  = 0;
            bool inStr  = false;
            bool escape = false;
            for(int i = 0; i < accumLen; i++){
                char ch = accumBuf[i];
                if(escape){ escape=false; continue; }
                if(ch=='\\'){ escape=true; continue; }
                if(ch=='"'){ inStr=!inStr; continue; }
                if(inStr) continue;
                if(ch=='{') depth++;
                if(ch=='}') depth--;
            }

            if(depth == 0 && accumLen > 0){
                processJSON(accumBuf, (size_t)accumLen);
                accumLen    = 0;
                accumBuf[0] = '\0';
            } else if(depth < 0){
                accumLen    = 0;
                accumBuf[0] = '\0';
            }
        }

        if(otaState == OTA_RECEIVING){
            uint32_t now = millis();
            if(now - lastHeapLog > 500){
                Serial.printf("[OTA-MEM] heap=%u chunk=%d\n",
                              ESP.getFreeHeap(), otaChunksRcvd);
                lastHeapLog = now;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// resetAll - CPU0 only
// -----------------------------------------------------------------------------
static void resetAll()
{
    route.clear(); turnIndices.clear();
    routeExpected=0; routeComplete=false; routeChanged=true;
    vp.ready=false; tripKm=0; lastTripValid=false;
    nearestIdx=-1; nearestDistM=1e9f; snapped=false;
    gpsValid=false; gpsChanged=true; zoomedMode=false; lastGpsMs=0;
    doResetAll=false;
    Serial.println("[reset] no GPS for 2s - state cleared");
}

// -----------------------------------------------------------------------------
// Waiting screen  - shown when no route and no GPS, before any BLE connection
// FIX 3: fill width is MAP_W-1 (not MAP_W) to avoid overwriting the separator
//         line drawn by initPanel(); drawSeparator() is called after the fill
//         to ensure the divider is always intact regardless of call order.
// -----------------------------------------------------------------------------
static void drawWaitingScreen()
{
    // FIX 3: clamp fill to MAP_W-1 so the separator at x=INFO_X is untouched
    tft.fillRect(0, 0, MAP_W-1, MAP_H, TFT_WHITE);
    // Restore separator in case any earlier draw clipped it
    drawSeparator();

    int cx = MAP_W / 2;
    int cy = MAP_H / 2;

    tftRoundRect(cx-22, cy-60, 44, 44, 10, C_CHIP_BLUE_BG);

    tft.drawCircle(cx, cy-38, 14, C_BLUE);
    tft.drawCircle(cx, cy-38, 10, C_BLUE);
    tft.fillCircle(cx, cy-38, 5,  C_BLUE);
    tft.fillCircle(cx, cy-38, 2,  TFT_WHITE);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextSize(2);
    tft.drawString("No Connection", cx, cy - 4);

    tft.setTextSize(1);
    tft.setTextColor(C_BLUE, TFT_WHITE);
    tft.drawString("Open the app and connect via BLE", cx, cy + 18);
}

// -----------------------------------------------------------------------------
// Boot screen
// -----------------------------------------------------------------------------
static void showBootScreen()
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

    drawChip(cx - 40, cy + 22, 80, 20, 5,
             C_CHIP_BLUE_BG, C_BLUE, FW_VERSION, 1);

    delay(2500);
}

// -----------------------------------------------------------------------------
// Setup  - CPU0
// FIX 4: removed first initPanel() call that caused a double-paint flash.
//         Single initPanel() call after drawWaitingScreen() is the only one.
// FIX 2: ensureSprite() moved here (one-shot) and removed from drawMap().
// -----------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200); delay(300);
    Serial.println("\n=== GPS Navigator " FW_VERSION " ===");
    Serial.printf("[MEM] boot heap=%u  psram=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    tft.begin();
    tft.setRotation(1);
    showBootScreen();
    tft.fillScreen(C_MAP_BG);

    uint16_t cal[5]={323,3485,405,3210,7};
    tft.setTouch(cal);

    // FIX 4: initPanel() removed from here — only called once after
    //         drawWaitingScreen() below, avoiding the double-paint flash.

    otaStartQueue = xQueueCreate(2,  sizeof(OtaStartMsg));
    otaWriteQueue = xQueueCreate(64, sizeof(OtaBlock));

    xTaskCreatePinnedToCore(bleProcessTask, "bleProc", 32768,
                            nullptr, 1, nullptr, 1);
    delay(10);

    BLEDevice::init(DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new SrvCB());
    BLEService *svc = pServer->createService(SERVICE_UUID);
    pChar = svc->createCharacteristic(CHARACTERISTIC_UUID,
            BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    pChar->setCallbacks(new ChrCB());
    pChar->addDescriptor(new BLE2902());
    pChar->setValue("{\"status\":\"ready\"}");
    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.printf("[MEM] after BLE init - heap=%u  psram=%u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());

    // FIX 2: allocate sprite once here; drawMap() must NOT call ensureSprite()
    ensureSprite();

    analogSetAttenuation(ADC_11db);
    analogSetWidth(12);
    pinMode(BAT_PIN, INPUT);
    readBattery();
    batStartMs = millis();

    // Single panel init + waiting screen — no duplicate calls
    drawWaitingScreen();
    initPanel();
    updatePanel();

    Serial.printf("[BLE] advertising as \"%s\" . heap=%u  psram=%u\n",
                  DEVICE_NAME, ESP.getFreeHeap(), ESP.getFreePsram());
}

// -----------------------------------------------------------------------------
// Loop  - CPU0
// FIX 5: BLE reconnect no longer uses delay(200); uses a non-blocking timer.
// -----------------------------------------------------------------------------
void loop()
{
    static uint32_t lastMap=0, lastPanel=0;
    static bool     wasConnected = false;
    uint32_t now = millis();

    // -- OTA begin -------------------------------------------------------------
    {
        OtaStartMsg msg = {};
        if(xQueueReceive(otaStartQueue, &msg, 0) == pdTRUE){
            if(otaState != OTA_IDLE){
                Serial.println("[OTA] already in progress, ignoring start");
            } else if(msg.chunkCount > 0){
                releaseSprite();
                otaHeaderDrawn = false;   // ensure fresh header on new OTA
                Serial.printf("[OTA] Update.begin(UNKNOWN) heap=%u\n",
                              ESP.getFreeHeap());
                if(!Update.begin(UPDATE_SIZE_UNKNOWN)){
                    ensureSprite();
                    otaAbortCPU0("OTA begin failed");
                    return;
                }
                otaChunkCount  = msg.chunkCount;
                otaChunksRcvd  = 0;
                otaTotalBytes  = 0;
                otaIsError     = false;
                strncpy(otaStatusMsg, "Receiving firmware...", sizeof(otaStatusMsg)-1);
                otaState       = OTA_RECEIVING;
                otaStartQueued = false;
                drawOtaScreen(0, otaChunkCount, 0, otaStatusMsg, false);
                Serial.printf("[OTA] Update.begin() OK . heap=%u . %d chunks\n",
                              ESP.getFreeHeap(), otaChunkCount);
            }
        }
    }

    // -- OTA write drain -------------------------------------------------------
    if(otaState == OTA_RECEIVING){
        OtaBlock block;
        while(xQueueReceive(otaWriteQueue, &block, 0) == pdTRUE){
            if(block.len == 0xFFFF){
                ensureSprite();
                otaAbortCPU0("Bad chunk payload from CPU1");
                return;
            }
            size_t written = Update.write(block.data, block.len);
            if(written != block.len){
                ensureSprite();
                otaAbortCPU0("Write length mismatch");
                return;
            }
            otaTotalBytes += written;
            otaChunksRcvd += 1;

            if(otaChunksRcvd % 50 == 0 || otaChunksRcvd == otaChunkCount)
                drawOtaScreen(otaChunksRcvd, otaChunkCount,
                              otaTotalBytes, otaStatusMsg, false);

            if(block.isLast){
                otaState = OTA_APPLYING;
                strncpy(otaStatusMsg, "Applying update...", sizeof(otaStatusMsg)-1);
                drawOtaScreen(otaChunksRcvd, otaChunkCount,
                              otaTotalBytes, otaStatusMsg, false);
                if(!Update.end(true)){
                    ensureSprite();
                    otaAbortCPU0("OTA end/verify failed");
                    return;
                }
                otaState = OTA_DONE;
                strncpy(otaStatusMsg, "Update complete! Rebooting...", sizeof(otaStatusMsg)-1);
                drawOtaScreen(otaChunksRcvd, otaChunkCount,
                              otaTotalBytes, otaStatusMsg, false);
                Serial.println("[OTA] success - rebooting in 1.5s");
                delay(1500);
                ESP.restart();
            }
        }
    }

    if(otaState != OTA_IDLE) return;

    // -- GPS timeout -----------------------------------------------------------
    if(zoomedMode && lastGpsMs > 0 && (now - lastGpsMs) > GPS_TIMEOUT_MS)
        resetAll();

    // -- BLE reconnect (non-blocking) ------------------------------------------
    // FIX 5: replaced delay(200) with a non-blocking 200 ms timer so the
    //         render loop is never stalled waiting for advertising to restart.
    if(!bleCon && bleWasCon && !bleAdvPending){
        bleAdvPending   = true;
        bleAdvPendingMs = now;
    }
    if(bleAdvPending && !bleCon && (now - bleAdvPendingMs) >= 200){
        BLEDevice::startAdvertising();
        bleAdvPending = false;
        bleWasCon     = false;
    }
    if(bleCon && !bleWasCon) bleWasCon = true;

    // -- Waiting screen when disconnected and no route -------------------------
    if(!bleCon && !gpsValid && route.empty() && wasConnected != bleCon){
        drawWaitingScreen();
        // Restore panel after waiting screen fill (FIX 3 companion)
        initPanel();
        updatePanel();
        wasConnected = bleCon;
    }
    if(bleCon) wasConnected = true;

    // -- Map -------------------------------------------------------------------
    if(now-lastMap > 50 && (routeChanged||gpsChanged||routeJustCompleted)){
        drawMap();
        routeChanged        = false;
        gpsChanged          = false;
        routeJustCompleted  = false;
        lastMap=now;
    }

    // -- Battery ---------------------------------------------------------------
    if(now-lastBatMs > BAT_INTERVAL_MS){
        readBattery(); lastBatMs=now;
    }

    // -- Panel -----------------------------------------------------------------
    if(now-lastPanel > 250){
        updatePanel(); lastPanel=now;
    }

    yield();
}