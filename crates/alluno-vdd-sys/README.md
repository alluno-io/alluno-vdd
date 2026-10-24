# alluno-vdd-sys

Rust bindings for the Alluno Virtual Display Driver (AllunoVDD).

Provides safe, ergonomic access to the AllunoVDD kernel driver via Windows IOCTL, allowing userspace applications to create, configure, and remove virtual displays.

## Features

- Add/remove virtual displays with configurable resolution, refresh rate, and bit depth
- Fractional refresh rate support (vsync numerator/denominator)
- HDR support (HDR10, HDR10+) with ST.2086 mastering display metadata
- Custom EDID injection (128 or 256 bytes)
- GPU render adapter selection
- Watchdog timer management
- Display mode hot-update
- Up to 16 simultaneous virtual displays
- Set any display as primary
- Set display topology (extend, duplicate, or external only)
- Move all windows to a target display
- Enumerate all active system displays

## Requirements

- Windows 10/11
- AllunoVDD driver installed (from [alluno-vdd](https://github.com/alluno-io/alluno-vdd))

## Usage

Add to your `Cargo.toml`:

```toml
[dependencies]
alluno-vdd-sys = "1.1.2"
```

```rust
use alluno_vdd_sys::*;

let device = AllunoVdd::new().expect("driver not installed");
let version = device.get_version().unwrap();
println!("Driver v{}.{}.{}", version.major, version.minor, version.patch);

device.set_watchdog(0).unwrap(); // disable watchdog
let result = device.add_display(1920, 1080, 60, "Test", 8, 0).unwrap();
println!("Added display, target_id={}", result.target_id);

// Ensure extended desktop (not duplicate/mirror)
set_display_topology(DisplayTopology::Extend).unwrap();
// Or duplicate: set_display_topology(DisplayTopology::Duplicate).unwrap();
// Or external only (physical display off): set_display_topology(DisplayTopology::External).unwrap();

// Set the virtual display as primary and move all windows to it
set_primary_display("\\\\.\\DISPLAY9").unwrap();
move_all_windows_to_display("\\\\.\\DISPLAY9").unwrap();

let list = device.list_displays().unwrap();
for d in &list {
    println!("{}x{} @{}Hz", d.width, d.height, d.refresh_rate);
}

// List all system displays (physical + virtual)
for d in list_system_displays() {
    let primary = if d.is_primary { " [PRIMARY]" } else { "" };
    println!("{} {}x{} @{}Hz \"{}\"{}", d.device_name, d.width, d.height, d.refresh_rate, d.adapter_desc, primary);
}

device.remove_all().unwrap();
```

## API

### AllunoVdd (driver handle)

| Method | Description |
|---|---|
| `AllunoVdd::new()` | Open a handle to the driver |
| `get_version()` | Query driver protocol version |
| `ping()` | Reset watchdog countdown |
| `add_display()` | Add a virtual display |
| `add_display_ex()` | Add with full parameter control (fractional refresh, serial, GUID) |
| `remove_display()` | Remove a display by GUID |
| `remove_all()` | Remove all virtual displays |
| `update_mode()` | Hot-update resolution/refresh/HDR on an existing display |
| `update_mode_ex()` | Hot-update with fractional refresh rate |
| `list_displays()` | List all active virtual displays |
| `set_render_adapter()` | Set which GPU renders to virtual displays |
| `get_watchdog()` / `set_watchdog()` | Query/configure watchdog timer |
| `set_hdr()` / `set_hdr_with_metadata()` | Enable HDR with optional ST.2086 metadata |
| `set_custom_edid()` | Inject custom EDID (128 or 256 bytes) |
| `is_compatible()` | Check driver protocol compatibility |

### Display management (free functions)

| Function | Description |
|---|---|
| `set_primary_display(device_name)` | Set any display as primary via DisplayConfig API (works with IddCx) |
| `set_display_topology(topology)` | Set display topology: `Extend`, `Duplicate`, or `External` |
| `move_all_windows_to_display(device_name)` | Move all visible windows to a target display (including minimized) |
| `list_system_displays()` | Enumerate all active GDI displays with resolution and primary status |
| `set_advanced_color(luid, target, enable)` | Enable/disable HDR on a display (auto-called for 10bpc) |

## Test Tool

An interactive test binary is included:

```sh
cargo run --bin alluno-vdd-test
```

Commands:
- `1`-`9` — Add displays at various resolutions
- `h1`-`h3` — Add HDR displays
- `0` — Remove all displays
- `L` — List VDD + system displays
- `s<N>` — Set DISPLAY\<N\> as primary (e.g., `s9`)
- `m<N>` — Move all windows to DISPLAY\<N\>
- `P` — Ping driver
- `Q` — Quit

## License

[MIT](LICENSE)
