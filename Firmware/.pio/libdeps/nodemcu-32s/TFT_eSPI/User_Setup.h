// ----------------------------------------------------
// TFT DRIVER
// ----------------------------------------------------
#define ILI9488_DRIVER
#define TFT_RGB_ORDER TFT_BGR

// ----------------------------------------------------
// SHARED SPI (VSPI) — SAME BUS FOR TFT + TOUCH
// ----------------------------------------------------
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18

// ----------------------------------------------------
// DISPLAY PINS
// ----------------------------------------------------
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST    4

// ----------------------------------------------------
// TOUCH PINS (SAME SPI BUS!)
// ----------------------------------------------------
#define TOUCH_CS   5

// ----------------------------------------------------
// SPI SPEEDS - OPTIMIZED FOR 40MHz!
// ----------------------------------------------------
#define SPI_FREQUENCY         40000000  // ✅ 40MHz - your display supports it!
#define SPI_READ_FREQUENCY    16000000  // ✅ Keep lower for stability
#define SPI_TOUCH_FREQUENCY    2500000

// ----------------------------------------------------
// FONTS
// ----------------------------------------------------
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

// ----------------------------------------------------
// PERFORMANCE OPTIMIZATIONS
// ----------------------------------------------------
#define SMOOTH_FONT
#define SPI_18BIT_DRIVER  // Use 18-bit mode for ILI9488 (faster!)

// ----------------------------------------------------
// ESP32 DMA SUPPORT - CRITICAL FOR PERFORMANCE!
// ----------------------------------------------------
#define TFT_DMA  // Enable DMA transfers for sprites