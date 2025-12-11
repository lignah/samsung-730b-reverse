Last updated: 2025‑12‑12

# Vendor protocol notes for Samsung 730B fingerprint sensor

USB protocol document for Samsung 730B fingerprint sensor (VID 0x04E8, PID 0x730B)

    Driver traffic captured with Windows USBPCap + Wireshark
    Reverse engineering based on Linux usbmon + libusb/pyusb
    Python Driver (730b.py )
    C/libusb driver (730b.c)
    libfprint driver (Samsung 730b.c)

## 0. Stack overview

Linux fingerprint stack (high level):

```text
[Sensor hardware (Samsung 730B)]
        ↑ USB (bulk + control, vendor-specific)
[Low-level driver (Python / libusb C)]
        ↑ Image / template API
[libfprint driver (samsung730b)]
        ↑ D-Bus
[fprintd]
        ↑ PAM / desktop lock screen
[Users]
```

## 1. Device / USB basics

- Vendor ID: 0x04E8 (Samsung)
- Product ID: 0x730B
- Interface:
    - Interface 0, Alternate Setting 0
    - Endpoints:
        - Bulk OUT: 0x01
        - Bulk IN : 0x82
    - Control transfers:
        - bmRequestType = 0x40 (Host → Device, Vendor, Device)
        - Known bRequest values:
            - 0xC3 : initialization
            - 0xCA : capture index / chunk selection

These values are used consistently in:

    The Python driver
    The C/libusb test program
    The libfprint samsung730b driver

## 2. Standard USB sequence (OS level)

On Windows (USBPcap trace), before the vendor‑specific protocol starts, the OS performs the usual USB enumeration:

```
GET_DESCRIPTOR (DEVICE, CONFIGURATION)
SET_CONFIGURATION
...
```

On Linux, libusb / pyusb’s set_configuration() plays the same role. From the sensor protocol point of view this sequence is not interesting.

## 3. Sensor initialization

### 3.1 Control initialization (0xC3)

Initial control commands commonly used by Python/C drivers:

```py
ctrl_data = bytes([
	0x80, 0x84, 0x1e, 0x00,
	0x08, 0x00, 0x00, 0x01,
	0x01, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00
])
self._ctrl_write(0xC3, 0x0000, 0x0000, ctrl_data)
```

Equivalent in the libfprint driver:

```c
guint8 c3_data[16] = {
  0x80, 0x84, 0x1e, 0x00, 0x08, 0x00, 0x00, 0x01,
  0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

usb_ctrl_out (usb_dev, 0xC3, 0x0000, 0x0000, c3_data, sizeof (c3_data),
              500, error);
```

The exact meaning of this structure (register map / bitfields) is still unknown. Based on behavior it likely configures basic sensor mode/power/clock.


### 3.2 0xA9 / 0xA8 register sequence (bulk OUT)

In the Windows USBPcap trace, the driver frequently sends bulk OUT packets of length 31 bytes where Wireshark shows a 4‑byte Leftover Capture Data section at the end, always starting with 0xA9 or 0xA8. Example:

```
URB_BULK out, length 31 ... Leftover Capture Data: a9 60 1b 00
URB_BULK out, length 31 ... Leftover Capture Data: a9 50 21 00
URB_BULK out, length 31 ... Leftover Capture Data: a9 61 00 00
```

Observations:

    The first 27 bytes appear to be host/stack wrapping, not understood.
    The last 3–4 bytes (starting with 0xA9 / 0xA8) are the real sensor commands.

The Python and C drivers only send these trailing bytes directly to the sensor:


```python
init_cmds = [
    bytes([0x4f, 0x80]),
    bytes([0xa9, 0x4f, 0x80]),
    bytes([0xa8, 0xb9, 0x00]),
    bytes([0xa9, 0x60, 0x1b, 0x00]),
    bytes([0xa9, 0x50, 0x21, 0x00]),
    bytes([0xa9, 0x61, 0x00, 0x00]),
    ...
]

for cmd in init_cmds:
    ep_out.write(cmd)
```

The libfprint driver has the same list as init_cmds[] and sends each entry with a bulk OUT.

Most commands are 4 bytes; some are 2 or 3 bytes. The semantics are still unclear, but they likely tune gain, timing, noise filtering, etc.

## 4. Capture protocol

### 4.1 Overview from Windows traces

In the USBPcap capture from Windows, the image capture sequence roughly looks like:

    A vendor control request (0xCA, sometimes 0xCC)
    A larger bulk OUT containing a8 06 00 00 (capture start) + zero padding
    Repeating bulk IN/OUT transfers that, when decoded, correspond to blocks of image data

