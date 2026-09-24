# AK506 Nano validation notes

This document records the limits and acceptance observations for the
`dsPIC33AK512MPS506` Classic DRC configuration. It is a guide to the
validated baseline, not a guarantee that every UART transfer size is safe while
audio is running.

## Supported configuration

This is the only hardware-qualified and supported target. AK512 legacy and
AK128 configurations retained in the source tree are regression references,
not supported targets.

## Validated setup

The acceptance setup used:

- a `dsPIC33AK512MPS506` dsPIC33AK Curiosity Nano (EV17P63A);
- a Curiosity Nano Base for Click boards, with the codec in mikroBUS slot 1;
- a WM8904 rev.4 codec board (`WM8904_PCB_REV4`);
- 48 kHz TDM8 audio; and
- the WM8904 as the owner of BCLK and FSYNC, with SPI1 operating as the framed
  SPI client.

The shipping Classic DRC graph has 141 DF2T stages per channel across four DSP
channels: 564 section instances per sample frame. The validated x1s3p default
measured 98.5% peak DSP load with an 8.6-us response margin on this setup.

## Coefficient CSV transfer

The normal coefficient CSV workflow was accepted on the board. A valid table
can be loaded while transport is stopped, and malformed, incomplete, overlong,
or otherwise rejected input aborts without applying a partial table. After an
abort, the console and the next valid transaction recover normally. The loader
uses all-or-nothing application semantics: it reports success only after the
complete table has passed validation and has been applied.

The CSV protocol's staging limit is 30 stages (150 coefficient rows), even
though the shipping DRC graph contains 141 stages per channel. The 141-stage
graph is therefore not a promise that a full 141-stage CSV table can be
transferred through the console.

## Running-audio boundary

A 7,106-byte continuous CSV transfer against an 84-stage, four-channel,
48 kHz high-load baseline exceeded the combined UART receive and audio
real-time budget. The transaction aborted at the receive-integrity guard and
did not apply a partial table. A shorter valid transfer completed, but the
instrumented case also showed temporary audio deadline pressure. This
measurement describes a transfer boundary, not the current default kernel.

This boundary has two independent parts:

1. The receive ring and foreground drain must retain the incoming bytes.
2. The high-priority UART receive ISR can preempt the lower-priority audio work
   while a long line is arriving.

Increasing the receive buffer helps data retention, but it does not remove the
ISR's execution cost from the audio budget. For a reliable user workflow, stop
transport before sending a large table. Treat a running-audio CSV transfer as
best effort and check for `APPLY OK` plus normal audio recovery afterwards.

## Reproducing the baseline checks

Use the Nano serial-monitor profile and verify that the monitor reports the
expected profile and connected board before sending commands. Do not open the
USB serial port directly; the monitor owns it. Test the following sequence:

1. Stop transport and send a valid CSV table.
2. Confirm `APPLY OK` and normal audio operation after restart.
3. Exercise malformed, no-terminator, and overlong input.
4. Confirm that the next valid table still applies and that no partial table
   was installed.
5. Only then, if needed, evaluate a shorter transfer while audio is running.

The measurements above are acceptance evidence for this hardware and build
configuration. They are not a throughput specification for arbitrary host
software, UART settings, or other dsPIC33AK devices.
