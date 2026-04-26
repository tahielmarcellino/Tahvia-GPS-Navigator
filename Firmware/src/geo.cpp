/**
 * geo.cpp
 * Geographic helpers implementation.
 */

#include "geo.h"
#include <math.h>

// -----------------------------------------------------------------------------
// Distance
// -----------------------------------------------------------------------------
float haversineM(float la1, float lo1, float la2, float lo2)
{
    const float R = 6371000.0f;
    float dLa = (la2-la1)*DEG_TO_RAD, dLo = (lo2-lo1)*DEG_TO_RAD;
    float a = sinf(dLa/2)*sinf(dLa/2)
            + cosf(la1*DEG_TO_RAD)*cosf(la2*DEG_TO_RAD)*sinf(dLo/2)*sinf(dLo/2);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f-a));
}

float haversineKm(float la1, float lo1, float la2, float lo2)
{
    return haversineM(la1, lo1, la2, lo2) / 1000.0f;
}

// -----------------------------------------------------------------------------
// Bearing / angle
// -----------------------------------------------------------------------------
float bearingDeg(const WP &a, const WP &b)
{
    float dLo = (b.lon-a.lon)*DEG_TO_RAD;
    float laA  = a.lat*DEG_TO_RAD, laB = b.lat*DEG_TO_RAD;
    float y    = sinf(dLo)*cosf(laB);
    float x    = cosf(laA)*sinf(laB) - sinf(laA)*cosf(laB)*cosf(dLo);
    return fmodf(atan2f(y,x)*RAD_TO_DEG + 360.0f, 360.0f);
}

float angleDiff(float a, float b)
{
    float d = fmodf(fabsf(a-b), 360.0f);
    return d > 180.0f ? 360.0f-d : d;
}

// -----------------------------------------------------------------------------
// Coordinate → pixel
// -----------------------------------------------------------------------------
void l2p(float lat, float lon, int &px, int &py)
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

// -----------------------------------------------------------------------------
// Viewport builders
// -----------------------------------------------------------------------------
void buildVP()
{
    if(route.size() < 2) return;
    float mnLa=1e9f, mxLa=-1e9f, mnLo=1e9f, mxLo=-1e9f;
    for(auto &w : route){
        mnLa=fminf(mnLa,w.lat); mxLa=fmaxf(mxLa,w.lat);
        mnLo=fminf(mnLo,w.lon); mxLo=fmaxf(mxLo,w.lon);
    }
    float cLa = (mnLa+mxLa)*0.5f, cLo = (mnLo+mxLo)*0.5f;
    float cosC = fmaxf(cosf(cLa*DEG_TO_RAD), 0.01f);
    float laM  = (mxLa-mnLa)*111320.0f;
    float loM  = (mxLo-mnLo)*111320.0f*cosC;
    float sideM = fmaxf(fmaxf(laM,loM), 50.0f)*1.5f;
    float halfLa = sideM*0.5f/111320.0f;
    float halfLo = sideM*0.5f/(111320.0f*cosC);
    vp.minLat=cLa-halfLa; vp.maxLat=cLa+halfLa;
    vp.minLon=cLo-halfLo; vp.maxLon=cLo+halfLo;
    vp.ready=true;
}

void buildZoomedVP(float centerLat, float centerLon)
{
    float cosC   = fmaxf(cosf(centerLat*DEG_TO_RAD), 0.01f);
    float halfLa = zoomRadiusM/111320.0f;
    float halfLo = zoomRadiusM/(111320.0f*cosC);
    vp.minLat=centerLat-halfLa; vp.maxLat=centerLat+halfLa;
    vp.minLon=centerLon-halfLo; vp.maxLon=centerLon+halfLo;
    vp.ready=true;
}

// -----------------------------------------------------------------------------
// Turn detection
// -----------------------------------------------------------------------------
void buildTurns()
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