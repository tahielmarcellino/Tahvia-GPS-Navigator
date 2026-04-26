#pragma once
/**
 * state.h
 * Extern declarations for all shared mutable state.
 * Include this wherever cross-module state access is needed.
 * The actual definitions live in state.cpp.
 */

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// -----------------------------------------------------------------------------
// Display objects
// -----------------------------------------------------------------------------
extern TFT_eSPI    tft;
extern TFT_eSprite mapSprite;
extern bool        spriteOk;
extern bool        spriteTried;

// -----------------------------------------------------------------------------
// BLE
// -----------------------------------------------------------------------------
#include <BLEServer.h>
#include <BLECharacteristic.h>

extern BLEServer         *pServer;
extern BLECharacteristic *pChar;
extern volatile bool      bleCon;
extern bool               bleWasCon;
extern bool               bleAdvPending;
extern uint32_t           bleAdvPendingMs;

// -----------------------------------------------------------------------------
// Ring buffer
// -----------------------------------------------------------------------------
struct RingSlot {
    char     data[512];   // BLE_CHUNK_DATA
    uint16_t len;
};

extern RingSlot          ring[8];         // RING_SLOTS
extern volatile uint32_t ringHead;
extern volatile uint32_t ringTail;
extern volatile bool     bleJustConnected;
extern TaskHandle_t      bleProcessTaskHandle;

// -----------------------------------------------------------------------------
// JSON accumulation buffer
// -----------------------------------------------------------------------------
#define ACCUM_BUF 1024
extern char accumBuf[ACCUM_BUF];
extern int  accumLen;

// -----------------------------------------------------------------------------
// OTA
// -----------------------------------------------------------------------------
struct OtaStartMsg { int chunkCount; };

struct OtaBlock {
    uint8_t  data[192];  // OTA_BUF_SIZE
    uint16_t len;
    bool     isLast;
};

enum OtaState { OTA_IDLE, OTA_RECEIVING, OTA_APPLYING, OTA_DONE, OTA_ERROR };

extern QueueHandle_t otaStartQueue;
extern QueueHandle_t otaWriteQueue;
extern OtaState      otaState;
extern volatile bool otaStartQueued;
extern int           otaChunkCount;
extern int           otaChunksRcvd;
extern size_t        otaTotalBytes;
extern bool          otaIsError;
extern char          otaStatusMsg[64];
extern int           cpu1OtaChunkCount;
extern int           chunksQueued;
extern bool          otaHeaderDrawn;

// -----------------------------------------------------------------------------
// Route / GPS
// -----------------------------------------------------------------------------
struct WP { float lat, lon; };

extern std::vector<WP> route;
extern int             routeExpected;
extern bool            routeComplete;
extern volatile bool   routeChanged;
extern volatile bool   gpsChanged;
extern volatile bool   doResetAll;
extern volatile bool   routeJustCompleted;

extern float gpsLat, gpsLon;
extern float gpsSpeed, gpsHeading;
extern bool  gpsValid;

extern bool     zoomedMode;
extern float    zoomRadiusM;
extern uint32_t lastGpsMs;

extern std::vector<int> turnIndices;

struct VP { float minLat,maxLat,minLon,maxLon; bool ready=false; };
extern VP vp;

extern int   nearestIdx;
extern float nearestDistM;
extern bool  snapped;

extern float tripKm;
extern float lastTripLat, lastTripLon;
extern bool  lastTripValid;

// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------
extern float    batVoltage;
extern int      batPercent;
extern uint32_t lastBatMs;
extern uint32_t batStartMs;