//! Rust bindings for Alluno Virtual Display Driver (AllunoVDD)
//!
//! # Example
//! ```no_run
//! use alluno_vdd_sys::*;
//!
//! let device = AllunoVdd::new().expect("driver not installed");
//! let version = device.get_version().unwrap();
//! println!("Driver v{}.{}.{}", version.major, version.minor, version.patch);
//!
//! device.set_watchdog(0).unwrap(); // disable watchdog
//! let result = device.add_display(1920, 1080, 60, "Test", 8, 0).unwrap();
//! println!("Added display, target_id={}", result.target_id);
//!
//! let list = device.list_displays().unwrap();
//! for d in &list {
//!     println!("{}x{} @{}Hz", d.width, d.height, d.refresh_rate);
//! }
//!
//! device.remove_all().unwrap();
//! ```

use std::ffi::c_void;
use std::mem::{size_of, zeroed};
use windows::core::{Error, Result, GUID, HRESULT, PCWSTR};
use windows::Win32::Devices::DeviceAndDriverInstallation::*;
use windows::Win32::Devices::Display::*;
use windows::Win32::Foundation::*;
use windows::Win32::Graphics::Gdi::*;
use windows::Win32::Storage::FileSystem::*;
use windows::Win32::System::IO::DeviceIoControl;
use windows::Win32::UI::WindowsAndMessaging::*;

// ============================================================================
// Constants
// ============================================================================

/// Device interface GUID: {A1142000-7DD0-4A11-4200-A114200007DD}
pub const ALLUNO_VDD_INTERFACE_GUID: GUID = GUID::from_u128(0xA1142000_7DD0_4A11_4200_A114200007DD);

pub const ALLUNO_VDD_MAX_DISPLAYS: usize = 16;
pub const ALLUNO_VDD_EDID_MAX_SIZE: usize = 256;
pub const ALLUNO_VDD_DEVICE_NAME_LEN: usize = 14;
pub const ALLUNO_VDD_SERIAL_LEN: usize = 14;

pub const ALLUNO_VDD_BPC_8: u32 = 8;
pub const ALLUNO_VDD_BPC_10: u32 = 10;
pub const ALLUNO_VDD_BPC_12: u32 = 12;

pub const ALLUNO_VDD_HDR_OFF: u32 = 0;
pub const ALLUNO_VDD_HDR_HDR10: u32 = 1;
pub const ALLUNO_VDD_HDR_HDR10_PLUS: u32 = 2;

pub const ALLUNO_VDD_VERSION_MAJOR: u32 = 1;
pub const ALLUNO_VDD_VERSION_MINOR: u32 = 0;
pub const ALLUNO_VDD_VERSION_PATCH: u32 = 0;
pub const ALLUNO_VDD_VERSION_BUILD: u32 = 1;

// IOCTL codes: CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, function, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
#[allow(clippy::identity_op)]
const fn ctl_code(function: u32) -> u32 {
    (0x22 << 16) | (0 << 14) | (function << 2) | 0 // FILE_DEVICE_UNKNOWN, FILE_ANY_ACCESS, METHOD_BUFFERED
}

const IOCTL_ALLUNO_VDD_ADD_DISPLAY: u32 = ctl_code(0x800);
const IOCTL_ALLUNO_VDD_REMOVE_DISPLAY: u32 = ctl_code(0x801);
const IOCTL_ALLUNO_VDD_SET_RENDER_ADAPTER: u32 = ctl_code(0x802);
const IOCTL_ALLUNO_VDD_GET_WATCHDOG: u32 = ctl_code(0x803);
const IOCTL_ALLUNO_VDD_UPDATE_MODE: u32 = ctl_code(0x804);
const IOCTL_ALLUNO_VDD_LIST_DISPLAYS: u32 = ctl_code(0x805);
const IOCTL_ALLUNO_VDD_REMOVE_ALL: u32 = ctl_code(0x806);
const IOCTL_ALLUNO_VDD_SET_WATCHDOG: u32 = ctl_code(0x807);
const IOCTL_ALLUNO_VDD_SET_HDR: u32 = ctl_code(0x808);
const IOCTL_ALLUNO_VDD_SET_CUSTOM_EDID: u32 = ctl_code(0x809);
const IOCTL_ALLUNO_VDD_PING: u32 = ctl_code(0x888);
const IOCTL_ALLUNO_VDD_GET_VERSION: u32 = ctl_code(0x8FF);

