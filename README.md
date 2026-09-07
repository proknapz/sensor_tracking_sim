# Real-Time Sensor & Target Tracking Simulator

A high-performance, embedded-style telemetry and target tracking simulator written in C++17. The system features custom bit-packed telemetry frames, deterministic state machine management, live telemetry ingestion, and a native Win32 hardware dashboard.

---

## Technical Highlights

- **ZERO DYNAMIC ALLOCATION:** Fixed-size circular telemetry queues and stack-allocated arrays
- **REAL-TIME TELEMETRY DECODE:** 3-word bit-packed frames with XOR checksum integrity validation
- **DETERMINISTIC STATE LOGIC:** IDLE -> ACQUIRING -> TRACKING -> FAULT with built-in watchdog recovery
- **LIVE API INGESTION:** Direct HTTP streaming via native Win32 WinHTTP (USGS and ISS feeds)
- **NATIVE WIN32 DASHBOARD:** Live visual analytics with custom GDI graph rendering

---

## System Architecture
```text
+-------------------------------------------------------------+
|                    TELEMETRY INGESTION                      |
|           (Synthetic Feed / USGS API / ISS API)             |
+------------------------------+------------------------------+
                               |
                               v
+-------------------------------------------------------------+
|                    BITFIELD FRAME PACKER                    |
|             (Packs telemetry into 3 x uint16_t)             |
+------------------------------+------------------------------+
                               |
                               v
+-------------------------------------------------------------+
|                   FIXED CIRCULAR QUEUE                      |
|             (Zero dynamic allocation buffer)                |
+------------------------------+------------------------------+
                               |
                               v
+-------------------------------------------------------------+
|                    STATE MACHINE ENGINE                     |
|         (Decodes frames, verifies XOR, updates state)       |
+--------------+-------------------------------+--------------+
               |                               |
               v                               v
+-----------------------------+ +-----------------------------+
|       CONSOLE STREAM        | |    WIN32 GUI DASHBOARD      |
|  (CLI logging & diagnostic) | |  (Live visual GDI rendering)|
+-----------------------------+ +-----------------------------+
```

---

## Telemetry Frame Protocol

Telemetry frames are packed into three 16-bit unsigned integer words (`uint16_t`):

Word 0 | [15] Valid Bit | [14..12] Status | [11..7] Target ID | [6..0] Sequence Counter
Word 1 | [15..10] Pitch Angle             | [9..4] Yaw Angle  | [3..0] Quality Metric
Word 2 | [15..6] Target Distance (Meters)                     | [5..0] XOR Checksum


| Word | Bit Range | Type / Format | Field Description |
| :--- | :--- | :--- | :--- |
| **0** | Bit 15 | Flag | Frame Validity Marker (`0x8000`) |
| | Bits 14–12 | Enum | Operational State Identifier |
| | Bits 11–7 | `uint5_t` | Target Identifier (0–31) |
| | Bits 6–0 | `uint7_t` | Frame Sequence Counter |
| **1** | Bits 15–10 | Offset Binary | Pitch Angle (Offset = +32°) |
| | Bits 9–4 | Offset Binary | Yaw Angle (Offset = +32°) |
| | Bits 3–0 | `uint4_t` | Signal Quality Metric (0–15) |
| **2** | Bits 15–6 | `uint10_t` | Distance / Range (Meters) |
| | Bits 5–0 | Mask | XOR Frame Verification Checksum |

---

## Build & Quickstart

### Environment Requirements
- **Compiler:** C++17 compliant (`MSVC`, `GCC`, `Clang`)
- **Platform:** Windows SDK (required for GUI and WinHTTP API features)

### Compile via MSVC Command Line
```bash
cl /O2 /EHsc /std:c++17 sensor_tracking_sim.cpp winhttp.lib user32.lib gdi32.lib
Execution Modes
Bash
# Launch interactive Win32 GUI Dashboard
./sensor_tracking_sim --gui

# Stream live ISS (International Space Station) orbital telemetry
./sensor_tracking_sim --iss

# Stream live USGS earthquake seismic telemetry
./sensor_tracking_sim --usgs

# Run deterministic benchmark simulation for 100 ticks
./sensor_tracking_sim --ticks 100