On Windows, the host payload is wrapped in 27‑byte headers, but the sensor‑relevant part is a repeating pattern of:

    Control OUT with index increasing by 0x0100
    Bulk OUT of a8 06 00 00 ... once (start)
    Bulk IN of 256‑byte chunks (image data)
    Bulk OUT of 256 bytes of zeros (ACK)

### 4.2 Python capture [`samsung_730b.py`](../scripts/samsung_730b.py)


```python
CAPTURE_CHUNK_SIZE = 256
CAPTURE_NUM_CHUNKS = 85
CAPTURE_START_INDEX = 0x032A

image_data = bytearray()

for i in range(CAPTURE_NUM_CHUNKS):
    offset = i * CAPTURE_CHUNK_SIZE
    wIndex = CAPTURE_START_INDEX + offset

    # 1) control 0xCA: select chunk
    dev.ctrl_transfer(0x40, 0xCA, 0x0003, wIndex & 0xFFFF, None)

    # 2) first chunk: capture start
    if i == 0:
        capture_cmd = bytes([0xa8, 0x06, 0x00, 0x00]) + bytes(252)
        ep_out.write(capture_cmd)

    # 3) bulk IN 256 bytes
    data = ep_in.read(256, timeout=1000)
    image_data.extend(data)

    # 4) ACK: bulk OUT 256 zero bytes
    ep_out.write(bytes(256))
```

Originally this code assumed that all chunks behave the same. Later investigation with usbmon on Linux revealed a subtle difference for chunk 0 (see below in the C/libusb section), but the overall pattern remains identical.

Typical total length: ~21.5 KB.

### 4.3 Capture layout and effective image area

Through experiments (sliding offsets, trying various resolutions), the following facts were established:

    The full capture buffer is around 21,500 bytes.

    Only a subrange corresponds to a clean single fingerprint frame.

    The only resolution that produces a reasonable, fingerprint‑like image is:
        112 x 96 (width x height), 8‑bit grayscale

    Initially, taking the first 112×96 bytes showed strong vertical and horizontal artifacts (border lines).

    By scanning offsets, the cleanest image (no central vertical line, no noisy first row) was found at:
        Offset (IMG_OFFSET / BEST_OFFSET) = 180 bytes

Thus the effective image area is:

    Start: buffer[180]
    Size: 112 * 96 = 10752 bytes

The libfprint driver currently uses the same constants:


## 4.4 Orientation

    - The "top" of the sensor internal coordinate system is mapped to the "right" on the image buffer
    - In order to look intuitive to the human eye, the Python driver rotates 90 degrees to the left (CCW)

    Note: The libprint driver is currently handing over raw layout (112x96) to FpImage without rotation. Matching quality should be reviewed to see if rotation is applied

## 5. C/libusb driver [`samsung_730b.c`](../scripts/samsung_730b.c)

### 5.1. Fixing chunk 0 behavior

Initial versions of the C program assumed that each 256‑byte chunk was identical. This caused timeouts and only a few bytes captured.

By comparing Python (usbmon logs) and C traces, the correct behavior was identified:

    Chunk 0 (index 0x032A):
        Control OUT 0xCA, wIndex=0x032A, wValue=0x0003
        Bulk OUT 256 bytes:
            First 4 bytes: a8 06 00 00 (capture start)
            Padding: zeros
        Bulk IN 256 bytes requested, but the device only returns 0–2 bytes (a status/acknowledgement)
        These bytes are treated as a “capture mode entry ACK” and discarded.
        No ACK bulk OUT is sent for chunk 0.

    Chunks 1..84:
        Control OUT 0xCA with wIndex = 0x032A + n * 0x0100 (n = 1..84)
            In practice, the exact index sequence is taken from Python logs and hard‑coded in capture_indices[].
        Bulk IN 256 bytes: actual image data
        Bulk OUT 256 bytes of zeros: ACK
        This pattern is repeated for all 84 data chunks, yielding ~21.5 KB.

The bug in the first C version was that it tried to send an ACK on chunk 0, which confused the sensor and prevented it from entering streaming mode.

With the corrected behavior, C and Python both produce valid fingerprints, and .pgm images match.

### 5.2. Finger‑detect probe in C

A shortened capture path is implemented for finger detection:

    Use DETECT_PACKETS small chunks (e.g. 6) instead of the full 85.
    Sequence is the same as in capture, but only part of the data is kept.
    The aggregated probe buffer is inspected for the distribution of 0x00 and 0xFF values.

