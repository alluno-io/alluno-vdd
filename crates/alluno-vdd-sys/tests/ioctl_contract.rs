#![cfg(target_os = "windows")]

use std::collections::HashMap;
use std::mem::size_of;

use alluno_vdd_sys::*;
use windows::core::GUID;

const HEADER: &str = include_str!("../../../include/alluno-vdd-ioctl.h");
const RESOURCE: &str = include_str!("../../../src/version.rc");
const PROJECT: &str = include_str!("../../../src/AllunoVDD.vcxproj");
const FILE_DEVICE_UNKNOWN: u32 = 0x22;

fn defines() -> HashMap<&'static str, &'static str> {
    HEADER
        .lines()
        .filter_map(|line| line.trim().strip_prefix("#define "))
        .filter_map(|rest| rest.split_once(char::is_whitespace))
        .map(|(name, value)| (name, value.trim()))
        .collect()
}

fn number(text: &str) -> u64 {
    let text = text.trim();
    match text.strip_prefix("0x") {
        Some(hex) => u64::from_str_radix(hex, 16).unwrap(),
        None => text.parse().unwrap(),
    }
}

fn header_number(name: &str) -> u64 {
    number(defines()[name])
}

fn header_ioctl(name: &str) -> u32 {
    let value = defines()[name];
    let arguments = value
        .strip_prefix("CTL_CODE(")
        .and_then(|rest| rest.strip_suffix(')'))
        .unwrap_or_else(|| panic!("{name} is not a CTL_CODE"));
    let arguments: Vec<&str> = arguments.split(',').map(str::trim).collect();
    assert_eq!(
        [arguments[0], arguments[2], arguments[3]],
        ["FILE_DEVICE_UNKNOWN", "METHOD_BUFFERED", "FILE_ANY_ACCESS"],
        "{name}"
    );
    (FILE_DEVICE_UNKNOWN << 16) | ((number(arguments[1]) as u32) << 2)
}

fn without_comments(text: &str) -> String {
    let mut kept = String::new();
    let mut rest = text;
    while let Some((before, after)) = rest.split_once("/*") {
        kept.push_str(before);
        rest = after.split_once("*/").map_or("", |(_, tail)| tail);
    }
    kept.push_str(rest);
    kept.lines()
        .map(|line| line.split("//").next().unwrap_or_default())
        .collect::<Vec<_>>()
        .join("\n")
}

fn header_struct_sizes() -> HashMap<String, usize> {
    let constants = defines();
    let mut sizes: HashMap<String, usize> = [
        ("UINT", 4),
        ("BOOL", 4),
        ("GUID", 16),
        ("LUID", 8),
        ("CHAR", 1),
        ("BYTE", 1),
    ]
    .into_iter()
    .map(|(kind, size)| (kind.to_string(), size))
    .collect();
    let packed = HEADER
        .split_once("#pragma pack(push, 1)")
        .and_then(|(_, rest)| rest.split_once("#pragma pack(pop)"))
        .map(|(packed, _)| without_comments(packed))
        .expect("the header packs its structs");
    let mut declared = HashMap::new();
    for definition in packed.split("typedef struct").skip(1) {
        let (fields, tail) = definition.split_once('}').unwrap();
        let name = tail
            .trim_start()
            .split(';')
            .next()
            .unwrap()
            .trim()
            .to_string();
        let size = fields
            .split_once('{')
            .unwrap()
            .1
            .split(';')
            .map(str::trim)
            .filter(|field| !field.is_empty())
            .map(|field| {
                let (kind, declarator) = field.split_once(char::is_whitespace).unwrap();
                let count = declarator.split_once('[').map_or(1, |(_, length)| {
                    let length = length.trim_end_matches(']').trim();
                    constants
                        .get(length)
                        .map_or_else(|| number(length), |value| number(value))
                        as usize
                });
                sizes[kind] * count
            })
            .sum::<usize>();
        sizes.insert(name.clone(), size);
        declared.insert(name, size);
    }
    declared
}

