# [DETAILED GUIDE] Configuration and Programming of XPT2046 Touch on STM32

This document focuses in-depth on implementing resistive touch functionality using the **XPT2046** IC. The content includes an analysis of the differences compared to the original Waveshare driver (Open405R) and practical signal optimization techniques.

---

## 1. Analysis of Differences from the Original Example (Open405R)

Based on the analysis of the `LCD28` library from Waveshare, the current project has made significant adjustments to optimize and be compatible with modern environments:

| Feature | Original Open405R Example | Current Project (Adjusted) |
| :--- | :--- | :--- |
| **Library** | Standard Peripheral Library (SPL) | **STM32 HAL Library** (Better support for CubeMX/IDE) |
| **Bus Management** | Dedicated SPI for LCD | **Shared SPI1 bus** between LCD and Touch via independent CS management. |
| **SPI Speed** | Fixed for LCD | **Dynamic Baudrate Switching**: Automatically lowers SPI speed to `Prescaler 64` when reading Touch to ensure XPT2046 ADC accuracy. |
| **Noise Processing** | Basic or none | **Median Filter + Stability Span Check**. |
| **Integration** | Discrete driver | Deeply integrated into a **FreeRTOS Task**, handling real-time touch events. |

---

## 2. Advanced Hardware Configuration

### Signal Pin Connections (Pinout)
Follows the Open405R schematic exactly but managed via HAL:
- **TP_CS (PB9):** Touch chip select pin (Active Low).
- **TP_IRQ (PB4):** Interrupt pin indicating a touch event (Active Low). Requires `Input Pull-up` configuration.
- **SPI1 (Shared):** SCK (PA5), MISO (PA6), MOSI (PA7).

### SPI Speed Management
This is the most critical part to ensure touch coordinates don't "jump" or become inaccurate. The XPT2046 touch IC cannot operate at high speeds like the ST7789 display.
- **LCD SPI Speed:** Typically runs at Prescaler 2 or 4 (high speed).
- **Touch SPI Speed:** Must be lowered to Prescaler 64 or higher.

---

## 3. Low-level Data Reading Process

To obtain accurate coordinates, the SPI communication process occurs as follows:

1. **Lower SPI Speed:** Save the current configuration and switch to a lower speed.
2. **Activate TCS:** Pull `TP_CS` pin LOW.
3. **Send Command (Control Byte):**
   - Read X: `0xD0` (12-bit mode, Differential, Power down between conversions).
   - Read Y: `0x90`.
4. **Receive Result:** Receive 2 bytes from SPI, combine them into a 12-bit value (0 - 4095).
5. **Deactivate TCS:** Pull `TP_CS` pin HIGH.
6. **Restore SPI Speed:** Restore the high speed for the LCD.

---

## 4. Signal Processing Algorithm

To ensure smooth touch and avoid "ghost touches", the project applies two layers of protection:

### Layer 1: Median Filter (3-point)
Read 3 consecutive times for each axis, take the middle value to eliminate sudden noise spikes.
```c
static uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
    // Sort and return the median value
}
```

### Layer 2: Stability Span Check
Only accept coordinates if the distance between the maximum and minimum values in 3 readings does not exceed a threshold (`TOUCH_STABLE_SPAN_MAX`, e.g., 180 ADC units). If exceeded, the data is considered noisy and the touch is ignored.

---

## 5. Calibration and Screen Coordinate System

The 12-bit ADC value needs to be mapped to screen pixels (240x320).

### Mapping Configuration
Constants need to be determined practically (Calibration):
- `TOUCH_RAW_X_MIN/MAX`: X-axis ADC limits.
- `TOUCH_RAW_Y_MIN/MAX`: Y-axis ADC limits.

### Orientation Handling
- **Swap XY:** If the screen is rotated horizontally/vertically.
- **Invert X/Y:** If the touch direction is opposite to the LCD drawing direction.

---

## 6. Physical Defect Handling (Dead Zones)

A common reality is that the resistive touch panel may be damaged or unresponsive in certain areas (e.g., bottom right corner).
- **Software Solution:** Instead of trying to fix the hardware flaw, design the UI to avoid that area.
- **Hit Detection:** Use area check functions (`ui_in_rect`) and design buttons with hit margins wider than the visual image to enhance user experience.
- **Safe Zone:** Move critical navigation buttons to the "safe zone" (usually the left half of the screen if the right half is damaged).

---
*Document based on the actual project adapted from Open405R.*
