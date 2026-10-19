# AK506 Nano test procedure

This is the focused procedure for the AK506 Nano configuration. The only
hardware-qualified and supported target is
`dsPIC33AK512MPS506_CLASSIC_DRC` on the dsPIC33AK Curiosity Nano (EV17P63A).

The source tree may also contain AK512 serial-update and AK128 configurations
for legacy/regression coverage. Their presence does not make them supported
targets, and their regression results do not block this procedure.

## 1. Host and build acceptance

From a clean checkout, select the AK506 standalone application and build it:

```powershell
.\buildtools\switch_config.ps1 -SerialUpdateSupport No `
    -Device dsPIC33AK512MPS506 `
    -Profile "Classic DRC"
.\buildtools\build.ps1 -Full
```

The expected configuration is
`dsPIC33AK512MPS506_CLASSIC_DRC`. A successful build produces the factory HEX
image under:

```text
dist/dsPIC33AK512MPS506_CLASSIC_DRC/production/*.factory.production.hex
```

This target is standalone-only. It does not use a resident downloader,
serial-update application or `.sfb` package.

## 2. Program and boot

1. Connect the Curiosity Nano over USB.
2. Copy the generated factory HEX image to the board's mass-storage
   programmer.
3. Wait for the board to restart.
4. Confirm the console banner identifies the AK506 Nano build and the expected
   commit.

The serial-monitor bridge owns the console UART. Do not open the COM port
directly from another program.

## 3. Focused functional acceptance

Use [AK506 Nano validation](ak506_nano_validation.md) for the validated setup,
CSV transfer boundary and running-audio observations. At minimum, record:

- the board boots and reports the expected firmware banner;
- the WM8904 codec is detected and audio starts at the documented baseline;
- the documented console and CSV-transfer checks complete without a trap or
  unexpected reset;
- the running-audio transfer boundary is reported separately from the
  standalone build result.

Procedures for resident-downloader and serial-update configurations are outside
this AK506 Nano procedure.

## 4. Support boundary

Only `dsPIC33AK512MPS506_CLASSIC_DRC` is hardware-qualified and supported.
AK512 legacy configurations and `dsPIC33AK128_SERIAL_UPDATE` are
informational/non-blocking regression targets; their link or runtime failures
are outside this qualification.
