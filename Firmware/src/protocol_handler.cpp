/**
 * protocol_handler.cpp
 * Top-level JSON dispatcher and individual message handlers.
 * Runs on CPU1 (bleProcessTask); must not touch TFT directly.
 *
 * BLE protocol messages handled:
 *   route_start     {"type":"route_start","totalWaypoints":N,...}
 *   route_chunk     {"type":"route_chunk","chunkIndex":I,"waypoints":[...]}
 *   route_complete  {"type":"route_complete","totalWaypoints":N}
 *   gps update      {"coordinates":{"latitude":..,"longitude":..},"speed":..,"heading":..}
 *   start_fw_update {"type":"start_fw_update","chunkCount":N}
 *   fw_chunk        {"type":"fw_chunk","index":I,"size":S,"data":"<base64>"}
 *   version         {"type":"version"}
 */

#include "protocol_handler.h"
#include "state.h"
#include "config.h"
#include "geo.h"
#include <ArduinoJson.h>
#include <BLECharacteristic.h>
#include <freertos/queue.h>
#include <string.h>

// =============================================================================
// Debug helpers
// =============================================================================

static void hexDump(const char *prefix, const char *buf, int len, int maxBytes=256)
{
    int limit = (len < maxBytes) ? len : maxBytes;
    for(int row=0; row<limit; row+=16){
        Serial.printf("%s %04x: ", prefix, row);
        for(int col=0; col<16; col++){
            if(row+col < limit) Serial.printf("%02x ", (uint8_t)buf[row+col]);
            else                Serial.print("   ");
        }
        Serial.print(" |");
        for(int col=0; col<16 && row+col<limit; col++){
            char c = buf[row+col];
            Serial.print((c>=0x20 && c<0x7f) ? c : '.');
        }
        Serial.println("|");
    }
    if(len > maxBytes)
        Serial.printf("%s ... (%d bytes total, %d shown)\n", prefix, len, maxBytes);
}

// =============================================================================
// Route handlers
// =============================================================================

static void onRouteStart(JsonDocument &d)
{
    route.clear(); routeComplete=false;
    vp.ready=false; tripKm=0; lastTripValid=false;
    nearestIdx=-1; nearestDistM=1e9f; snapped=false;
    routeExpected = d["totalWaypoints"]|0;
    if(routeExpected>0) route.reserve((size_t)routeExpected);
    routeChanged = true;
    Serial.printf("[route_start] expecting %d wp\n", routeExpected);
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
    routeComplete      = true;
    routeJustCompleted = true;
    buildVP();
    buildTurns();
    gpsChanged   = true;
    routeChanged = true;
    Serial.printf("[route_complete] %d wp\n", (int)route.size());
}

// =============================================================================
// GPS handler
// =============================================================================

static void onGps(JsonDocument &d)
{
    float la = d["coordinates"]["latitude"]  | 0.0f;
    float lo = d["coordinates"]["longitude"] | 0.0f;
    float sp = d["speed"]   | 0.0f;
    float hd = d["heading"] | 0.0f;
    if(la==0.0f && lo==0.0f){ gpsValid=false; return; }

    if(lastTripValid){
        float dist = haversineKm(lastTripLat, lastTripLon, la, lo);
        if(dist>0.0005f && dist<0.5f) tripKm += dist;
    }
    lastTripLat=la; lastTripLon=lo; lastTripValid=true;

    if(!route.empty()){
        float best=1e9f; int bestI=0;
        for(int i=0; i<(int)route.size(); i++){
            float dist = haversineM(la, lo, route[i].lat, route[i].lon);
            if(dist < best){ best=dist; bestI=i; }
        }
        nearestIdx=bestI; nearestDistM=best; snapped=(best<=SNAP_M);
    }

    gpsLat=la; gpsLon=lo; gpsSpeed=sp; gpsHeading=hd;
    gpsValid  = true;
    zoomedMode= true;
    lastGpsMs = millis();
    gpsChanged= true;
}

// =============================================================================
// OTA handlers
// =============================================================================

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

static void onFwChunk(const char *raw)
{
    // --- Index check ---
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

    // --- Parse size ---
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

    // --- Extract base64 data ---
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

    // --- Decode base64 ---
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
    for(size_t i=0; i<inLen && o<outLen; i+=4){
        uint32_t s[4];
        for(int j=0; j<4; j++){
            const char *p = strchr(b64t, b64start[i+j]);
            s[j] = (b64start[i+j]=='=') ? 0 : (p ? (uint32_t)(p-b64t) : 0);
        }
        uint32_t triple = (s[0]<<18)|(s[1]<<12)|(s[2]<<6)|s[3];
        if(o<outLen) block.data[o++] = (triple>>16)&0xFF;
        if(o<outLen) block.data[o++] = (triple>> 8)&0xFF;
        if(o<outLen) block.data[o++] =  triple     &0xFF;
    }
    block.len = (uint16_t)o;

    // --- Enqueue ---
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

// =============================================================================
// Top-level JSON dispatcher
// =============================================================================

void processJSON(const char *raw, size_t /*len*/)
{
    // fw_chunk is performance-sensitive; parse without ArduinoJson
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
        else Serial.printf("[JSON] unknown type: %s\n", t);
    } else if(doc["coordinates"].is<JsonObject>()){
        onGps(doc);
    } else {
        Serial.println("[JSON] unrecognised packet");
    }
}