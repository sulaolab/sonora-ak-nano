# Build tools

`buildtools/` is the command-line build and configuration toolset for this
source tree. The AK506 Nano path is intentionally small: choose the device and
application profile with `switch_config.ps1`, then build with
`build.ps1`.

## AK506 Nano scope

The hardware-qualified target is `dsPIC33AK512MPS506_CLASSIC_DRC`.
`dsPIC33AK512MPS506_CLASSIC` provides Classic 1 for PWM PCB first-light
preparation with build verification only. Other configurations
may remain in the source tree as legacy/regression material; they are
informational and non-blocking, not supported targets.

| Item | Value |
| --- | --- |
| Device | `dsPIC33AK512MPS506` |
| Board | dsPIC33AK Curiosity Nano (EV17P63A) |
| Application | Classic DRC |
| Delivery | Standalone application (`SerialUpdateSupport = No`) |
| MPLAB X configuration | `dsPIC33AK512MPS506_CLASSIC_DRC` |

The AK506 configuration has no resident bootloader and no serial-update
package in its baseline flow. Build the standalone application and program its
factory HEX through the Curiosity Nano mass-storage programmer. The
Nano-specific acceptance used that route; the older board-programming helper
used by other dsPIC33AK boards is not an AK506 supported path.

## Prerequisites

- PowerShell 7 (`pwsh`)
- MPLAB X and XC-DSC 3.31.01, with `dsPIC33AK-MP_DFP` 1.5.269 installed
- Python 3.11+ for the host-side tools and build checks

The command-line build is the reproducible project path. MPLAB X may be used
to inspect, debug and build the application project, but the script path is the
reference for selecting the configuration and locating the release artifact.

## Quick start

From the repository root:

```powershell
.\buildtools\switch_config.ps1 -SerialUpdateSupport No `
    -Device dsPIC33AK512MPS506 `
    -Profile "Classic DRC"
.\buildtools\build.ps1
```

`switch_config.ps1 -List` prints the available catalog and the current
selection without changing it. A selection is stored in the untracked
`buildtools/active_build.json`; it is local machine state, not a release input.

For a clean rebuild:

```powershell
.\buildtools\build.ps1 -Full
```

The build itself checks the MPLAB configuration catalog and the relevant map
and image invariants. Do not edit generated project state under
`nbproject/private/` or commit build output.

`buildtools/clean.ps1` is a helper for that same path and requires
`MPLABX_CONF`; `build.ps1` supplies it before invoking the helper. A direct
clean without an explicit configuration fails instead of guessing a legacy
AK512 target.

## Configuration resolution

The MPLAB X configuration is derived from the device, delivery mode and
application profile. It is not a fourth choice that can silently disagree with
the command-line selection.

| Serial update | Device | Profile | Configuration |
| --- | --- | --- | --- |
| `No` | `dsPIC33AK512MPS506` | Classic DRC | `dsPIC33AK512MPS506_CLASSIC_DRC` |
| `No` | `dsPIC33AK512MPS506` | Classic 1 | `dsPIC33AK512MPS506_CLASSIC` |

This is the only selection described and supported by the AK506 Nano workflow.
Qualification blocks only on `dsPIC33AK512MPS506_CLASSIC_DRC`.

The device/profile resolution is also available through the script's internal
diagnostic output. Unsupported combinations are rejected rather than silently
falling back to another delivery mode.

## PWM PCB preparation

Use WM8904 rev.4 in Nano Base slot 1 and MPQ6612A non-D PWM PCB in slot 3.
The default preparation command builds Classic 1 with PWM enabled and saves
a named factory HEX outside `dist`:

```powershell
.\buildtools\prepare_pwm_mikrobus.ps1
.\buildtools\verify_pwm_mikrobus.ps1
```

Both scripts default to slot 3. `-Slot 2` supports the earlier wiring.
Verification prints the saved HEX path and checks its SHA256 and factory
metadata; it does not access the board. Copy the verified factory HEX to
the intended Nano mass-storage programmer when hardware is available.
PWM uses a 48 kHz L-channel audio stream and a 192 kHz carrier by default.
A plain `build.ps1` keeps PWM off, so use `prepare_pwm_mikrobus.ps1` for the
PWM ROM image.
Slot 2 POT sampling and AVAS pitch control are outside this change.

## Release artifact

For the AK506 selection, the application factory HEX is written below:

```text
dist/dsPIC33AK512MPS506_CLASSIC_DRC/production/*.factory.production.hex
```

This is a standalone application image. It is not a serial-update package and
does not require a resident image to be combined with it.

The compiler may produce different binary layout details between builds of the
same sources. Treat the map checks, configuration checks and firmware
behaviour as the release evidence; do not use a HEX hash as a reproducibility
claim.

## Programming the Nano

After a successful build:

1. Connect the Curiosity Nano to the host over USB.
2. Confirm that the programmer identifies the target as
   `dsPIC33AK512MPS506`.
3. Copy the generated `*.factory.production.hex` factory HEX image to the Nano's
   mass-storage programmer.
4. Wait for the board to restart, then verify the firmware build banner on the
   console.

The console is 230400 8N1 over the Nano on-board debugger CDC connection. The
serial-monitor bridge owns that port when it is used for console work; do not
open the USB serial port directly from a second program.