#[test]
fn every_ioctl_code_is_the_one_the_header_declares() {
    let codes = [
        ("IOCTL_ALLUNO_VDD_ADD_DISPLAY", IOCTL_ALLUNO_VDD_ADD_DISPLAY),
        (
            "IOCTL_ALLUNO_VDD_REMOVE_DISPLAY",
            IOCTL_ALLUNO_VDD_REMOVE_DISPLAY,
        ),
        (
            "IOCTL_ALLUNO_VDD_SET_RENDER_ADAPTER",
            IOCTL_ALLUNO_VDD_SET_RENDER_ADAPTER,
        ),
        (
            "IOCTL_ALLUNO_VDD_GET_WATCHDOG",
            IOCTL_ALLUNO_VDD_GET_WATCHDOG,
        ),
        ("IOCTL_ALLUNO_VDD_UPDATE_MODE", IOCTL_ALLUNO_VDD_UPDATE_MODE),
        (
            "IOCTL_ALLUNO_VDD_LIST_DISPLAYS",
            IOCTL_ALLUNO_VDD_LIST_DISPLAYS,
        ),
        ("IOCTL_ALLUNO_VDD_REMOVE_ALL", IOCTL_ALLUNO_VDD_REMOVE_ALL),
        (
            "IOCTL_ALLUNO_VDD_SET_WATCHDOG",
            IOCTL_ALLUNO_VDD_SET_WATCHDOG,
        ),
        ("IOCTL_ALLUNO_VDD_SET_HDR", IOCTL_ALLUNO_VDD_SET_HDR),
        (
            "IOCTL_ALLUNO_VDD_SET_CUSTOM_EDID",
            IOCTL_ALLUNO_VDD_SET_CUSTOM_EDID,
        ),
        ("IOCTL_ALLUNO_VDD_PING", IOCTL_ALLUNO_VDD_PING),
        ("IOCTL_ALLUNO_VDD_GET_VERSION", IOCTL_ALLUNO_VDD_GET_VERSION),
    ];
    for (name, code) in codes {
        assert_eq!(code, header_ioctl(name), "{name}");
    }
    let declared = defines()
        .keys()
        .filter(|name| name.starts_with("IOCTL_"))
        .count();
    assert_eq!(
        declared,
        codes.len(),
        "an IOCTL in the header has no binding"
    );
}

#[test]
fn the_limits_and_modes_are_the_ones_the_header_declares() {
    let values: [(&str, u64); 10] = [
        ("ALLUNO_VDD_MAX_DISPLAYS", ALLUNO_VDD_MAX_DISPLAYS as u64),
        ("ALLUNO_VDD_EDID_MAX_SIZE", ALLUNO_VDD_EDID_MAX_SIZE as u64),
        (
            "ALLUNO_VDD_DEVICE_NAME_LEN",
            ALLUNO_VDD_DEVICE_NAME_LEN as u64,
        ),
        ("ALLUNO_VDD_SERIAL_LEN", ALLUNO_VDD_SERIAL_LEN as u64),
        ("ALLUNO_VDD_BPC_8", ALLUNO_VDD_BPC_8.into()),
        ("ALLUNO_VDD_BPC_10", ALLUNO_VDD_BPC_10.into()),
        ("ALLUNO_VDD_BPC_12", ALLUNO_VDD_BPC_12.into()),
        ("ALLUNO_VDD_HDR_OFF", ALLUNO_VDD_HDR_OFF.into()),
        ("ALLUNO_VDD_HDR_HDR10", ALLUNO_VDD_HDR_HDR10.into()),
        (
            "ALLUNO_VDD_HDR_HDR10_PLUS",
            ALLUNO_VDD_HDR_HDR10_PLUS.into(),
        ),
    ];
    for (name, value) in values {
        assert_eq!(value, header_number(name), "{name}");
    }
}

