# ESP32-S3 Wireless DAPLink

An ESP32-S3-based wired and wireless CMSIS-DAP debugger built with **ESP-IDF v5.5** and TinyUSB. It enumerates as a multi-interface USB composite device for local debugging and configuration, while offering Wi-Fi wireless debugging via the ELP (elaphureLink) protocol and an embedded web management interface.

## Features

- **USB Composite Device (Plug & Play)**:
  - **CMSIS-DAP v2 (WinUSB)**: Driverless high-speed SWD debugging on Windows 10/11, Linux, and macOS (compatible with Keil MDK, OpenOCD, and pyOCD).
  - **USB CDC Serial Port**: Transparent USB-to-UART bridge for target serial communication.
  - **USB Mass Storage (MSC)**: Mounts as a USB flash drive containing `wifi.txt` for easy Wi-Fi SSID and password configuration without recompiling firmware.
- **Wireless Debugging (Wi-Fi + mDNS)**:
  - Automatically resolves via mDNS at `dap.local`.
  - Works seamlessly with the included `pyocd_elp.py` bridge script for wireless flashing, register inspection, and interactive debugging over TCP.
- **Embedded Web Console (`http://dap.local/`)**:
  - **Web Serial Terminal**: View and interact with live target UART passthrough logs directly in your browser.
  - **Log Management**: Download recorded UART passthrough log files stored on the device.
  - **Web OTA Updates**: Upload `.bin` firmware images to upgrade the ESP32-S3 debugger over Wi-Fi.

## Hardware Pinout

Connect your ESP32-S3 board to the target MCU using the following GPIO assignments:

| Function | Signal | ESP32-S3 Pin | Description |
| :--- | :--- | :--- | :--- |
| **SWD** | `SWCLK` | `GPIO 1` | Serial Wire Clock |
| **SWD** | `SWDIO` | `GPIO 2` | Serial Wire Data Input/Output |
| **SWD** | `nRESET` | `GPIO 3` | Target Hardware Reset (Active Low) |
| **UART** | `TXD` | `GPIO 17` | UART Transmit (connect to Target RX) |
| **UART** | `RXD` | `GPIO 18` | UART Receive (connect to Target TX) |

> **Note**: Ensure both the ESP32-S3 and the target board share a common `GND` and operate at compatible 3.3V logic levels.

## Quick Start

### 1. Build & Flash Firmware
Ensure you have **ESP-IDF v5.5** installed and activated in your environment:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### 2. USB Mode & Wi-Fi Configuration
1. Plug the ESP32-S3 native USB port into your PC.
2. Three interfaces will appear automatically:
   - A **CMSIS-DAP** debug probe ready for wired debugging.
   - A **USB Serial (CDC)** COM port for UART passthrough.
   - A **USB Mass Storage** drive.
3. Open the mounted USB drive, edit `wifi.txt` with your Wi-Fi **SSID** and **Password**, save the file, and reset the ESP32-S3.

### 3. Web Console (`http://dap.local/`)
Once connected to your Wi-Fi network, open a browser and navigate to:
```text
[http://dap.local/](http://dap.local/)
```
From the web interface, you can:
- Monitor live UART serial output in the web terminal.
- Download saved UART log files.
- Perform wireless OTA firmware updates.

### 4. Wireless Debugging with pyOCD
This repository includes `pyocd_elp.py` to bridge wireless ELP probes with `pyocd`.

1. Install `pyocd` on your host machine:
   ```bash
   pip install pyocd
   ```
2. Launch an interactive pyOCD commander session over Wi-Fi (example using a Nations Tech `N32G452CCL7` target):
   ```bash
   python ./pyocd_elp.py commander -u ELP-dap.local --pack ./Nations.N32G45x_DFP.1.3.0.pack -t n32g452ccl7 -f 2000000
   ```
   *(Replace the `--pack` path, `-t` target name, and `-f` SWD clock frequency to match your target MCU.)*

## License

This project is open-source. See the `LICENSE` file for details.