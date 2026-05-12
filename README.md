# STM32 RTOS Data Logger & Interactive GUI System

## Overview
This project is an advanced embedded system developed on the **STM32F405RGTX** microcontroller. It serves as a robust Data Logger and Interactive Touch Display, integrating a Real-Time Operating System (**FreeRTOS**), an interactive Graphical User Interface (TFT LCD + **XPT2046** Touch), precision sensor acquisition via **ADC with DMA**, and persistent data storage using a MicroSD card via the **SDIO** interface and **FatFs**.

Designed with industry-standard practices, this project demonstrates the ability to manage complex multi-threaded environments, optimize peripheral sharing, and handle physical hardware edge cases efficiently.

## Key Technical Features

### 1. Real-Time Multitasking (FreeRTOS)
- **Task Architecture:** System is divided into three primary tasks (`TaskGUI`, `TaskSensor`, and `TaskSD`), ensuring non-blocking operations and predictable execution times.
- **Efficient Synchronization:** Utilizes lightweight Task Notifications (`osThreadFlags`) for zero-copy, fast inter-task communication (e.g., signaling the SD task to start/stop logging or read data) instead of heavy queues.

### 2. Advanced Touch & Display Integration (SPI Bus Sharing)
- **Dynamic SPI Baudrate Switching:** Safely shares a single SPI bus (`SPI1`) between the high-speed TFT LCD (ST7789) and the low-speed XPT2046 touch controller by actively switching the SPI prescaler during context execution.
- **Signal Processing Pipeline:** Implements a 3-point median filter and stable-span boundary checks on the raw ADC touch data to eliminate noise spikes and "ghost touches".
- **Resilient UI Design:** Hardware-aware GUI design that incorporates "safe zones" and expanded hit-margins to effectively bypass physically dead or unresponsive touch areas on the screen panel.

### 3. Precision Data Acquisition (ADC & DMA)
- **Hardware-Level Automation:** Uses DMA for background ADC sampling, reducing CPU overhead.
- **Factory Calibration & Compensation:** Extracts ST factory calibration data (at 30°C and 110°C) from ROM and applies `VREFINT` compensation to dynamically calculate highly accurate absolute junction temperatures, eliminating chip-to-chip offset variations.

### 4. Robust SD Card Logging (SDIO & FatFs)
- **State Machine Driven:** Maintains SD card status (Checking, Ready, Mounted, Error) continuously.
- **Auto-Recovery:** Includes an automatic format mechanism (`f_mkfs`) if an SD card is inserted without a valid FAT filesystem.
- **Dual Operating Modes:** 
  - **PLAY Mode:** Logs real-time sensor data (Chip Temperature & Analog Test Board ADC) to `data.txt` every 3 seconds.
  - **PAUSE Mode (Data Viewer):** Pauses data collection and acts as an onboard text viewer, allowing the user to navigate and read historical data pages directly on the LCD screen.

## Hardware Stack
- **Microcontroller:** STM32F405RGTX (ARM Cortex-M4F)
- **Display:** 2.8" SPI TFT LCD (Driver: ST7789)
- **Touch Controller:** XPT2046 (Resistive Touch)
- **Storage:** MicroSD Card connected via SDIO 4-bit interface

## Software & Toolchain
- **Firmware Library:** STM32 HAL Library
- **OS:** FreeRTOS (via CMSIS-OS wrapper)
- **Filesystem:** ChaN's FatFs
- **IDE / Toolchain:** STM32CubeIDE / GCC ARM

## Project Structure
- `Core/Src/main.c`: Contains the FreeRTOS tasks, RTOS objects initialization, ADC calibration logic, and the UI rendering state machine.
- `huong_dan_cam_ung.md`: Detailed technical documentation outlining the XPT2046 custom driver implementation and SPI bus management strategies.
- `LCD28/` & `FATFS/`: Peripheral and file system drivers.

---
*This repository highlights practical experience in Bare-metal to RTOS migration, Low-level Driver Optimization, and robust Embedded C programming.*
