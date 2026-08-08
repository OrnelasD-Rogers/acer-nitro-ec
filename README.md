# acer-nitro-ec

Linux kernel module (hwmon driver) that exposes fan control and temperature
sensors via the standard hwmon sysfs interface for Acer Nitro AN515/AN517
laptops.

Two interchangeable backend implementations are provided — pick whichever
one works on your model:

| Variant | Source file | Talks to hardware via | Module name |
|---------|-------------|------------------------|-------------|
| **EC**  | `ec/acer-nitro-ec.c`   | Direct Embedded Controller register access (`ec_read`/`ec_write`) | `acer-nitro-ec` |
| **WMI** | `wmi/wmi-version.c`    | ACPI-WMI "gaming" method interface (same methods NitroSense/PredatorSense use on Windows) | `acer-nitro-wmi` |

Only install **one** of the two at a time — see
[WMI version](#wmi-version-acpi-wmi-backend) below for why, and
[Build & install](#build--install) for how to choose.

## Supported models

- AN515-44, AN515-46, AN515-54, AN515-56, AN515-57, AN515-58
- AN517-55

## Features

- CPU and GPU fan speed readout (RPM)
- PWM duty-cycle control (0–255)
- Fan mode selection: Turbo / Manual / Auto
- Temperature readings: CPU, GPU, System

## WMI version (ACPI-WMI backend)

`wmi/wmi-version.c` is an **alternative backend for the same driver**, not a
separate project. It exposes the exact same hwmon interface (fan speed,
PWM control, temperature) but talks to the hardware differently.

### Why it exists

The EC version writes fan duty-cycle values directly to Embedded Controller
registers. The EC also runs its own firmware fan-control loop in the
background, and that loop keeps overriding a raw duty-cycle write unless the
EC has first been told — via a separate *mode* register, in the right order —
to hand control over to software. Get that sequencing wrong and the fan
appears stuck to whatever floor the firmware curve enforces.

The WMI version avoids that class of bug entirely by never touching EC
registers directly. Instead it goes through the same ACPI-WMI "gaming"
method interface that Acer's own NitroSense/PredatorSense utilities use on Windows, always setting fan
*behavior/mode* before fan *speed*, in the same order and with the same
payloads the vendor tooling uses.

### Features

- CPU and GPU fan speed readout (RPM) — **live** hardware reads
- CPU and GPU temperature readout, plus a secondary/system sensor — **live**
  hardware reads
- PWM duty-cycle control (0–255) via the vendor "gaming" WMI methods
- Fan mode selection: Turbo / Manual / Auto (same `pwmX_enable` semantics as
  the EC version)
- Per-model sensor support is queried from firmware at load time
  (`GET_SUPPORTED_SENSORS`), so unsupported sensor files are hidden instead
  of returning bogus data
- Scope is intentionally limited to fan + temperature hwmon attributes —
  it does not implement rfkill, backlight, hotkeys, RGB keyboard, battery
  health, accelerometer, or platform-profile, which the upstream `acer-wmi`
  driver this logic is adapted from also covers

### Known protocol quirks

These come from the vendor's own WMI encoding, not from this driver:

1. **Turbo and Auto are whole-system modes.** Only "custom" mode can drive
   CPU and GPU duty independently. Setting `pwm1_enable=0` (turbo) or
   `pwm1_enable=2` (auto) also changes fan2's effective mode.
2. **A requested duty of 0% means "auto", not "stop".** A manual request of
   exactly 0% is nudged up to 1% before being sent, so "manual, very low"
   stays distinguishable from "automatic" to the firmware.
3. **No readback command exists.** There is no documented WMI method to
   query the fan's current behavior mode or commanded duty cycle — only to
   set them. `pwm1`/`pwm2`/`pwm1_enable`/`pwm2_enable` reads report the last
   value this driver successfully wrote (cached in memory), **not** a live
   readback. `fan1_input`/`fan2_input`/`temp*_input` are unaffected and
   remain live hardware reads.

### Requirements (WMI version)

In addition to the general requirements below, your laptop's firmware must
expose the `7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56` ACPI-WMI GUID. The driver
checks for it at load time and refuses to load (with a log message) if it's
missing — in that case, use the EC version instead.

## Requirements

- Linux kernel headers for the running kernel
- `make`
- LLVM toolchain (`clang`, `llvm-ar`, …) — required by default for
  LLVM-built kernels such as CachyOS; can be disabled with `LLVM=0`

## Build & install

### Option A — interactive installer (recommended)

`install.sh` builds and registers whichever variant you pick with DKMS, and
makes sure the other variant is removed first (they must not both be
loaded). Run it from the repo root:

```bash
sudo ./install.sh
```

or non-interactively:

```bash
sudo ./install.sh ec     # install the EC version
sudo ./install.sh wmi    # install the WMI version
sudo ./install.sh uninstall   # remove whichever variant is installed
```

### Option B — build manually with make

Each variant has its own `Makefile` in its own folder:

```bash
cd ec/    # or: cd wmi/

# Build (LLVM=1 is the default)
make

# Build without LLVM (GCC kernels)
make LLVM=0

# Install to the running kernel's module tree
sudo make install
```

## DKMS (auto-rebuild on kernel updates)

`install.sh` (Option A above) already does this for you. To do it by hand
for a specific variant:

```bash
# Pick a variant
VARIANT=ec        # or: wmi
MODULE=acer-nitro-ec     # or: acer-nitro-wmi
VERSION=1.0.0

# Copy sources and register
sudo cp -r "$VARIANT" "/usr/src/$MODULE-$VERSION"
sudo sed -i "s/@PKGVER@/$VERSION/" "/usr/src/$MODULE-$VERSION/dkms.conf"
sudo dkms add    "$MODULE/$VERSION"
sudo dkms build  "$MODULE/$VERSION"
sudo dkms install "$MODULE/$VERSION"
```

## Sysfs interface

After loading, the following files are available under
`/sys/class/hwmon/hwmonX/` (find the right `hwmonX` with
`grep -l acer_nitro_ec /sys/class/hwmon/*/name`):

| File | Access | Description |
|------|--------|-------------|
| `fan1_input` | r | CPU fan speed (RPM) |
| `fan2_input` | r | GPU fan speed (RPM) |
| `pwm1` | rw | CPU fan duty cycle (0–255) |
| `pwm2` | rw | GPU fan duty cycle (0–255) |
| `pwm1_enable` | rw | CPU fan mode: `0`=Turbo, `1`=Manual, `2`=Auto |
| `pwm2_enable` | rw | GPU fan mode: `0`=Turbo, `1`=Manual, `2`=Auto |
| `temp1_input` | r | CPU temperature (m°C) |
| `temp2_input` | r | GPU temperature (m°C) |
| `temp3_input` | r | System temperature (m°C) |

Example — set CPU fan to manual at ~50% and read current speed:

```bash
HWMON=/sys/class/hwmon/$(grep -l acer_nitro_ec /sys/class/hwmon/*/name | cut -d/ -f5)
echo 1   | sudo tee $HWMON/pwm1_enable   # manual mode
echo 128 | sudo tee $HWMON/pwm1          # ~50% duty cycle
cat $HWMON/fan1_input                    # current RPM
```

## Debug / logging

```bash
# Enable verbose logging at load time
sudo modprobe acer-nitro-ec debug=1

# Or enable dynamic_debug at runtime (no reload needed)
echo "module acer_nitro_ec +p" | sudo tee /sys/kernel/debug/dynamic_debug/control

# Watch kernel messages
sudo dmesg -w | grep acer-nitro-ec
```

## Credits

EC register maps reverse-engineered from the
[Linux-NitroSense](https://github.com/JafarAkhondali/linux-nitroshark) project.

WMI method IDs and fan-behavior payload encodings reverse-engineered from
the vendor NitroSense/PredatorSense Windows utilities by the
Linux-NitroSense.
