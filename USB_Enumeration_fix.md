# USB Enumeration: Port Silently Disabled

## Problem

Some USB MIDI devices (e.g. Jieli-based keyboards) may briefly drop and re-assert the D+ pull-up shortly after the host port reset.

On STM32H7 OTG USB Host this can cause:

1. `DISCINT + PCDET` events.
2. Hardware clears `HPRT0.PENA`.
3. The USB Host Library still considers the device connected.
4. Enumeration continues and starts the first control request.
5. The control transfer never completes because the host port is disabled.
6. `USBH_Process()` waits indefinitely for the request to finish.

Typical symptom:

```text
GET_DEV_DESC_8 START
GET_DEV_DESC_8 BUSY
GET_DEV_DESC_8 BUSY
...
```

with no subsequent `HCINT/XFRC` or `URB_DONE`.

## Root Cause

The USB OTG hardware and the USB Host Library state machine become inconsistent:

```text
Hardware:  HPRT0.PENA = 0   (port disabled)
USBH:      is_connected = 1  (device considered connected)
```

The stock USB Host event handling does not reliably detect this particular disconnect/re-attach sequence.

## Fix

`usbh_core.c` contains `USBH_PortLostRecover()`.

It detects:

```c
phost->device.is_connected != 0
```

while:

```c
HPRT0.PENA == 0
```

during the active host/enumeration state.

Recovery performs:

```text
USBH_LL_Stop()
    ↓
deinitialize active USB class
    ↓
DeInitStateMachine()
    ↓
USBH_LL_Start()
    ↓
wait 300 ms
    ↓
HOST_IDLE
    ↓
normal USB reset/enumeration
```

`is_connected` is intentionally preserved so that the normal connection/enumeration sequence can restart without requiring a physical USB reconnect.

## Diagnostics

`HCD_LogGINT()` also logs:

```text
HPRT0
HFNUM
```

along with `GINTSTS`, `GINTMSK`, `HAINT` and `HAINTMSK`.

This makes it possible to distinguish a disabled USB host port from a problem inside the control-transfer state machine.

## Key Lesson

If USB enumeration starts but the first control request remains permanently `BUSY` with no `HCINT/XFRC`, check **`HPRT0.PENA` first**.

A USB Host stack can believe that a device is connected while the OTG hardware has already disabled the port.