// ============================================================================
// Wire structs (must match alluno-vdd-ioctl.h, packed)
// ============================================================================

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawAddParams {
    width: u32,
    height: u32,
    refresh_rate: u32,
    monitor_guid: GUID,
    device_name: [u8; ALLUNO_VDD_DEVICE_NAME_LEN],
    serial_number: [u8; ALLUNO_VDD_SERIAL_LEN],
    bits_per_channel: u32,
    hdr_mode: u32,
    vsync_numerator: u32,
    vsync_denominator: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawAddResult {
    adapter_luid: i64,
    target_id: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawRemoveParams {
    monitor_guid: GUID,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawUpdateModeParams {
    monitor_guid: GUID,
    width: u32,
    height: u32,
    refresh_rate: u32,
    bits_per_channel: u32,
    hdr_mode: u32,
    vsync_numerator: u32,
    vsync_denominator: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawSetAdapterParams {
    adapter_luid: i64,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawWatchdogParams {
    timeout_ms: u32,
    countdown_ms: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawSetWatchdogParams {
    timeout_ms: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawVersion {
    major: u32,
    minor: u32,
    patch: u32,
    build: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawHdrMetadata {
    red_primary_x: u32,
    red_primary_y: u32,
    green_primary_x: u32,
    green_primary_y: u32,
    blue_primary_x: u32,
    blue_primary_y: u32,
    white_point_x: u32,
    white_point_y: u32,
    max_luminance: u32,
    min_luminance_x10000: u32,
    max_content_light_level: u32,
    max_frame_avg_light_level: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawDisplayInfo {
    monitor_guid: GUID,
    width: u32,
    height: u32,
    refresh_rate: u32,
    bits_per_channel: u32,
    hdr_mode: u32,
    adapter_luid: i64,
    target_id: u32,
    device_name: [u8; ALLUNO_VDD_DEVICE_NAME_LEN],
    active: i32,
    vsync_numerator: u32,
    vsync_denominator: u32,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawListResult {
    count: u32,
    displays: [RawDisplayInfo; ALLUNO_VDD_MAX_DISPLAYS],
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawSetHdrParams {
    monitor_guid: GUID,
    hdr_mode: u32,
    bits_per_channel: u32,
    has_metadata: u32,
    metadata: RawHdrMetadata,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct RawSetCustomEdidParams {
    monitor_guid: GUID,
    edid_size: u32,
    edid_data: [u8; ALLUNO_VDD_EDID_MAX_SIZE],
}

// ============================================================================
// Public types
// ============================================================================

/// Result of adding a virtual display.
#[derive(Debug, Clone)]
pub struct AddResult {
    pub adapter_luid: i64,
    pub target_id: u32,
    pub monitor_guid: GUID,
}

/// Driver version.
#[derive(Debug, Clone)]
pub struct Version {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
    pub build: u32,
}

/// Watchdog state.
#[derive(Debug, Clone)]
pub struct WatchdogState {
    pub timeout_ms: u32,
    pub countdown_ms: u32,
}

/// Information about an active virtual display.
#[derive(Debug, Clone)]
pub struct DisplayInfo {
    pub monitor_guid: GUID,
    pub width: u32,
    pub height: u32,
    pub refresh_rate: u32,
    pub bits_per_channel: u32,
    pub hdr_mode: u32,
    pub adapter_luid: i64,
    pub target_id: u32,
    pub device_name: String,
    pub active: bool,
    pub vsync_numerator: u32,
    pub vsync_denominator: u32,
}

/// ST.2086 HDR mastering display metadata.
#[derive(Debug, Clone, Default)]
pub struct HdrMetadata {
    pub red_primary_x: u32,
    pub red_primary_y: u32,
    pub green_primary_x: u32,
    pub green_primary_y: u32,
    pub blue_primary_x: u32,
    pub blue_primary_y: u32,
    pub white_point_x: u32,
    pub white_point_y: u32,
    pub max_luminance: u32,
    pub min_luminance_x10000: u32,
    pub max_content_light_level: u32,
    pub max_frame_avg_light_level: u32,
}

// ============================================================================
// Device handle
// ============================================================================

/// Handle to the Alluno VDD driver.
pub struct AllunoVdd {
    handle: HANDLE,
}

impl AllunoVdd {
    /// Open a handle to the Alluno VDD driver.
    pub fn new() -> Result<Self> {
        let mut list_size: u32 = 0;
        let cr = unsafe {
            CM_Get_Device_Interface_List_SizeW(
                &mut list_size,
                &ALLUNO_VDD_INTERFACE_GUID,
                None,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT,
            )
        };
        if cr != CR_SUCCESS || list_size <= 1 {
            return Err(Error::new(HRESULT(-1), "AllunoVDD device not found"));
        }

        let mut list = vec![0u16; list_size as usize];
        let cr = unsafe {
            CM_Get_Device_Interface_ListW(
                &ALLUNO_VDD_INTERFACE_GUID,
                None,
                &mut list,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT,
            )
        };
        if cr != CR_SUCCESS || list[0] == 0 {
            return Err(Error::new(
                HRESULT(-1),
                "Failed to enumerate device interfaces",
            ));
        }

        let path = windows::core::PCWSTR(list.as_ptr());

        let handle = unsafe {
            CreateFileW(
                path,
                (GENERIC_READ | GENERIC_WRITE).0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                None,
                OPEN_EXISTING,
                FILE_FLAGS_AND_ATTRIBUTES(0),
                None,
            )?
        };

        Ok(Self { handle })
    }

    /// Open a handle to the Alluno VDD driver.
    #[deprecated(note = "use new() instead")]
    pub fn open() -> Result<Self> {
        Self::new()
    }

    // ---- IOCTL helpers ----

    fn ioctl_in_out<I, O>(&self, code: u32, input: &I) -> Result<O> {
        unsafe {
            let mut output: O = zeroed();
            let mut returned: u32 = 0;
            DeviceIoControl(
                self.handle,
                code,
                Some(input as *const I as *const c_void),
                size_of::<I>() as u32,
                Some(&mut output as *mut O as *mut c_void),
                size_of::<O>() as u32,
                Some(&mut returned),
                None,
            )?;
            Ok(output)
        }
    }

    fn ioctl_in<I>(&self, code: u32, input: &I) -> Result<()> {
        unsafe {
            let mut returned: u32 = 0;
            DeviceIoControl(
                self.handle,
                code,
                Some(input as *const I as *const c_void),
                size_of::<I>() as u32,
                None,
                0,
                Some(&mut returned),
                None,
            )?;
            Ok(())
        }
    }

    fn ioctl_out<O>(&self, code: u32) -> Result<O> {
        unsafe {
            let mut output: O = zeroed();
            let mut returned: u32 = 0;
            DeviceIoControl(
                self.handle,
                code,
                None,
                0,
                Some(&mut output as *mut O as *mut c_void),
                size_of::<O>() as u32,
                Some(&mut returned),
                None,
            )?;
            Ok(output)
        }
    }

    fn ioctl_void(&self, code: u32) -> Result<()> {
        unsafe {
            let mut returned: u32 = 0;
            DeviceIoControl(
                self.handle,
                code,
                None,
                0,
                None,
                0,
                Some(&mut returned),
                None,
            )?;
            Ok(())
        }
    }

    // ---- Public API ----

    /// Get driver version.
    pub fn get_version(&self) -> Result<Version> {
        let raw: RawVersion = self.ioctl_out(IOCTL_ALLUNO_VDD_GET_VERSION)?;
        Ok(Version {
            major: raw.major,
            minor: raw.minor,
            patch: raw.patch,
            build: raw.build,
        })
    }

    /// Ping the driver (reset watchdog countdown).
    pub fn ping(&self) -> Result<()> {
        self.ioctl_void(IOCTL_ALLUNO_VDD_PING)
    }

    /// Add a virtual display.
    pub fn add_display(
        &self,
        width: u32,
        height: u32,
        refresh_rate: u32,
        name: &str,
        bits_per_channel: u32,
        hdr_mode: u32,
    ) -> Result<AddResult> {
        let guid = guid_new();
        self.add_display_ex(
            width,
            height,
            refresh_rate,
            name,
            "",
            bits_per_channel,
            hdr_mode,
            guid,
            0,
            0,
        )
    }

    /// Add a virtual display with full control over all parameters.
    #[allow(clippy::too_many_arguments)]
    pub fn add_display_ex(
        &self,
        width: u32,
        height: u32,
        refresh_rate: u32,
        name: &str,
        serial: &str,
        bits_per_channel: u32,
        hdr_mode: u32,
        monitor_guid: GUID,
        vsync_numerator: u32,
        vsync_denominator: u32,
    ) -> Result<AddResult> {
        let mut params: RawAddParams = unsafe { zeroed() };
        params.width = width;
        params.height = height;
        params.refresh_rate = refresh_rate;
        params.monitor_guid = monitor_guid;
        params.bits_per_channel = if bits_per_channel == 0 {
            8
        } else {
            bits_per_channel
        };
        params.hdr_mode = hdr_mode;
        params.vsync_numerator = vsync_numerator;
        params.vsync_denominator = vsync_denominator;
        copy_str_to_buf(name, &mut params.device_name);
        copy_str_to_buf(serial, &mut params.serial_number);

        let raw: RawAddResult = self.ioctl_in_out(IOCTL_ALLUNO_VDD_ADD_DISPLAY, &params)?;
        let result = AddResult {
            adapter_luid: raw.adapter_luid,
            target_id: raw.target_id,
            monitor_guid,
        };

        Ok(result)
    }

    /// Remove a virtual display by GUID.
    pub fn remove_display(&self, monitor_guid: &GUID) -> Result<()> {
        let params = RawRemoveParams {
            monitor_guid: *monitor_guid,
        };
        self.ioctl_in(IOCTL_ALLUNO_VDD_REMOVE_DISPLAY, &params)
    }

    /// Remove all virtual displays.
    pub fn remove_all(&self) -> Result<()> {
        self.ioctl_void(IOCTL_ALLUNO_VDD_REMOVE_ALL)
    }

    /// Update display mode. Pass 0 for fields to keep unchanged. Use 0xFF for hdr_mode to keep unchanged.
    pub fn update_mode(
        &self,
        monitor_guid: &GUID,
        width: u32,
        height: u32,
        refresh_rate: u32,
        bits_per_channel: u32,
        hdr_mode: u32,
    ) -> Result<()> {
        self.update_mode_ex(
            monitor_guid,
            width,
            height,
            refresh_rate,
            bits_per_channel,
            hdr_mode,
            0,
            0,
        )
    }

    /// Update display mode with fractional refresh rate.
    #[allow(clippy::too_many_arguments)]
    pub fn update_mode_ex(
        &self,
        monitor_guid: &GUID,
        width: u32,
        height: u32,
        refresh_rate: u32,
        bits_per_channel: u32,
        hdr_mode: u32,
        vsync_numerator: u32,
        vsync_denominator: u32,
    ) -> Result<()> {
        let params = RawUpdateModeParams {
            monitor_guid: *monitor_guid,
            width,
            height,
            refresh_rate,
            bits_per_channel,
            hdr_mode,
            vsync_numerator,
            vsync_denominator,
        };
        self.ioctl_in(IOCTL_ALLUNO_VDD_UPDATE_MODE, &params)
    }

    /// List all active virtual displays.
    pub fn list_displays(&self) -> Result<Vec<DisplayInfo>> {
        let raw: RawListResult = self.ioctl_out(IOCTL_ALLUNO_VDD_LIST_DISPLAYS)?;
        let count = raw.count as usize;
        let mut displays = Vec::with_capacity(count);
        for i in 0..count.min(ALLUNO_VDD_MAX_DISPLAYS) {
            let d = &raw.displays[i];
            displays.push(DisplayInfo {
                monitor_guid: d.monitor_guid,
                width: d.width,
                height: d.height,
                refresh_rate: d.refresh_rate,
                bits_per_channel: d.bits_per_channel,
                hdr_mode: d.hdr_mode,
                adapter_luid: d.adapter_luid,
                target_id: d.target_id,
                device_name: buf_to_string(&d.device_name),
                active: d.active != 0,
                vsync_numerator: d.vsync_numerator,
                vsync_denominator: d.vsync_denominator,
            });
        }
        Ok(displays)
    }

    /// Set which GPU renders to virtual displays.
    pub fn set_render_adapter(&self, adapter_luid: i64) -> Result<()> {
        let params = RawSetAdapterParams { adapter_luid };
        self.ioctl_in(IOCTL_ALLUNO_VDD_SET_RENDER_ADAPTER, &params)
    }

    /// Get watchdog state.
    pub fn get_watchdog(&self) -> Result<WatchdogState> {
        let raw: RawWatchdogParams = self.ioctl_out(IOCTL_ALLUNO_VDD_GET_WATCHDOG)?;
        Ok(WatchdogState {
            timeout_ms: raw.timeout_ms,
            countdown_ms: raw.countdown_ms,
        })
    }

    /// Set watchdog timeout in milliseconds. 0 = disable.
    pub fn set_watchdog(&self, timeout_ms: u32) -> Result<()> {
        let params = RawSetWatchdogParams { timeout_ms };
        self.ioctl_in(IOCTL_ALLUNO_VDD_SET_WATCHDOG, &params)
    }

    /// Set HDR mode on a display.
    pub fn set_hdr(&self, monitor_guid: &GUID, hdr_mode: u32, bits_per_channel: u32) -> Result<()> {
        let mut params: RawSetHdrParams = unsafe { zeroed() };
        params.monitor_guid = *monitor_guid;
        params.hdr_mode = hdr_mode;
        params.bits_per_channel = bits_per_channel;
        self.ioctl_in(IOCTL_ALLUNO_VDD_SET_HDR, &params)
    }

    /// Set HDR mode with ST.2086 mastering display metadata.
    pub fn set_hdr_with_metadata(
        &self,
        monitor_guid: &GUID,
        hdr_mode: u32,
        bits_per_channel: u32,
        metadata: &HdrMetadata,
    ) -> Result<()> {
        let mut params: RawSetHdrParams = unsafe { zeroed() };
        params.monitor_guid = *monitor_guid;
        params.hdr_mode = hdr_mode;
        params.bits_per_channel = bits_per_channel;
        params.has_metadata = 1;
        params.metadata = RawHdrMetadata {
            red_primary_x: metadata.red_primary_x,
            red_primary_y: metadata.red_primary_y,
            green_primary_x: metadata.green_primary_x,
            green_primary_y: metadata.green_primary_y,
            blue_primary_x: metadata.blue_primary_x,
            blue_primary_y: metadata.blue_primary_y,
            white_point_x: metadata.white_point_x,
            white_point_y: metadata.white_point_y,
            max_luminance: metadata.max_luminance,
            min_luminance_x10000: metadata.min_luminance_x10000,
            max_content_light_level: metadata.max_content_light_level,
            max_frame_avg_light_level: metadata.max_frame_avg_light_level,
        };
        self.ioctl_in(IOCTL_ALLUNO_VDD_SET_HDR, &params)
    }

    /// Set custom EDID for a display. edid must be 128 or 256 bytes.
    pub fn set_custom_edid(&self, monitor_guid: &GUID, edid: &[u8]) -> Result<()> {
        if edid.len() != 128 && edid.len() != 256 {
            return Err(Error::new(HRESULT(-1), "EDID must be 128 or 256 bytes"));
        }
        let mut params: RawSetCustomEdidParams = unsafe { zeroed() };
        params.monitor_guid = *monitor_guid;
        params.edid_size = edid.len() as u32;
        params.edid_data[..edid.len()].copy_from_slice(edid);
        self.ioctl_in(IOCTL_ALLUNO_VDD_SET_CUSTOM_EDID, &params)
    }

    /// Check if driver version is compatible with this crate.
    pub fn is_compatible(&self) -> Result<bool> {
        let v = self.get_version()?;
        #[allow(clippy::absurd_extreme_comparisons)]
        Ok(v.major == ALLUNO_VDD_VERSION_MAJOR && v.minor >= ALLUNO_VDD_VERSION_MINOR)
    }
}

impl Drop for AllunoVdd {
    fn drop(&mut self) {
        if !self.handle.is_invalid() {
            let _ = unsafe { CloseHandle(self.handle) };
        }
    }
}

unsafe impl Send for AllunoVdd {}
unsafe impl Sync for AllunoVdd {}

// ============================================================================
// Advanced Color (HDR) control via Windows Display API
// ============================================================================

/// Enable or disable Advanced Color (HDR) on a display target.
///
/// Uses `DisplayConfigSetDeviceInfo` — does not require the driver handle.
/// Call after `add_display` with the returned `adapter_luid` and `target_id`.
pub fn set_advanced_color(adapter_luid: i64, target_id: u32, enable: bool) -> Result<()> {
    let luid = LUID {
        LowPart: adapter_luid as u32,
        HighPart: (adapter_luid >> 32) as i32,
    };

    let mut state: DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE = unsafe { zeroed() };
    state.header.r#type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    state.header.size = size_of::<DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE>() as u32;
    state.header.adapterId = luid;
    state.header.id = target_id;
    state.Anonymous.Anonymous._bitfield = u32::from(enable);

    let ret = unsafe { DisplayConfigSetDeviceInfo(&state.header) };
    if ret == 0 {
        Ok(())
    } else {
        Err(Error::new(
            HRESULT(ret),
            "DisplayConfigSetDeviceInfo failed",
        ))
    }
}

// ============================================================================
// Resolve GDI device name from LUID + target ID
// ============================================================================

/// Resolve the Windows GDI device name (e.g. `\\.\DISPLAY5`) from the
/// adapter LUID and target ID returned by `add_display`.
///
/// Returns `None` if the path is not found.
pub fn resolve_gdi_device_name(adapter_luid: i64, target_id: u32) -> Option<String> {
    let target_luid = LUID {
        LowPart: adapter_luid as u32,
        HighPart: (adapter_luid >> 32) as i32,
    };

    unsafe {
        let mut path_count = 0u32;
        let mut mode_count = 0u32;
        let flags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;

        if GetDisplayConfigBufferSizes(flags, &mut path_count, &mut mode_count) != WIN32_ERROR(0) {
            return None;
        }

        let mut paths = vec![zeroed::<DISPLAYCONFIG_PATH_INFO>(); path_count as usize];
        let mut modes = vec![zeroed::<DISPLAYCONFIG_MODE_INFO>(); mode_count as usize];

        if QueryDisplayConfig(
            flags,
            &mut path_count,
            paths.as_mut_ptr(),
            &mut mode_count,
            modes.as_mut_ptr(),
            None,
        ) != WIN32_ERROR(0)
        {
            return None;
        }

        for path in &paths[..path_count as usize] {
            if path.sourceInfo.adapterId == target_luid && path.targetInfo.id == target_id {
                let mut source_name: DISPLAYCONFIG_SOURCE_DEVICE_NAME = zeroed();
                source_name.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source_name.header.size = size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32;
                source_name.header.adapterId = path.sourceInfo.adapterId;
                source_name.header.id = path.sourceInfo.id;

                if DisplayConfigGetDeviceInfo(&mut source_name.header) == 0 {
                    let name_slice = &source_name.viewGdiDeviceName;
                    let len = name_slice
                        .iter()
                        .position(|&c| c == 0)
                        .unwrap_or(name_slice.len());
                    let name = String::from_utf16_lossy(&name_slice[..len]);
                    if !name.is_empty() {
                        return Some(name);
                    }
                }
            }
        }
    }

    None
}

// ============================================================================
// Set primary display
// ============================================================================

/// Set a display as the primary monitor using `ChangeDisplaySettingsExW`.
///
/// The primary display defines the (0,0) origin. This function:
/// 1. Enumerates all active displays and their current positions
/// 2. Calculates the offset to move the target display to (0,0)
/// 3. Applies the offset to all displays, preserving relative layout
/// 4. Marks the target as primary with `CDS_SET_PRIMARY`
///
/// `target_device_name` is the Windows GDI device name (e.g., `"\\\\.\\DISPLAY3"`).
pub fn set_primary_display(target_device_name: &str) -> Result<()> {
    // Step 1: Get buffer sizes for active display paths
    let mut num_paths = 0u32;
    let mut num_modes = 0u32;
    let ret = unsafe {
        GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut num_paths, &mut num_modes)
    };
    if ret != WIN32_ERROR(0) {
        return Err(Error::new(
            HRESULT(ret.0 as i32),
            "GetDisplayConfigBufferSizes failed",
        ));
    }

    // Step 2: Query current display configuration
    let mut paths = vec![unsafe { zeroed::<DISPLAYCONFIG_PATH_INFO>() }; num_paths as usize];
    let mut modes = vec![unsafe { zeroed::<DISPLAYCONFIG_MODE_INFO>() }; num_modes as usize];
    let ret = unsafe {
        QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &mut num_paths,
            paths.as_mut_ptr(),
            &mut num_modes,
            modes.as_mut_ptr(),
            None,
        )
    };
    if ret != WIN32_ERROR(0) {
        return Err(Error::new(
            HRESULT(ret.0 as i32),
            "QueryDisplayConfig failed",
        ));
    }
    paths.truncate(num_paths as usize);
    modes.truncate(num_modes as usize);

    // Step 3: Find which source index corresponds to the target device name
    let mut target_source_idx: Option<u32> = None;
    for path in &paths {
        let mut source_name: DISPLAYCONFIG_SOURCE_DEVICE_NAME = unsafe { zeroed() };
        source_name.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source_name.header.size = size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32;
        source_name.header.adapterId = path.sourceInfo.adapterId;
        source_name.header.id = path.sourceInfo.id;

        let ret = unsafe { DisplayConfigGetDeviceInfo(&mut source_name.header) };
        if ret != 0i32 {
            continue;
        }

        let gdi_name = String::from_utf16_lossy(&source_name.viewGdiDeviceName)
            .trim_end_matches('\0')
            .to_string();

        if gdi_name == target_device_name {
            target_source_idx = Some(unsafe { path.sourceInfo.Anonymous.modeInfoIdx });
            break;
        }
    }

    let target_idx = target_source_idx.ok_or_else(|| {
        Error::new(
            HRESULT(-1),
            "Target display not found in DisplayConfig paths",
        )
    })? as usize;

    // Step 4: Get target's current position
    if target_idx >= modes.len() {
        return Err(Error::new(HRESULT(-1), "Target mode index out of range"));
    }
    let target_mode = &modes[target_idx];
    if target_mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE {
        return Err(Error::new(HRESULT(-1), "Target mode is not a source mode"));
    }
    let (off_x, off_y) = unsafe {
        let pos = target_mode.Anonymous.sourceMode.position;
        (pos.x, pos.y)
    };

    // Step 5: Offset all source mode positions so target lands at (0,0)
    for mode in &mut modes {
        if mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE {
            unsafe {
                mode.Anonymous.sourceMode.position.x -= off_x;
                mode.Anonymous.sourceMode.position.y -= off_y;
            }
        }
    }

    // Step 6: Apply the new configuration
    let ret = unsafe {
        SetDisplayConfig(
            Some(&paths),
            Some(&modes),
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE,
        )
    };
    if ret != 0i32 {
        return Err(Error::new(HRESULT(ret), "SetDisplayConfig failed"));
    }

    Ok(())
}

/// Set the display topology.
///
/// Display topology mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DisplayTopology {
    /// Extended desktop — each display is independent.
    Extend,
    /// Duplicate/mirror — all displays show the same content.
    Duplicate,
    /// External only — physical turns off.
    External,
}

/// Set the display topology.
pub fn set_display_topology(topology: DisplayTopology) -> Result<()> {
    let flag = match topology {
        DisplayTopology::Extend => SDC_TOPOLOGY_EXTEND,
        DisplayTopology::Duplicate => SDC_TOPOLOGY_CLONE,
        DisplayTopology::External => SDC_TOPOLOGY_EXTERNAL,
    };
    let ret = unsafe { SetDisplayConfig(None, None, SDC_APPLY | flag) };
    if ret != 0i32 {
        return Err(Error::new(HRESULT(ret), "SetDisplayConfig topology failed"));
    }
    Ok(())
}

// ============================================================================
// Move windows to display
// ============================================================================

/// Move all visible top-level windows to a target display.
///
/// Enumerates all visible top-level windows (including minimized) and repositions
/// them onto the target display. For minimized windows, the restored position is
/// updated via `SetWindowPlacement` without actually restoring them.
///
/// `target_device_name` is the Windows GDI device name (e.g., `"\\\\.\\DISPLAY9"`).
pub fn move_all_windows_to_display(target_device_name: &str) -> Result<u32> {
    // Find the target display's position and size
    let mut target_rect: Option<RECT> = None;
    let mut idx = 0u32;
    loop {
        let mut dd: DISPLAY_DEVICEW = unsafe { zeroed() };
        dd.cb = size_of::<DISPLAY_DEVICEW>() as u32;

        if !unsafe { EnumDisplayDevicesW(PCWSTR(std::ptr::null()), idx, &mut dd, 0) }.as_bool() {
            break;
        }
        idx += 1;

        let name = String::from_utf16_lossy(&dd.DeviceName)
            .trim_end_matches('\0')
            .to_string();

        if name == target_device_name {
            let name_wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
            let mut dm: DEVMODEW = unsafe { zeroed() };
            dm.dmSize = size_of::<DEVMODEW>() as u16;
            unsafe {
                let _ = EnumDisplaySettingsW(
                    PCWSTR(name_wide.as_ptr()),
                    ENUM_CURRENT_SETTINGS,
                    &mut dm,
                );
            }
            let (x, y) = unsafe {
                (
                    dm.Anonymous1.Anonymous2.dmPosition.x,
                    dm.Anonymous1.Anonymous2.dmPosition.y,
                )
            };
            target_rect = Some(RECT {
                left: x,
                top: y,
                right: x + dm.dmPelsWidth as i32,
                bottom: y + dm.dmPelsHeight as i32,
            });
            break;
        }
    }

    let target = target_rect.ok_or_else(|| {
        Error::new(
            HRESULT(-1),
            "Target display not found for move_all_windows_to_display",
        )
    })?;

    let target_w = target.right - target.left;
    let target_h = target.bottom - target.top;

    enum WindowMove {
        Normal(HWND, i32, i32, i32, i32),
        Minimized(HWND, WINDOWPLACEMENT),
    }

    struct MoveCtx {
        target: RECT,
        target_w: i32,
        target_h: i32,
        desktop: HWND,
        shell: HWND,
        moves: Vec<WindowMove>,
    }

    let mut ctx = MoveCtx {
        target,
        target_w,
        target_h,
        desktop: unsafe { GetDesktopWindow() },
        shell: unsafe { GetShellWindow() },
        moves: Vec::new(),
    };

    unsafe extern "system" fn enum_callback(hwnd: HWND, lparam: LPARAM) -> windows::core::BOOL {
        let ctx = &mut *(lparam.0 as *mut MoveCtx);

        if !IsWindowVisible(hwnd).as_bool() {
            return windows::core::BOOL(1);
        }
        if hwnd == ctx.desktop || hwnd == ctx.shell {
            return windows::core::BOOL(1);
        }

        // Skip child windows and tool windows
        let style = GetWindowLongW(hwnd, GWL_STYLE) as u32;
        let ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE) as u32;
        if style & WS_CHILD.0 != 0 {
            return windows::core::BOOL(1);
        }
        if ex_style & WS_EX_TOOLWINDOW.0 != 0 {
            return windows::core::BOOL(1);
        }

        // Skip owned windows (popups owned by another window)
        if GetWindow(hwnd, GW_OWNER)
            .map(|h| h != HWND::default())
            .unwrap_or(false)
        {
            return windows::core::BOOL(1);
        }

        let is_minimized = IsIconic(hwnd).as_bool();

        if is_minimized {
            // Use GetWindowPlacement to read the restored position,
            // update it to the target display, then SetWindowPlacement
            // (keeps the window minimized but changes where it restores to)
            let mut wp: WINDOWPLACEMENT = zeroed();
            wp.length = size_of::<WINDOWPLACEMENT>() as u32;
            if GetWindowPlacement(hwnd, &mut wp).is_err() {
                return windows::core::BOOL(1);
            }

            let r = &wp.rcNormalPosition;
            let win_w = r.right - r.left;
            let win_h = r.bottom - r.top;
            if win_w < 50 || win_h < 50 {
                return windows::core::BOOL(1);
            }

            // Check if restored position is already on target
            let win_cx = r.left + win_w / 2;
            let win_cy = r.top + win_h / 2;
            if win_cx >= ctx.target.left
                && win_cx < ctx.target.right
                && win_cy >= ctx.target.top
                && win_cy < ctx.target.bottom
            {
                return windows::core::BOOL(1);
            }

            let new_w = win_w.min(ctx.target_w);
            let new_h = win_h.min(ctx.target_h);
            let new_x = ctx.target.left + (ctx.target_w - new_w) / 2;
            let new_y = ctx.target.top + (ctx.target_h - new_h) / 2;

            let mut new_wp = wp;
            new_wp.rcNormalPosition = RECT {
                left: new_x,
                top: new_y,
                right: new_x + new_w,
                bottom: new_y + new_h,
            };
            ctx.moves.push(WindowMove::Minimized(hwnd, new_wp));
        } else {
            // Normal (non-minimized) window — use GetWindowRect + MoveWindow
            let mut rect: RECT = zeroed();
            if GetWindowRect(hwnd, &mut rect).is_err() {
                return windows::core::BOOL(1);
            }

            let win_w = rect.right - rect.left;
            let win_h = rect.bottom - rect.top;

            if win_w < 50 || win_h < 50 {
                return windows::core::BOOL(1);
            }

            // Already on target display?
            let win_cx = rect.left + win_w / 2;
            let win_cy = rect.top + win_h / 2;
            if win_cx >= ctx.target.left
                && win_cx < ctx.target.right
                && win_cy >= ctx.target.top
                && win_cy < ctx.target.bottom
            {
                return windows::core::BOOL(1);
            }

            let new_w = win_w.min(ctx.target_w);
            let new_h = win_h.min(ctx.target_h);
            let new_x = ctx.target.left + (ctx.target_w - new_w) / 2;
            let new_y = ctx.target.top + (ctx.target_h - new_h) / 2;

            ctx.moves
                .push(WindowMove::Normal(hwnd, new_x, new_y, new_w, new_h));
        }

        windows::core::BOOL(1)
    }

    unsafe {
        let _ = EnumWindows(
            Some(enum_callback),
            LPARAM(&mut ctx as *mut MoveCtx as isize),
        );
    }

    let count = ctx.moves.len() as u32;
    for m in ctx.moves {
        unsafe {
            match m {
                WindowMove::Normal(hwnd, x, y, w, h) => {
                    let _ = MoveWindow(hwnd, x, y, w, h, true);
                }
                WindowMove::Minimized(hwnd, wp) => {
                    let _ = SetWindowPlacement(hwnd, &wp);
                }
            }
        }
    }

    Ok(count)
}

// ============================================================================
// Display enumeration
// ============================================================================

/// Information about an active system display (physical or virtual).
#[derive(Clone, Debug)]
pub struct SystemDisplayInfo {
    /// GDI device name (e.g., `\\.\DISPLAY9`)
    pub device_name: String,
    /// Adapter/driver description (e.g., "Intel(R) Iris(R) Xe Graphics")
    pub adapter_desc: String,
    pub width: u32,
    pub height: u32,
    pub refresh_rate: u32,
    pub is_primary: bool,
}

/// Enumerate all active system displays.
///
/// Returns physical and virtual displays visible to Windows GDI.
pub fn list_system_displays() -> Vec<SystemDisplayInfo> {
    let mut displays = Vec::new();
    let mut idx = 0u32;
    loop {
        let mut dd: DISPLAY_DEVICEW = unsafe { zeroed() };
        dd.cb = size_of::<DISPLAY_DEVICEW>() as u32;

        if !unsafe { EnumDisplayDevicesW(PCWSTR(std::ptr::null()), idx, &mut dd, 0) }.as_bool() {
            break;
        }
        idx += 1;

        let is_active =
            (dd.StateFlags & DISPLAY_DEVICE_STATE_FLAGS(0x1)) != DISPLAY_DEVICE_STATE_FLAGS(0);
        if !is_active {
            continue;
        }

        let is_primary =
            (dd.StateFlags & DISPLAY_DEVICE_STATE_FLAGS(0x4)) != DISPLAY_DEVICE_STATE_FLAGS(0);

        let name = String::from_utf16_lossy(&dd.DeviceName)
            .trim_end_matches('\0')
            .to_string();
        let desc = String::from_utf16_lossy(&dd.DeviceString)
            .trim_end_matches('\0')
            .to_string();
        let name_wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();

        let mut dm: DEVMODEW = unsafe { zeroed() };
        dm.dmSize = size_of::<DEVMODEW>() as u16;
        let has_mode = unsafe {
            EnumDisplaySettingsW(PCWSTR(name_wide.as_ptr()), ENUM_CURRENT_SETTINGS, &mut dm)
        }
        .as_bool();

        displays.push(SystemDisplayInfo {
            device_name: name,
            adapter_desc: desc,
            width: if has_mode { dm.dmPelsWidth } else { 0 },
            height: if has_mode { dm.dmPelsHeight } else { 0 },
            refresh_rate: if has_mode { dm.dmDisplayFrequency } else { 0 },
            is_primary,
        });
    }

    displays
}

// ============================================================================
// Helpers
// ============================================================================

fn copy_str_to_buf(s: &str, buf: &mut [u8]) {
    let bytes = s.as_bytes();
    let len = bytes.len().min(buf.len());
    buf[..len].copy_from_slice(&bytes[..len]);
}

fn buf_to_string(buf: &[u8]) -> String {
    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    String::from_utf8_lossy(&buf[..end]).to_string()
}

fn guid_new() -> GUID {
    unsafe { windows::Win32::System::Com::CoCreateGuid().unwrap_or(GUID::zeroed()) }
}
