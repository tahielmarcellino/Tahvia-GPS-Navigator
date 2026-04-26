/**
 * main.cpp  —  gps_navigator v1.1.7
 *
 * Contains only setup() and loop(). All logic lives in:
 *   config.h           compile-time constants & palette
 *   state.h / .cpp     shared mutable state
 *   geo.h / .cpp       geographic helpers & viewport builders
 *   display.h / .cpp   all rendering (map, panel, OTA, screens)
 *   battery.h / .cpp   ADC sampling & battery percentage
 *   protocol_handler.h / .cpp  BLE JSON dispatcher & message handlers
 *   ble_handler.h / .cpp       BLE callbacks & CPU1 ring-buffer task
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Update.h>

#include "config.h"
#include "state.h"
#include "geo.h"
#include "display.h"
#include "battery.h"
#include "ble_handler.h"
#include "protocol_handler.h"

// =============================================================================
// resetAll  (CPU0 only — called on GPS timeout)
// =============================================================================
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

// =============================================================================
// setup  (CPU0)
// =============================================================================
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

    // Queues
    otaStartQueue = xQueueCreate(2,  sizeof(OtaStartMsg));
    otaWriteQueue = xQueueCreate(64, sizeof(OtaBlock));

    // BLE processing task on CPU1
    xTaskCreatePinnedToCore(bleProcessTask, "bleProc", 32768,
                            nullptr, 1, nullptr, 1);
    delay(10);

    // BLE stack
    BLEDevice::init(DEVICE_NAME);
    pServer = BLEDevice::createServer();
    bleSetupCallbacks();          // attach SrvCB

    BLEService *svc = pServer->createService(SERVICE_UUID);
    pChar = svc->createCharacteristic(CHARACTERISTIC_UUID,
            BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY);
    pChar->setCallbacks(new BLECharacteristicCallbacks()); // ChrCB lives in ble_handler
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

    // Sprite (allocated once here; drawMap must NOT call ensureSprite)
    ensureSprite();

    // Battery
    analogSetAttenuation(ADC_11db);
    analogSetWidth(12);
    pinMode(BAT_PIN, INPUT);
    readBattery();
    batStartMs = millis();

    // Initial UI
    drawWaitingScreen();
    initPanel();
    updatePanel();

    Serial.printf("[BLE] advertising as \"%s\" . heap=%u  psram=%u\n",
                  DEVICE_NAME, ESP.getFreeHeap(), ESP.getFreePsram());
}

// =============================================================================
// loop  (CPU0)
// =============================================================================
void loop()
{
    static uint32_t lastMap=0, lastPanel=0;
    static bool     wasConnected = false;
    uint32_t now = millis();

    // ── OTA begin ─────────────────────────────────────────────────────────────
    {
        OtaStartMsg msg = {};
        if(xQueueReceive(otaStartQueue, &msg, 0) == pdTRUE){
            if(otaState != OTA_IDLE){
                Serial.println("[OTA] already in progress, ignoring start");
            } else if(msg.chunkCount > 0){
                releaseSprite();
                otaHeaderDrawn = false;
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

    // ── OTA write drain ───────────────────────────────────────────────────────
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

    // ── GPS timeout ───────────────────────────────────────────────────────────
    if(zoomedMode && lastGpsMs > 0 && (now - lastGpsMs) > GPS_TIMEOUT_MS)
        resetAll();

    // ── BLE reconnect (non-blocking 200 ms timer) ─────────────────────────────
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

    // ── Waiting screen when disconnected and no route ─────────────────────────
    if(!bleCon && !gpsValid && route.empty() && wasConnected != bleCon){
        drawWaitingScreen();
        initPanel();
        updatePanel();
        wasConnected = bleCon;
    }
    if(bleCon) wasConnected = true;

    // ── Map (50 ms rate-limit, only on change) ────────────────────────────────
    if(now-lastMap > 50 && (routeChanged||gpsChanged||routeJustCompleted)){
        drawMap();
        routeChanged       = false;
        gpsChanged         = false;
        routeJustCompleted = false;
        lastMap = now;
    }

    // ── Battery ───────────────────────────────────────────────────────────────
    if(now-lastBatMs > BAT_INTERVAL_MS){
        readBattery(); lastBatMs=now;
    }

    // ── Panel (250 ms rate-limit) ─────────────────────────────────────────────
    if(now-lastPanel > 250){
        updatePanel(); lastPanel=now;
    }

    yield();
}