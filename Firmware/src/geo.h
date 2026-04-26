#pragma once
/**
 * geo.h
 * Geographic helpers: distance calculations, viewport builders, turn detection,
 * and coordinate-to-pixel projection.
 */

#include <Arduino.h>
#include "state.h"
#include "config.h"

// ── Distance
float haversineM (float la1, float lo1, float la2, float lo2);
float haversineKm(float la1, float lo1, float la2, float lo2);

// ── Bearing / angle
float bearingDeg(const WP &a, const WP &b);
float angleDiff (float a, float b);

// ── Coordinate → pixel projection
void l2p(float lat, float lon, int &px, int &py);

// ── Viewport builders
void buildVP();
void buildZoomedVP(float centerLat, float centerLon);

// ── Turn-index detection
void buildTurns();