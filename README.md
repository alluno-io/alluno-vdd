# Alluno Virtual Display Driver (AllunoVDD)

Windows IddCx virtual display driver for remote desktop streaming. Creates virtual monitors on demand via a clean IOCTL API.

## Requirements

- Windows 10 version 2004+ (build 19041) or Windows 11
- 64-bit (x64)

## Features

- Up to 16 virtual displays with dynamic add/remove
- Per-monitor EDID generation (ALN manufacturer)
- Unlimited custom resolutions and refresh rates (GPU-bound)
- Fractional refresh rates (59.94Hz, 23.976Hz, etc.)
- HDR10 / HDR10+ support (10/12-bit color) with ST.2086 metadata
- IddCx 1.10 HDR callbacks + FP16 swapchain
- Runtime mode changes without reconnect
- Custom EDID per monitor
- GPU adapter selection (registry or IOCTL)
- Configurable watchdog with ping keepalive
- Protocol versioning (v2.0.0)
- User-mode C API wrapper (`alluno-vdd.h`, 21 functions)

## API

Include `alluno-vdd.h` in your application:

```c
#include "alluno-vdd.h"

HANDLE device = AllunoVddOpenDevice();

// Check driver version
ALLUNO_VDD::ALLUNO_VDD_VERSION ver;
AllunoVddGetVersion(device, &ver);

// Create a 1080p 60Hz display
ALLUNO_VDD::ALLUNO_VDD_ADD_RESULT result;
AllunoVddAddDisplay(device, 1920, 1080, 60, "Remote Display", 8, 0, &result);

// Create with fractional refresh rate (59.94Hz)
AllunoVddAddDisplayEx(device, 1920, 1080, 60, "Display", "SN001",
                      8, 0, NULL, 60000, 1001, &result);

// Update mode at runtime
AllunoVddUpdateMode(device, &guid, 2560, 1440, 144, 0, 0xFF);

// Toggle HDR
AllunoVddSetHdr(device, &guid, ALLUNO_VDD_HDR_HDR10, ALLUNO_VDD_BPC_10);

// HDR with ST.2086 metadata
ALLUNO_VDD::ALLUNO_VDD_HDR_METADATA meta = { /* primaries, luminance */ };
AllunoVddSetHdrWithMetadata(device, &guid, 1, 10, &meta);

// List active displays
ALLUNO_VDD::ALLUNO_VDD_LIST_RESULT list;
AllunoVddListDisplays(device, &list);

// Set custom EDID
AllunoVddSetCustomEdid(device, &guid, edidBytes, 256);

// Disable watchdog (display persists after process exits)
AllunoVddSetWatchdog(device, 0);

// Remove display
AllunoVddRemoveDisplay(device, &guid);

// Remove all
AllunoVddRemoveAll(device);

AllunoVddCloseDevice(device);
```

## IOCTL Reference

| Code | Function | Description |
|------|----------|-------------|
| `0x800` | AddDisplay | Create virtual display |
| `0x801` | RemoveDisplay | Remove by GUID |
| `0x802` | SetRenderAdapter | Bind to specific GPU |
| `0x803` | GetWatchdog | Get watchdog state |
| `0x804` | UpdateMode | Change resolution/refresh/HDR |
| `0x805` | ListDisplays | List all active displays |
| `0x806` | RemoveAll | Remove all displays (atomic) |
| `0x807` | SetWatchdog | Set watchdog timeout (0=disable) |
| `0x808` | SetHdr | Toggle HDR mode + metadata |
| `0x809` | SetCustomEdid | Set custom EDID per monitor |
| `0x888` | Ping | Reset watchdog countdown |
| `0x8FF` | GetVersion | Protocol version |

## License

MIT
