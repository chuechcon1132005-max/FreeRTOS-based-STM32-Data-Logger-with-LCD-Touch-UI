# Real-Time STM32 Data Logger with Touch LCD and SD Storage

Embedded systems course project built on an STM32F405RGTX board. The system samples
internal chip temperature and an external analog input, displays live values on a
2.8-inch touch LCD, and stores timestamped records on a microSD card.

The goal of this project was not only to connect peripherals, but to build a
complete embedded workflow: real-time acquisition, interactive control, persistent
logging, on-device data viewing, and recoverable SD-card handling.

## Highlights

| Area | Result |
| --- | --- |
| RTOS architecture | Split the firmware into sensor, GUI, and SD-card tasks using CMSIS-RTOS v2. |
| Data acquisition | Used ADC DMA for internal temperature, VREFINT, and external analog input sampling. |
| Data logging | Wrote timestamped records to `data.txt` every 3 seconds while in PLAY mode. |
| On-device viewing | Implemented PAUSE mode with newest-first, paginated data viewing directly on the LCD. |
| Touch robustness | Added shared-SPI handling, dynamic SPI baud-rate switching, median filtering, and stability checks for XPT2046 touch input. |
| SD-card recovery | Added SD mount/status handling, FAT32 auto-format for blank cards, and clear LCD status/error reporting. |

## System Architecture

| Component | Responsibility |
| --- | --- |
| `TaskSensor` | Reads ADC DMA buffers, applies EMA filtering, compensates measurements using VREFINT calibration, and updates shared sensor state. |
| `TaskGUI` | Draws the LCD interface, handles Play/Pause/Data Viewer touch events, and updates live sensor/status fields. |
| `TaskSD` | Mounts the SD card, logs data, reads historical records for the viewer, clears data, and reports SD status/errors. |
| `MutexSPI` | Protects the shared SPI bus used by the ST7789 LCD and XPT2046 touch controller. |

## Hardware

- MCU: STM32F405RGTX, ARM Cortex-M4F
- Board: Waveshare Open405R-C
- Display: 2.8-inch SPI TFT LCD, ST7789-compatible driver path
- Touch: XPT2046 resistive touch controller
- Storage: microSD card over SDIO with FatFs
- Analog input: ADC channel on PC0 for external/test-board input

## Repository Structure

```text
.
|-- Core/                 # Application source, main firmware, RTOS tasks
|-- Drivers/              # STM32 HAL and CMSIS drivers
|-- FATFS/                # FatFs app and target glue code
|-- LCD28/                # LCD/touch driver adaptation
|-- Middlewares/          # FreeRTOS and FatFs middleware
|-- docs/
|   |-- course_report.pdf # Full course report with design and test evidence
|   `-- touch_guide.md    # Notes on XPT2046 touch and shared SPI handling
|-- doan.ioc              # STM32CubeMX configuration
|-- STM32F405RGTX_FLASH.ld
`-- STM32F405RGTX_RAM.ld
```

## Build and Flash

1. Open STM32CubeIDE.
2. Import this directory as an existing STM32CubeIDE project.
3. Confirm that `doan.ioc` targets the STM32F405RGTX board configuration.
4. Build the project.
5. Flash the firmware to the STM32F405RGTX target.
6. Insert a FAT32 microSD card, or allow the firmware to auto-format a blank card.

## Data Format

When logging is enabled, records are appended to `data.txt`:

```text
HH:MM:SS|DD/MM/YYYY|XX.X C|T:XXXX|A:XXXX
```

Where:

- `XX.X C`: compensated internal chip temperature
- `T`: raw ADC value associated with the temperature channel
- `A`: raw ADC value from the external analog input

## Validation Summary

The course report documents the following working functions:

- live LCD display for chip temperature and ADC values
- Play/Pause touch interaction
- periodic SD-card logging every 3 seconds
- paginated LCD viewer with newest records shown first
- clear-data flow from the LCD UI
- SD-card status codes for checking, formatting, ready, OK, and error states

See [docs/course_report.pdf](docs/course_report.pdf) for the full design report and
test screenshots.

## Known Limitations

- The internal STM32 temperature sensor measures chip junction temperature, not ambient temperature.
- RTC time is initialized from build time because no backup battery is used.
- The tested LCD touch panel had a partially unreliable touch area, so the UI uses safe zones and wider hit margins.
- `data.txt` is append-only; file rotation is not implemented.

## Possible Improvements

- Add an external temperature sensor such as DS18B20, LM35, or BME280.
- Stream live data to a PC over USB CDC/UART for plotting.
- Add file rotation by date or maximum file size.
- Add a calibration screen for touch coordinates and sensor offsets.