Heuristic (identical to Python):

```py
ff_ratio = (double) ff / total;
zero_ratio = (double) zeros / total;

if (ff_ratio > 0.30 && zeros < total * 0.95)
    finger present
else
    no finger
```

## 6. libfprint driver [`samsung730b.c`](https://gitlab.freedesktop.org/lignah/libfprint/-/blob/feature/samsung730b/libfprint/drivers/samsung730b.c?ref_type=heads)

### 6.1. libfprint (s730b_detect_finger)

libfprint driver function:

```c
static guint8 *
s730b_detect_finger (GUsbDevice *usb_dev, gsize *out_len, GError **error)
{
  ...
  /* packet 0: control 0xCA + start cmd, short status IN */
  ...

  /* packets 1..N: small number of data/ACK loops */
  for (i = 1; i < DETECT_PACKETS; i++) {
      guint16 widx = CAPTURE_START_IDX + i * BULK_PACKET_SIZE;
      usb_ctrl_out (..., widx, ...);
      usb_bulk_in (...);
      usb_bulk_out (... zeros ACK ...);
  }
  ...
}
```

Key points:

    Same control/bulk pattern as in full capture.
    Only a handful of packets are read (e.g. DETECT_PACKETS = 6).
    Data is returned in a temporary buffer for analysis.

### 6.2. Heuristic in s730b_has_fingerprint


```c
static gboolean
s730b_has_fingerprint (const guint8 *data, gsize len)
{
  if (!data || len < 512) return FALSE;
  // ...

  double ff_ratio = (double) ff / (double) total;

  if (ff_ratio > 0.30 && zeros < total * 0.95)
    return TRUE;

  return FALSE;
}
```

This is intentionally simple:

    If the frame is mostly zeros or has very few 0xFF values, assume “no finger”.
    If more than ~30% of the bytes are 0xFF and the frame is not completely zero‑filled, assume “finger present”.

These thresholds were chosen empirically by inspecting multiple captures with and without a finger.

### 6.3. libfprint wait (s730b_wait_finger and s730b_wait_finger_lost)

    - s730b_wait_finger: "Init → Detect Finger → Heuristic"
    - Output log on success and reset the sensor again before Full Capture
    - s730b_wait_finger_lost: Wait until Heuristics reports "Fingerless" (for processing between Enroll/Verify)

## 7. libfprint driver's capture

Same C/libusb driver's logic

Notes:

    capture_indices[] is a copy of the sequence observed from Windows / Python logs:

    { 0x032a, 0x042a, 0x052a, ..., 0x572a }

    The first index 0x032a corresponds to chunk 0.

    Subsequent indices are spaced by 0x0100, which matches the behavior in the original USBPcap capture.

    Typical total length is again ~21.5 KB, matching the Python implementation.

## 8. Image hand‑off to libfprint

Image submission function:

```c
static void
s730b_submit_image (FpImageDevice *dev, guint8 *raw, gsize raw_len)
{
  ...
  FpImage *img = fp_image_new (IMG_W, IMG_H);
  gsize img_size = 0;
  guchar *dst = (guchar *) fp_image_get_data (img, &img_size);

  memcpy (dst, raw + IMG_OFFSET, IMG_W * IMG_H);
  g_free (raw);

  fpi_image_device_report_finger_status (dev, TRUE);
  fpi_image_device_image_captured (dev, img);
}
```

Current state :

    The driver forwards the unrotated 112×96 buffer starting at offset 180 as an FpImage.(No rotation)

In field tests :

    Enrollment via fprintd-enroll succeeds (5/5 stages).
    Verification via fprintd-verify currently often yields scores of 0/20 (no match), despite visually reasonable images.

## 9. Summary and future plans

confirmed contents :

    Device: Samsung 730B (VID 0x04E8, PID 0x730B)
    Protocols: Control (0xC3/0xCA) and Bulk (0xA9/0xA8 Init, data chunk loop)
    Capture: Chunk 0 Special Processing + 84 Data Chunk
    Image: Offset 180, 112x96, 8bpp Grayscale (Requires 90 degree rotation for viewing?)
    Detection: Short capture (6 chunks) + 0xFF/0x00 ratio Heuristic
    - Complete creation of libfprint driver (samsung730b)
    - Check Enroll Behavior

Next thing to do:

    [ ] fprintd-verify score 0/20 Cause Analysis/Resolution