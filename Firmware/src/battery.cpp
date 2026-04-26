/**
 * battery.cpp
 * Battery voltage sampling and percentage calculation.
 */

#include "battery.h"
#include "state.h"
#include "config.h"
#include <Arduino.h>

void readBattery()
{
    uint32_t sum = 0;
    for(int i=0; i<BAT_SAMPLES; i++){ sum += analogRead(BAT_PIN); delayMicroseconds(200); }
    float raw   = (float)(sum / BAT_SAMPLES);
    float vAdc  = (raw / BAT_ADC_MAX) * BAT_ADC_REF;
    batVoltage  = vAdc / BAT_DIV_RATIO;
    float v     = constrain(batVoltage, BAT_MIN_V, BAT_MAX_V);
    int newPct  = (int)(((v - BAT_MIN_V) / (BAT_MAX_V - BAT_MIN_V)) * 100.0f);
    newPct      = constrain(newPct, 0, 100);
    bool inGrace = (batStartMs==0) || ((millis()-batStartMs) < BAT_GRACE_MS);
    if(batPercent < 0 || inGrace) batPercent = newPct;
    else if(newPct < batPercent)  batPercent = newPct;
}