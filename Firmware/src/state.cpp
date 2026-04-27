/**
 * state.cpp
 * Definitions for all shared mutable state declared in state.h.
 */

#include "state.h"
#include "config.h"
#include <BLEDevice.h>

// -----------------------------------------------------------------------------
// Display objects
// -----------------------------------------------------------------------------
TFT_eSPI    tft;
TFT_eSprite mapSprite(&tft);
bool        spriteOk    = false;
bool        spriteTried = false;
int         spriteBpp = 0;

// -----------------------------------------------------------------------------
// BLE
// -----------------------------------------------------------------------------
BLEServer         *pServer   = nullptr;
BLECharacteristic *pChar     = nullptr;
volatile bool      bleCon    = false;
bool               bleWasCon = false;
bool               bleAdvPending    = false;
uint32_t           bleAdvPendingMs  = 0;

// -----------------------------------------------------------------------------
// Ring buffer
// -----------------------------------------------------------------------------
RingSlot          ring[RING_SLOTS];
volatile uint32_t ringHead = 0;
volatile uint32_t ringTail = 0;
volatile bool     bleJustConnected = false;
TaskHandle_t      bleProcessTaskHandle = nullptr;

// -----------------------------------------------------------------------------
// JSON accumulation buffer
// -----------------------------------------------------------------------------
char accumBuf[ACCUM_BUF];
int  accumLen = 0;

// -----------------------------------------------------------------------------
// OTA
// -----------------------------------------------------------------------------
QueueHandle_t otaStartQueue = nullptr;
QueueHandle_t otaWriteQueue = nullptr;
OtaState      otaState      = OTA_IDLE;
volatile bool otaStartQueued    = false;
int           otaChunkCount     = 0;
int           otaChunksRcvd     = 0;
size_t        otaTotalBytes     = 0;
bool          otaIsError        = false;
char          otaStatusMsg[64]  = "Receiving firmware...";
int           cpu1OtaChunkCount = 0;
int           chunksQueued      = 0;
bool          otaHeaderDrawn    = false;

// -----------------------------------------------------------------------------
// Route / GPS
// -----------------------------------------------------------------------------
std::vector<WP> route;
int             routeExpected = 0;
bool            routeComplete = false;
volatile bool   routeChanged       = false;
volatile bool   gpsChanged         = false;
volatile bool   doResetAll         = false;
volatile bool   routeJustCompleted = false;

float gpsLat = 0, gpsLon = 0;
float gpsSpeed = 0, gpsHeading = 0;
bool  gpsValid = false;

bool     zoomedMode  = false;
float    zoomRadiusM = 300.0f;
uint32_t lastGpsMs   = 0;

std::vector<int> turnIndices;

VP vp;

int   nearestIdx   = -1;
float nearestDistM = 1e9f;
bool  snapped      = false;

float tripKm        = 0;
float lastTripLat   = 0, lastTripLon = 0;
bool  lastTripValid = false;

// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------
float    batVoltage = BAT_NOM_V;
int      batPercent = -1;
uint32_t lastBatMs  = 0;
uint32_t batStartMs = 0;