#pragma once

#include <windows.h>

#ifdef __cplusplus
namespace ALLUNO_VDD
{
#endif

// ============================================================================
// IOCTL codes (FILE_DEVICE_UNKNOWN base, METHOD_BUFFERED, FILE_ANY_ACCESS)
// ============================================================================

#define IOCTL_ALLUNO_VDD_ADD_DISPLAY        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_REMOVE_DISPLAY     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_SET_RENDER_ADAPTER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_GET_WATCHDOG       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_UPDATE_MODE        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_LIST_DISPLAYS      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_REMOVE_ALL         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_SET_WATCHDOG       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_SET_HDR            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_SET_CUSTOM_EDID    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_PING               CTL_CODE(FILE_DEVICE_UNKNOWN, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ALLUNO_VDD_GET_VERSION        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8FF, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ============================================================================
// Constants
// ============================================================================

#define ALLUNO_VDD_MAX_DISPLAYS     16
#define ALLUNO_VDD_EDID_MAX_SIZE    256
#define ALLUNO_VDD_DEVICE_NAME_LEN  14
#define ALLUNO_VDD_SERIAL_LEN       14

#define ALLUNO_VDD_BPC_8            8
#define ALLUNO_VDD_BPC_10           10
#define ALLUNO_VDD_BPC_12           12

#define ALLUNO_VDD_HDR_OFF          0
#define ALLUNO_VDD_HDR_HDR10        1
#define ALLUNO_VDD_HDR_HDR10_PLUS   2

// Driver version — must match vcxproj TimeStamp
#define ALLUNO_VDD_VERSION_MAJOR    1
#define ALLUNO_VDD_VERSION_MINOR    0
#define ALLUNO_VDD_VERSION_PATCH    1
#define ALLUNO_VDD_VERSION_BUILD    0

// Hardware ID
static const char* ALLUNO_VDD_HARDWARE_ID = "root\\alluno\\vdd";

// DO NOT CHANGE
// {4d36e968-e325-11ce-bfc1-08002be10318}
static const GUID ALLUNO_VDD_CLASS_GUID = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
// {A1142000-7DD0-4A11-4200-A114200007DD}
static const GUID ALLUNO_VDD_INTERFACE_GUID = { 0xA1142000, 0x7DD0, 0x4A11, { 0x42, 0x00, 0xA1, 0x14, 0x20, 0x00, 0x07, 0xDD } };

// ============================================================================
// Structures (all packed)
// ============================================================================

#pragma pack(push, 1)

typedef struct _ALLUNO_VDD_ADD_PARAMS {
	UINT Width;
	UINT Height;
	UINT RefreshRate;
	GUID MonitorGuid;
	CHAR DeviceName[ALLUNO_VDD_DEVICE_NAME_LEN];
	CHAR SerialNumber[ALLUNO_VDD_SERIAL_LEN];
	UINT BitsPerChannel;
	UINT HdrMode;
	UINT VsyncNumerator;
	UINT VsyncDenominator;
} ALLUNO_VDD_ADD_PARAMS;

typedef struct _ALLUNO_VDD_ADD_RESULT {
	LUID AdapterLuid;
	UINT TargetId;
} ALLUNO_VDD_ADD_RESULT;

typedef struct _ALLUNO_VDD_REMOVE_PARAMS {
	GUID MonitorGuid;
} ALLUNO_VDD_REMOVE_PARAMS;

typedef struct _ALLUNO_VDD_UPDATE_MODE_PARAMS {
	GUID MonitorGuid;
	UINT Width;
	UINT Height;
	UINT RefreshRate;
	UINT BitsPerChannel;
	UINT HdrMode;            /* 0xFF = no change */
	UINT VsyncNumerator;
	UINT VsyncDenominator;
} ALLUNO_VDD_UPDATE_MODE_PARAMS;

typedef struct _ALLUNO_VDD_SET_ADAPTER_PARAMS {
	LUID AdapterLuid;
} ALLUNO_VDD_SET_ADAPTER_PARAMS;

typedef struct _ALLUNO_VDD_WATCHDOG_PARAMS {
	UINT TimeoutMs;
	UINT CountdownMs;
} ALLUNO_VDD_WATCHDOG_PARAMS;

typedef struct _ALLUNO_VDD_SET_WATCHDOG_PARAMS {
	UINT TimeoutMs;
} ALLUNO_VDD_SET_WATCHDOG_PARAMS;

typedef struct _ALLUNO_VDD_VERSION {
	UINT Major;
	UINT Minor;
	UINT Patch;
	UINT Build;
} ALLUNO_VDD_VERSION;

typedef struct _ALLUNO_VDD_HDR_METADATA {
	UINT RedPrimaryX;
	UINT RedPrimaryY;
	UINT GreenPrimaryX;
	UINT GreenPrimaryY;
	UINT BluePrimaryX;
	UINT BluePrimaryY;
	UINT WhitePointX;
	UINT WhitePointY;
	UINT MaxLuminance;
	UINT MinLuminanceX10000;
	UINT MaxContentLightLevel;
	UINT MaxFrameAvgLightLevel;
} ALLUNO_VDD_HDR_METADATA;

typedef struct _ALLUNO_VDD_DISPLAY_INFO {
	GUID MonitorGuid;
	UINT Width;
	UINT Height;
	UINT RefreshRate;
	UINT BitsPerChannel;
	UINT HdrMode;
	LUID AdapterLuid;
	UINT TargetId;
	CHAR DeviceName[ALLUNO_VDD_DEVICE_NAME_LEN];
	BOOL Active;
	UINT VsyncNumerator;
	UINT VsyncDenominator;
} ALLUNO_VDD_DISPLAY_INFO;

typedef struct _ALLUNO_VDD_LIST_RESULT {
	UINT Count;
	ALLUNO_VDD_DISPLAY_INFO Displays[ALLUNO_VDD_MAX_DISPLAYS];
} ALLUNO_VDD_LIST_RESULT;

typedef struct _ALLUNO_VDD_SET_HDR_PARAMS {
	GUID MonitorGuid;
	UINT HdrMode;
	UINT BitsPerChannel;
	UINT HasMetadata;
	ALLUNO_VDD_HDR_METADATA Metadata;
} ALLUNO_VDD_SET_HDR_PARAMS;

typedef struct _ALLUNO_VDD_SET_CUSTOM_EDID_PARAMS {
	GUID MonitorGuid;
	UINT EdidSize;
	BYTE EdidData[ALLUNO_VDD_EDID_MAX_SIZE];
} ALLUNO_VDD_SET_CUSTOM_EDID_PARAMS;

#pragma pack(pop)

#ifdef __cplusplus
} // namespace ALLUNO_VDD
#endif
