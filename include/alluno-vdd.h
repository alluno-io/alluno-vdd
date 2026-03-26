/**
 * Alluno Virtual Display Driver - User-Mode API
 *
 * High-level wrapper over IOCTL calls. Include this single header
 * in your application to manage virtual displays.
 *
 * Usage:
 *   HANDLE h = AllunoVddOpenDevice();
 *   ALLUNO_VDD_ADD_RESULT result;
 *   AllunoVddAddDisplay(h, 1920, 1080, 60, "Client Display", 8, 0, &result);
 *   // ... streaming ...
 *   AllunoVddRemoveDisplay(h, &result.MonitorGuid);
 *   AllunoVddCloseDevice(h);
 *
 * MIT License - (c) 2026 Alluno
 */

#pragma once

#include "alluno-vdd-ioctl.h"

#ifdef _WIN32

#include <objbase.h>   // CoCreateGuid
#include <cfgmgr32.h>  // CM_Get_Device_Interface_ListW

#pragma comment(lib, "cfgmgr32.lib")

// ============================================================================
// Device open / close
// ============================================================================

/**
 * Open a handle to the Alluno VDD driver device via the device interface GUID.
 * Returns INVALID_HANDLE_VALUE on failure.
 */
static inline HANDLE AllunoVddOpenDevice(void) {
    ULONG listSize = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(
        &listSize,
        (LPGUID)&ALLUNO_VDD::ALLUNO_VDD_INTERFACE_GUID,
        NULL,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

    if (cr != CR_SUCCESS || listSize <= 1) {
        return INVALID_HANDLE_VALUE;
    }

    WCHAR* interfaceList = (WCHAR*)calloc(listSize, sizeof(WCHAR));
    if (!interfaceList) {
        return INVALID_HANDLE_VALUE;
    }

    cr = CM_Get_Device_Interface_ListW(
        (LPGUID)&ALLUNO_VDD::ALLUNO_VDD_INTERFACE_GUID,
        NULL,
        interfaceList,
        listSize,
        CM_GET_DEVICE_INTERFACE_LIST_PRESENT);

    if (cr != CR_SUCCESS || interfaceList[0] == L'\0') {
        free(interfaceList);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE device = CreateFileW(
        interfaceList,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    free(interfaceList);
    return device;
}

/**
 * Close a previously opened device handle.
 */
static inline void AllunoVddCloseDevice(HANDLE device) {
    if (device && device != INVALID_HANDLE_VALUE) {
        CloseHandle(device);
    }
}

// ============================================================================
// Fractional refresh rate helpers
// ============================================================================

/** Convert integer Hz to vsync numerator/denominator (e.g., 60 -> 60/1) */
static inline void AllunoVddHzToVsync(UINT hz, UINT* num, UINT* den) {
    if (num) *num = hz;
    if (den) *den = 1;
}

/** Convert float Hz to vsync num/den (e.g., 59.94f -> 5994/100, simplified) */
static inline void AllunoVddFloatToVsync(float hz, UINT* num, UINT* den) {
    // Scale to avoid floating point: use 1000 as denominator, then simplify
    UINT n = (UINT)(hz * 1000.0f + 0.5f);
    UINT d = 1000;
    // Simplify common cases
    if (n % 1000 == 0) { n /= 1000; d = 1; }
    else if (n % 100 == 0) { n /= 100; d /= 100; }
    else if (n % 10 == 0) { n /= 10; d /= 10; }
    if (num) *num = n;
    if (den) *den = d;
}

// ============================================================================
// High-level API functions
// ============================================================================

/**
 * Add a virtual display with specified parameters.
 * Generates a new GUID automatically.
 * Returns TRUE on success, fills result with adapter info.
 */
static inline BOOL AllunoVddAddDisplay(
    HANDLE device,
    UINT width,
    UINT height,
    UINT refreshRate,
    const char* name,
    UINT bitsPerChannel,
    UINT hdrMode,
    ALLUNO_VDD::ALLUNO_VDD_ADD_RESULT* result
) {
    if (device == INVALID_HANDLE_VALUE || !result) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_ADD_PARAMS params = {0};
    params.Width = width;
    params.Height = height;
    params.RefreshRate = refreshRate;
    params.BitsPerChannel = bitsPerChannel ? bitsPerChannel : ALLUNO_VDD_BPC_8;
    params.HdrMode = hdrMode;

    CoCreateGuid(&params.MonitorGuid);

    if (name) {
        strncpy_s(params.DeviceName, sizeof(params.DeviceName), name, _TRUNCATE);
    } else {
        strcpy_s(params.DeviceName, sizeof(params.DeviceName), "Alluno Display");
    }
    strcpy_s(params.SerialNumber, sizeof(params.SerialNumber), "ALLUNO-VDD");

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_ADD_DISPLAY,
        &params, sizeof(params),
        result, sizeof(*result),
        &bytesReturned,
        NULL);

    return ok && bytesReturned == sizeof(*result);
}

/**
 * Add a virtual display with explicit GUID and optional fractional refresh rate.
 * Pass vsyncNumerator=0, vsyncDenominator=0 to use integer refreshRate.
 */
static inline BOOL AllunoVddAddDisplayEx(
    HANDLE device,
    UINT width,
    UINT height,
    UINT refreshRate,
    const char* name,
    const char* serial,
    UINT bitsPerChannel,
    UINT hdrMode,
    const GUID* monitorGuid,
    UINT vsyncNumerator,
    UINT vsyncDenominator,
    ALLUNO_VDD::ALLUNO_VDD_ADD_RESULT* result
) {
    if (device == INVALID_HANDLE_VALUE || !result) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_ADD_PARAMS params = {0};
    params.Width = width;
    params.Height = height;
    params.RefreshRate = refreshRate;
    params.BitsPerChannel = bitsPerChannel ? bitsPerChannel : ALLUNO_VDD_BPC_8;
    params.HdrMode = hdrMode;
    params.VsyncNumerator = vsyncNumerator;
    params.VsyncDenominator = vsyncDenominator;

    if (monitorGuid) {
        params.MonitorGuid = *monitorGuid;
    } else {
        CoCreateGuid(&params.MonitorGuid);
    }

    if (name) {
        strncpy_s(params.DeviceName, sizeof(params.DeviceName), name, _TRUNCATE);
    } else {
        strcpy_s(params.DeviceName, sizeof(params.DeviceName), "Alluno Display");
    }

    if (serial) {
        strncpy_s(params.SerialNumber, sizeof(params.SerialNumber), serial, _TRUNCATE);
    } else {
        strcpy_s(params.SerialNumber, sizeof(params.SerialNumber), "ALLUNO-VDD");
    }

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_ADD_DISPLAY,
        &params, sizeof(params),
        result, sizeof(*result),
        &bytesReturned,
        NULL);

    return ok && bytesReturned == sizeof(*result);
}

/**
 * Add a virtual display with explicit GUID (for reconnecting to same display).
 */
static inline BOOL AllunoVddAddDisplayWithGuid(
    HANDLE device,
    UINT width,
    UINT height,
    UINT refreshRate,
    const char* name,
    UINT bitsPerChannel,
    UINT hdrMode,
    const GUID* monitorGuid,
    ALLUNO_VDD::ALLUNO_VDD_ADD_RESULT* result
) {
    return AllunoVddAddDisplayEx(device, width, height, refreshRate, name, NULL,
                                 bitsPerChannel, hdrMode, monitorGuid, 0, 0, result);
}

/**
 * Remove a virtual display by GUID.
 */
static inline BOOL AllunoVddRemoveDisplay(
    HANDLE device,
    const GUID* monitorGuid
) {
    if (device == INVALID_HANDLE_VALUE || !monitorGuid) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_REMOVE_PARAMS params = {0};
    params.MonitorGuid = *monitorGuid;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_REMOVE_DISPLAY,
        &params, sizeof(params),
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Remove all virtual displays (atomic, driver-side).
 */
static inline BOOL AllunoVddRemoveAll(HANDLE device) {
    if (device == INVALID_HANDLE_VALUE) return FALSE;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_REMOVE_ALL,
        NULL, 0,
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Update display mode (resolution/refresh rate/HDR) on existing display.
 * Pass 0 for fields you don't want to change. Pass 0xFF for hdrMode to keep current.
 */
static inline BOOL AllunoVddUpdateMode(
    HANDLE device,
    const GUID* monitorGuid,
    UINT width,
    UINT height,
    UINT refreshRate,
    UINT bitsPerChannel,
    UINT hdrMode
) {
    if (device == INVALID_HANDLE_VALUE || !monitorGuid) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_UPDATE_MODE_PARAMS params = {0};
    params.MonitorGuid = *monitorGuid;
    params.Width = width;
    params.Height = height;
    params.RefreshRate = refreshRate;
    params.BitsPerChannel = bitsPerChannel;
    params.HdrMode = hdrMode;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_UPDATE_MODE,
        &params, sizeof(params),
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Update display mode with fractional refresh rate.
 */
static inline BOOL AllunoVddUpdateModeEx(
    HANDLE device,
    const GUID* monitorGuid,
    UINT width,
    UINT height,
    UINT refreshRate,
    UINT bitsPerChannel,
    UINT hdrMode,
    UINT vsyncNumerator,
    UINT vsyncDenominator
) {
    if (device == INVALID_HANDLE_VALUE || !monitorGuid) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_UPDATE_MODE_PARAMS params = {0};
    params.MonitorGuid = *monitorGuid;
    params.Width = width;
    params.Height = height;
    params.RefreshRate = refreshRate;
    params.BitsPerChannel = bitsPerChannel;
    params.HdrMode = hdrMode;
    params.VsyncNumerator = vsyncNumerator;
    params.VsyncDenominator = vsyncDenominator;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_UPDATE_MODE,
        &params, sizeof(params),
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * List all active virtual displays.
 */
static inline BOOL AllunoVddListDisplays(
    HANDLE device,
    ALLUNO_VDD::ALLUNO_VDD_LIST_RESULT* list
) {
    if (device == INVALID_HANDLE_VALUE || !list) return FALSE;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_LIST_DISPLAYS,
        NULL, 0,
        list, sizeof(*list),
        &bytesReturned,
        NULL);
}

/**
 * Set which GPU renders to virtual displays.
 */
static inline BOOL AllunoVddSetRenderAdapter(
    HANDLE device,
    LUID adapterLuid
) {
    if (device == INVALID_HANDLE_VALUE) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_SET_ADAPTER_PARAMS params = {0};
    params.AdapterLuid = adapterLuid;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_SET_RENDER_ADAPTER,
        &params, sizeof(params),
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Get watchdog timeout parameters.
 */
static inline BOOL AllunoVddGetWatchdog(
    HANDLE device,
    ALLUNO_VDD::ALLUNO_VDD_WATCHDOG_PARAMS* watchdog
) {
    if (device == INVALID_HANDLE_VALUE || !watchdog) return FALSE;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_GET_WATCHDOG,
        NULL, 0,
        watchdog, sizeof(*watchdog),
        &bytesReturned,
        NULL);
}

/**
 * Set watchdog timeout in milliseconds. 0 = disable.
 */
static inline BOOL AllunoVddSetWatchdog(
    HANDLE device,
    UINT timeoutMs
) {
    if (device == INVALID_HANDLE_VALUE) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_SET_WATCHDOG_PARAMS params = {0};
    params.TimeoutMs = timeoutMs;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_SET_WATCHDOG,
        &params, sizeof(params),
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Ping driver (reset watchdog countdown).
 */
static inline BOOL AllunoVddPing(HANDLE device) {
    if (device == INVALID_HANDLE_VALUE) return FALSE;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_PING,
        NULL, 0,
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Get protocol version for compatibility checking.
 */
static inline BOOL AllunoVddGetVersion(
    HANDLE device,
    ALLUNO_VDD::ALLUNO_VDD_VERSION* version
) {
    if (device == INVALID_HANDLE_VALUE || !version) return FALSE;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_GET_VERSION,
        NULL, 0,
        version, sizeof(*version),
        &bytesReturned,
        NULL);
}

/**
 * Set HDR mode on an existing virtual display.
 */
static inline BOOL AllunoVddSetHdr(
    HANDLE device,
    const GUID* monitorGuid,
    UINT hdrMode,
    UINT bitsPerChannel
) {
    if (device == INVALID_HANDLE_VALUE || !monitorGuid) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_SET_HDR_PARAMS params = {0};
    params.MonitorGuid = *monitorGuid;
    params.HdrMode = hdrMode;
    params.BitsPerChannel = bitsPerChannel;

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_SET_HDR,
        &params, sizeof(params),
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Set HDR mode with ST.2086 mastering display metadata.
 */
static inline BOOL AllunoVddSetHdrWithMetadata(
    HANDLE device,
    const GUID* monitorGuid,
    UINT hdrMode,
    UINT bitsPerChannel,
    const ALLUNO_VDD::ALLUNO_VDD_HDR_METADATA* metadata
) {
    if (device == INVALID_HANDLE_VALUE || !monitorGuid) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_SET_HDR_PARAMS params = {0};
    params.MonitorGuid = *monitorGuid;
    params.HdrMode = hdrMode;
    params.BitsPerChannel = bitsPerChannel;

    if (metadata) {
        params.HasMetadata = 1;
        params.Metadata = *metadata;
    }

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_SET_HDR,
        &params, sizeof(params),
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Set custom EDID for a virtual display (overrides auto-generated EDID).
 * Takes effect on next monitor reconnect (UpdateMode or remove + add with same GUID).
 * edidSize must be 128 or 256.
 */
static inline BOOL AllunoVddSetCustomEdid(
    HANDLE device,
    const GUID* monitorGuid,
    const BYTE* edidData,
    UINT edidSize
) {
    if (device == INVALID_HANDLE_VALUE || !monitorGuid || !edidData) return FALSE;
    if (edidSize != 128 && edidSize != 256) return FALSE;

    ALLUNO_VDD::ALLUNO_VDD_SET_CUSTOM_EDID_PARAMS params = {0};
    params.MonitorGuid = *monitorGuid;
    params.EdidSize = edidSize;
    memcpy(params.EdidData, edidData, edidSize);

    DWORD bytesReturned = 0;
    return DeviceIoControl(
        device,
        IOCTL_ALLUNO_VDD_SET_CUSTOM_EDID,
        &params, sizeof(params),
        NULL, 0,
        &bytesReturned,
        NULL);
}

/**
 * Check if driver is compatible with this header version.
 * Returns TRUE if driver major version matches and minor >= header minor.
 */
static inline BOOL AllunoVddIsCompatible(HANDLE device) {
    ALLUNO_VDD::ALLUNO_VDD_VERSION version = {0};
    if (!AllunoVddGetVersion(device, &version)) return FALSE;
    return version.Major == ALLUNO_VDD_PROTOCOL_MAJOR
        && version.Minor >= ALLUNO_VDD_PROTOCOL_MINOR;
}

#endif /* _WIN32 */
