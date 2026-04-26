#pragma once
/**
 * battery.h
 * Battery voltage sampling and percentage calculation.
 */

// Sample ADC, update batVoltage and batPercent in state.
void readBattery();