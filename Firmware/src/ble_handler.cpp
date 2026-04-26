/**
 * ble_handler.cpp
 * BLE server/characteristic callbacks and the CPU1 ring-buffer processing task.
 * JSON routing is delegated to protocol_handler.cpp.
 */

#include "ble_handler.h"
#include "state.h"
#include "config.h"
#include "protocol_handler.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// =============================================================================
// BLE server callbacks
// =============================================================================

class SrvCB : public BLEServerCallbacks {
    void onConnect(BLEServer*) override {
        bleCon           = true;
        bleJustConnected = true;
        // Trigger an immediate map redraw so "No route loaded" appears
        // as soon as the app connects, without waiting for a data packet.
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
        if(next == ringTail){
            Serial.println("[BLE] ring full - packet dropped");
            return;
        }
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

void bleSetupCallbacks()
{
    pServer->setCallbacks(new SrvCB());
    // Characteristic callbacks are set by the caller in main setup()
    // after pChar is created; expose a helper if needed in future.
}

// =============================================================================
// CPU1 processing task
// =============================================================================

void bleProcessTask(void*)
{
    bleProcessTaskHandle = xTaskGetCurrentTaskHandle();
    uint32_t lastHeapLog = 0;

    while(true){
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Reset accumulator before draining the ring on a fresh connection
        // so stale partial JSON from a previous session is discarded.
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

            // Scan for complete JSON object
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