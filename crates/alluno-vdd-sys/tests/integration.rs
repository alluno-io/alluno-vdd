//! Integration tests for alluno-vdd-sys
//!
//! Tests skip gracefully if the AllunoVDD driver is not installed.
//! Run with: cargo test -- --nocapture
//!
//! Tests are serialized via a global mutex since they share one driver instance.

use alluno_vdd_sys::*;
use std::sync::Mutex;

static DRIVER_LOCK: Mutex<()> = Mutex::new(());

macro_rules! locked_test {
    (|$device:ident| $body:block) => {{
        let _guard = DRIVER_LOCK.lock().unwrap();
        let Some($device) = AllunoVdd::new().ok() else {
            eprintln!("skipped: driver not installed");
            return;
        };
        $body
    }};
}

#[test]
fn test_open_device() {
    match AllunoVdd::new() {
        Ok(_) => eprintln!("driver opened successfully"),
        Err(_) => eprintln!("skipped: driver not installed"),
    }
}

#[test]
fn test_get_version() {
    locked_test!(|device| {
        let version = device.get_version().expect("get_version failed");

        assert_eq!(version.major, ALLUNO_VDD_VERSION_MAJOR);
        #[allow(clippy::absurd_extreme_comparisons)]
        {
            assert!(version.minor >= ALLUNO_VDD_VERSION_MINOR);
        }
        println!(
            "Driver version: {}.{}.{}.{}",
            version.major, version.minor, version.patch, version.build
        );
    });
}

#[test]
fn test_is_compatible() {
    locked_test!(|device| {
        assert!(
            device.is_compatible().expect("is_compatible failed"),
            "Driver is not compatible with this crate"
        );
    });
}

#[test]
fn test_ping() {
    locked_test!(|device| {
        device.ping().expect("ping failed");
    });
}

#[test]
fn test_get_watchdog() {
    locked_test!(|device| {
        let state = device.get_watchdog().expect("get_watchdog failed");
        println!(
            "Watchdog: timeout={}ms, countdown={}ms",
            state.timeout_ms, state.countdown_ms
        );
    });
}

#[test]
fn test_set_watchdog_disable_and_restore() {
    locked_test!(|device| {
        let original = device.get_watchdog().expect("get_watchdog failed");
        device.set_watchdog(0).expect("set_watchdog(0) failed");
        device
            .set_watchdog(original.timeout_ms)
            .expect("set_watchdog restore failed");
    });
}

#[test]
fn test_list_displays_empty() {
    locked_test!(|device| {
        device.set_watchdog(0).expect("set_watchdog failed");
        let _ = device.remove_all();

        let displays = device.list_displays().expect("list_displays failed");
        assert!(
            displays.is_empty(),
            "Expected no displays, got {}",
            displays.len()
        );
    });
}

#[test]
fn test_add_remove_display() {
    locked_test!(|device| {
        device.set_watchdog(0).expect("set_watchdog failed");
        let _ = device.remove_all();

        let result = device
            .add_display(1920, 1080, 60, "Test", 8, 0)
            .expect("add_display failed");
        println!(
            "Added: target_id={}, guid={:?}",
            result.target_id, result.monitor_guid
        );

        let displays = device.list_displays().expect("list_displays failed");
        assert_eq!(displays.len(), 1);
        assert_eq!(displays[0].width, 1920);
        assert_eq!(displays[0].height, 1080);

        device
            .remove_display(&result.monitor_guid)
            .expect("remove_display failed");

        let displays = device.list_displays().expect("list_displays failed");
        assert!(displays.is_empty());
    });
}

#[test]
fn test_add_multiple_and_remove_all() {
    locked_test!(|device| {
        device.set_watchdog(0).expect("set_watchdog failed");
        let _ = device.remove_all();

        let _r1 = device
            .add_display(1920, 1080, 60, "Disp1", 8, 0)
            .expect("add 1 failed");
        let _r2 = device
            .add_display(2560, 1440, 144, "Disp2", 8, 0)
            .expect("add 2 failed");
        let _r3 = device
            .add_display(3840, 2160, 60, "Disp3", 8, 0)
            .expect("add 3 failed");

        let displays = device.list_displays().expect("list failed");
        assert_eq!(displays.len(), 3);

        device.remove_all().expect("remove_all failed");

        let displays = device.list_displays().expect("list failed");
        assert!(displays.is_empty());
    });
}

#[test]
fn test_add_display_with_fractional_refresh() {
    locked_test!(|device| {
        device.set_watchdog(0).expect("set_watchdog failed");
        let _ = device.remove_all();

        let result = device
            .add_display_ex(
                1920,
                1080,
                60,
                "Frac",
                "SN001",
                8,
                0,
                guid_new_test(),
                60000,
                1001,
            )
            .expect("add_display_ex failed");

        let displays = device.list_displays().expect("list failed");
        assert_eq!(displays.len(), 1);
        println!(
            "Fractional: {}x{} vsync={}/{}",
            displays[0].width,
            displays[0].height,
            displays[0].vsync_numerator,
            displays[0].vsync_denominator
        );

        device
            .remove_display(&result.monitor_guid)
            .expect("remove failed");
    });
}

fn guid_new_test() -> windows::core::GUID {
    unsafe { windows::Win32::System::Com::CoCreateGuid().unwrap() }
}