#[test]
fn the_driver_version_agrees_across_the_header_the_resource_the_inf_stamp_and_the_bindings() {
    let header = ["MAJOR", "MINOR", "PATCH", "BUILD"]
        .map(|part| header_number(&format!("ALLUNO_VDD_VERSION_{part}")).to_string())
        .join(".");
    let resource = RESOURCE
        .lines()
        .find_map(|line| line.trim().strip_prefix("#define VER_PRODUCTVERSION_STR"))
        .map(|value| value.trim().trim_matches('"').to_string())
        .expect("version.rc declares VER_PRODUCTVERSION_STR");
    let stamps: Vec<&str> = PROJECT
        .split("<TimeStamp>")
        .skip(1)
        .filter_map(|rest| {
            rest.split_once("</TimeStamp>")
                .map(|(stamp, _)| stamp.trim())
        })
        .collect();
    let bindings = format!(
        "{ALLUNO_VDD_VERSION_MAJOR}.{ALLUNO_VDD_VERSION_MINOR}.{ALLUNO_VDD_VERSION_PATCH}.{ALLUNO_VDD_VERSION_BUILD}"
    );
    assert_eq!(
        header, resource,
        "the header's GET_VERSION answer against version.rc"
    );
    assert!(
        !stamps.is_empty(),
        "the vcxproj stamps the INF with a TimeStamp"
    );
    for stamp in stamps {
        assert_eq!(
            header, stamp,
            "the header's GET_VERSION answer against the INF stamp"
        );
    }
    assert_eq!(bindings, header, "the bindings' version against the header");
}

#[test]
fn the_interface_guid_is_the_one_the_header_declares() {
    let line = HEADER
        .lines()
        .find(|line| line.contains("ALLUNO_VDD_INTERFACE_GUID ="))
        .expect("the header declares the interface GUID");
    let parts: Vec<u64> = line
        .split(|c: char| !c.is_ascii_alphanumeric())
        .filter_map(|token| token.strip_prefix("0x"))
        .map(|hex| u64::from_str_radix(hex, 16).unwrap())
        .collect();
    assert_eq!(parts.len(), 11);
    let tail: [u8; 8] = parts[3..]
        .iter()
        .map(|&byte| byte as u8)
        .collect::<Vec<_>>()
        .try_into()
        .unwrap();
    let declared = GUID::from_values(parts[0] as u32, parts[1] as u16, parts[2] as u16, tail);
    assert_eq!(ALLUNO_VDD_INTERFACE_GUID, declared);
}

#[test]
fn every_wire_struct_is_packed_to_the_size_the_header_declares() {
    let declared = header_struct_sizes();
    let bindings = [
        ("ALLUNO_VDD_ADD_PARAMS", size_of::<RawAddParams>()),
        ("ALLUNO_VDD_ADD_RESULT", size_of::<RawAddResult>()),
        ("ALLUNO_VDD_REMOVE_PARAMS", size_of::<RawRemoveParams>()),
        (
            "ALLUNO_VDD_UPDATE_MODE_PARAMS",
            size_of::<RawUpdateModeParams>(),
        ),
        (
            "ALLUNO_VDD_SET_ADAPTER_PARAMS",
            size_of::<RawSetAdapterParams>(),
        ),
        ("ALLUNO_VDD_WATCHDOG_PARAMS", size_of::<RawWatchdogParams>()),
        (
            "ALLUNO_VDD_SET_WATCHDOG_PARAMS",
            size_of::<RawSetWatchdogParams>(),
        ),
        ("ALLUNO_VDD_VERSION", size_of::<RawVersion>()),
        ("ALLUNO_VDD_HDR_METADATA", size_of::<RawHdrMetadata>()),
        ("ALLUNO_VDD_DISPLAY_INFO", size_of::<RawDisplayInfo>()),
        ("ALLUNO_VDD_LIST_RESULT", size_of::<RawListResult>()),
        ("ALLUNO_VDD_SET_HDR_PARAMS", size_of::<RawSetHdrParams>()),
        (
            "ALLUNO_VDD_SET_CUSTOM_EDID_PARAMS",
            size_of::<RawSetCustomEdidParams>(),
        ),
    ];
    for (name, size) in bindings {
        assert_eq!(size, declared[name], "{name}");
    }
    assert_eq!(
        declared.len(),
        bindings.len(),
        "a struct in the header has no binding"
    );
}
