#pragma once
/**
 * config.h
 * Compile-time constants: display geometry, BLE UUIDs, pin assignments,
 * colour palette, and tuning parameters.
 */

// -----------------------------------------------------------------------------
// Firmware version
// -----------------------------------------------------------------------------
#define FW_VERSION  "V1.1.9C"

// -----------------------------------------------------------------------------
// Display geometry
// -----------------------------------------------------------------------------
#define SCREEN_W    480
#define SCREEN_H    320
#define MAP_W       320
#define MAP_H       320
#define INFO_X      320
#define INFO_W      160

// -----------------------------------------------------------------------------
// Palette helper
// -----------------------------------------------------------------------------
#define RGB(r,g,b) ((uint16_t)(((uint16_t)((r)>>3)<<11)|((uint16_t)((g)>>2)<<5)|((uint16_t)((b)>>3))))

// ── Map colors
#define C_MAP_BG      RGB(242,239,233)
#define C_ROAD_FILL   TFT_WHITE
#define C_ROAD_SHAD   RGB(220,216,210)
#define C_ROUTE_SHAD  RGB(  8, 98,185)
#define C_ROUTE_FILL  RGB( 26,133,255)
#define C_DONE_SHAD   RGB( 80,110,200)
#define C_DONE_FILL   RGB(130,160,235)
#define C_START       RGB( 26,133,255)
#define C_END         RGB(234, 67, 53)
#define C_YOU_RING    TFT_WHITE
#define C_YOU_FILL    RGB( 26,133,255)
#define C_YOU_SNAP    RGB( 26,133,255)
#define C_YOU_PULSE   RGB(161,204,255)
#define C_TURN_DOT    TFT_WHITE
#define C_NOROUTE     RGB( 26,133,255)

// ── Panel colors
#define C_PANEL_BG    TFT_WHITE
#define C_PANEL_LINE  RGB(232,234,237)
#define C_HEADER_BG   TFT_WHITE
#define C_HEADER_LINE RGB(232,234,237)

// ── Text colors
#define C_TEXT_PRIMARY   TFT_BLACK
#define C_TEXT_LABEL     RGB( 32, 33, 36)
#define C_TEXT_MUTED     RGB(128, 68,255)
#define C_TEXT_UNIT      RGB( 26,133,255)

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

// ── Progress bar track
#define C_BAR_TRACK   RGB(232,234,237)

// ── Boot screen
#define C_BOOT_BG     RGB(248,249,250)

// -----------------------------------------------------------------------------
// BLE
// -----------------------------------------------------------------------------
#define DEVICE_NAME         "ESP32_Speedometer"
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// -----------------------------------------------------------------------------
// Ring buffer
// -----------------------------------------------------------------------------
#define BLE_CHUNK_DATA  512
#define RING_SLOTS      8

// -----------------------------------------------------------------------------
// OTA
// -----------------------------------------------------------------------------
#define OTA_BUF_SIZE 192

// -----------------------------------------------------------------------------
// GPS / navigation
// -----------------------------------------------------------------------------
#define GPS_TIMEOUT_MS  2000
#define SNAP_M          15.0f
#define TURN_DEG        45.0f

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

// -----------------------------------------------------------------------------
// Info panel layout
// -----------------------------------------------------------------------------
#define PANEL_ROW_H  48
#define PANEL_ROW_Y0 29