#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""
Standalone (non-browser) test client for heartkit-vitals-demo's TileIO USB
vendor stream. Runs anywhere pyusb + libusb are available -- useful for
verifying the device enumerates and streams correctly without a browser/
WebUSB-capable host.

Protocol: modules/nsx-tileio/modules/nsx-tileio-usb/src/tio_usb.c
  - Vendor interface (class 0xFF), one bulk OUT + one bulk IN endpoint.
  - Fixed 256-byte packets:
      [0]     start   = 0x55
      [1]     slot     (0=ECG, 1=PPG, 2=CPU)
      [2]     slot_type (0=signal, 1=metrics, 2=UIO state)
      [3:5]   data_len, little-endian uint16
      [5:253] data (up to 248 bytes, zero-padded)
      [253:255] crc16 (over bytes [3:5+data_len], little-endian)
      [255]   stop    = 0xAA
  - CRC16: init=0xEF4A, poly=0x1021 (CCITT-style, MSB-first), see
    tio_usb_compute_crc16() in tio_usb.c.
  - Host->device framing: every client (the production TileIO web
    dashboard, api/usb.ts's setUioState()) sends host->device writes as a
    single raw WebUSB transferOut() of the full packed 256-byte packet --
    no application-level chunking or per-transfer header. WebUSB
    automatically splits this into as many wMaxPacketSize (64-byte) USB
    transactions as needed, mirroring how the device->host read direction
    already works. (An earlier web app version instead split writes into
    62-byte payloads with a 2-byte "NS frame header" per 64-byte transfer,
    a legacy convention this firmware never actually implemented -- fixed
    by dropping that framing from both the web app and firmware together
    rather than teaching the firmware to parse it.)

IMPORTANT: the device only starts streaming (tio_usb_tx_available()) once it
has received at least one vendor OUT write from the host -- this tool writes a
zero-length UIO state-request packet before reading, exactly like a real
dashboard connecting. An eight-byte all-zero UIO packet is a state update, not
a request.

Requires: pip install pyusb, and a libusb backend (libusb-1.0) installed.
On macOS: brew install libusb
On Linux you may need a udev rule granting your user access to the device:
    SUBSYSTEM=="usb", ATTR{idVendor}=="cafe", ATTR{idProduct}=="0001", \\
        MODE="0660", GROUP="plugdev"
"""
import argparse
import struct
import sys
import time

import usb.core
import usb.util

VENDOR_ID = 0xCAFE
DEFAULT_PRODUCT_ID = 0x0001
VENDOR_INTERFACE_CLASS = 0xFF

PACKET_LEN = 256
START_VAL = 0x55
STOP_VAL = 0xAA
DLEN_IDX = 3
DATA_IDX = 5
DATA_MAX_LEN = 248
CRC_IDX = 253
CRC_INIT = 0xEF4A
CRC_POLY = 0x1021

SLOT_NAMES = {0: "ECG", 1: "PPG", 2: "CPU"}
TYPE_NAMES = {0: "signal", 1: "metrics", 2: "uio"}

ECG_METRICS_FMT = "<11f"  # hr, hrv, denoiseCossim, arrLabel, denoiseIps, segmentIps,
                          # arrhythmiaIps, qos, denoiseuIpspw, segmentuIpspw, arrhythmiaIpspw
PPG_METRICS_FMT = "<3f"   # pr, spo2, qos
CPU_METRICS_FMT = "<3f"   # cpuPercUtil, batteryDays, avgAiIps


def crc16(data: bytes) -> int:
    """Matches tio_usb_compute_crc16() in tio_usb.c bit-for-bit."""
    crc = CRC_INIT
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            temp = (crc << 1) & 0xFFFF
            if crc & 0x8000:
                temp ^= CRC_POLY
            crc = temp & 0xFFFF
    return crc


def pack_packet(slot: int, slot_type: int, data: bytes) -> bytes:
    """Mirrors tio_usb_pack_slot_data() in tio_usb.c."""
    if len(data) > DATA_MAX_LEN:
        raise ValueError(f"data too long: {len(data)} > {DATA_MAX_LEN}")
    packet = bytearray(PACKET_LEN)
    packet[0] = START_VAL
    packet[1] = slot
    packet[2] = slot_type
    struct.pack_into("<H", packet, DLEN_IDX, len(data))
    packet[DATA_IDX:DATA_IDX + len(data)] = data
    crc = crc16(bytes(packet[DLEN_IDX:DATA_IDX + len(data)]))
    struct.pack_into("<H", packet, CRC_IDX, crc)
    packet[255] = STOP_VAL
    return bytes(packet)


def unpack_packet(packet: bytes):
    """Returns (slot, slot_type, data) or None if invalid (bad start/stop/crc)."""
    if len(packet) != PACKET_LEN:
        return None
    if packet[0] != START_VAL or packet[255] != STOP_VAL:
        return None
    slot = packet[1]
    slot_type = packet[2]
    data_len = struct.unpack_from("<H", packet, DLEN_IDX)[0]
    if data_len > DATA_MAX_LEN:
        return None
    packet_crc = struct.unpack_from("<H", packet, CRC_IDX)[0]
    computed_crc = crc16(packet[DLEN_IDX:DATA_IDX + data_len])
    if packet_crc != computed_crc:
        return None
    data = packet[DATA_IDX:DATA_IDX + data_len]
    return slot, slot_type, data


def find_vendor_endpoints(dev):
    """Return (interface_number, ep_out, ep_in) for the vendor class interface."""
    for cfg in dev:
        for intf in cfg:
            if intf.bInterfaceClass != VENDOR_INTERFACE_CLASS:
                continue
            ep_out = ep_in = None
            for ep in intf:
                direction = usb.util.endpoint_direction(ep.bEndpointAddress)
                if direction == usb.util.ENDPOINT_OUT:
                    ep_out = ep
                elif direction == usb.util.ENDPOINT_IN:
                    ep_in = ep
            if ep_out is not None and ep_in is not None:
                return intf.bInterfaceNumber, ep_out, ep_in
    return None, None, None


def describe_metrics(slot: int, data: bytes) -> str:
    try:
        if slot == 0 and len(data) >= struct.calcsize(ECG_METRICS_FMT):
            hr, hrv, cossim, arr, dips, sips, aips, qos, dipw, sipw, aipw = struct.unpack(
                ECG_METRICS_FMT, data[:struct.calcsize(ECG_METRICS_FMT)])
            return (f"hr={hr:.1f} hrv={hrv:.1f} cossim={cossim:.1f} arr={int(arr)} qos={qos:.1f} "
                    f"ips(den/seg/arr)={dips:.1f}/{sips:.1f}/{aips:.1f}")
        if slot == 1 and len(data) >= struct.calcsize(PPG_METRICS_FMT):
            pr, spo2, qos = struct.unpack(PPG_METRICS_FMT, data[:struct.calcsize(PPG_METRICS_FMT)])
            return f"pr={pr:.1f} spo2={'n/a' if spo2 == 0 else f'{spo2:.1f}'} qos={qos:.1f}"
        if slot == 2 and len(data) >= struct.calcsize(CPU_METRICS_FMT):
            cpu, batt, ips = struct.unpack(CPU_METRICS_FMT, data[:struct.calcsize(CPU_METRICS_FMT)])
            return f"cpu={cpu:.1f}% battery={batt:.1f}d avg_ai_ips={ips:.1f}"
    except struct.error:
        pass
    return f"{len(data)} raw bytes: {data[:16].hex()}..."


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--vid", type=lambda x: int(x, 0), default=VENDOR_ID, help="USB vendor ID (default 0xCAFE)")
    parser.add_argument("--pid", type=lambda x: int(x, 0), default=DEFAULT_PRODUCT_ID,
                         help="USB product ID (default 0x0001)")
    parser.add_argument("--list-devices", action="store_true", help="list all USB devices and exit")
    parser.add_argument("--duration", type=float, default=10.0, help="seconds to read (0 = run forever)")
    parser.add_argument("--no-kick", action="store_true",
                         help="don't send the wake-up packet first (device will likely stay silent)")
    parser.add_argument("--uio-state", metavar="HEX", default=None,
                        help="send an eight-byte UIO state after wake-up, for example 0600000000020202 for live input")
    parser.add_argument("--timeout-ms", type=int, default=2000, help="bulk read timeout in ms")
    parser.add_argument("--raw", action="store_true", help="print every packet's raw slot/type/data")
    parser.add_argument("--ppg-stats", action="store_true",
                        help="summarize PPG signal range, clip-rail hits, and sample-to-sample steps")
    args = parser.parse_args()

    if args.list_devices:
        for dev in usb.core.find(find_all=True):
            try:
                print(f"  VID=0x{dev.idVendor:04X} PID=0x{dev.idProduct:04X} "
                      f"bus={dev.bus} addr={dev.address} product={usb.util.get_string(dev, dev.iProduct) if dev.iProduct else '?'}")
            except (usb.core.USBError, ValueError):
                print(f"  VID=0x{dev.idVendor:04X} PID=0x{dev.idProduct:04X} bus={dev.bus} addr={dev.address}")
        return 0

    dev = usb.core.find(idVendor=args.vid, idProduct=args.pid)
    if dev is None:
        print(f"error: no device found with VID=0x{args.vid:04X} PID=0x{args.pid:04X}", file=sys.stderr)
        print("Is it plugged into THIS host's USB (not just the J-Link debug USB)? "
              "Some EVBs need a second cable for the target's own USB peripheral.", file=sys.stderr)
        print("Run with --list-devices to see everything currently enumerated.", file=sys.stderr)
        return 1

    print(f"found device: VID=0x{dev.idVendor:04X} PID=0x{dev.idProduct:04X} "
          f"manufacturer={dev.manufacturer!r} product={dev.product!r} serial={dev.serial_number!r}")

    intf_num, ep_out, ep_in = find_vendor_endpoints(dev)
    if ep_in is None or ep_out is None:
        print("error: no vendor-class (0xFF) interface with both IN+OUT bulk endpoints found", file=sys.stderr)
        return 1

    print(f"vendor interface: {intf_num}, OUT endpoint: 0x{ep_out.bEndpointAddress:02X}, "
          f"IN endpoint: 0x{ep_in.bEndpointAddress:02X}, max packet size: {ep_in.wMaxPacketSize}")

    if dev.is_kernel_driver_active(intf_num):
        try:
            dev.detach_kernel_driver(intf_num)
        except usb.core.USBError:
            # macOS's libusb backend doesn't support detach_kernel_driver
            # (there's no vendor-class kernel driver to detach from in the
            # first place there); harmless to skip and proceed straight to
            # claim_interface().
            pass

    usb.util.claim_interface(dev, intf_num)
    try:
        if not args.no_kick:
            # Wake up TileIO TX and request its current state. A zero-length
            # UIO frame is a request; an eight-byte all-zero frame would
            # overwrite the device state.
            kick = pack_packet(0, 2, bytes())
            ep_out.write(kick, timeout=args.timeout_ms)
            print("sent UIO state request")
        if args.uio_state is not None:
            try:
                state = bytes.fromhex(args.uio_state)
            except ValueError as exc:
                parser.error(f"invalid --uio-state hex: {exc}")
            if len(state) != 8:
                parser.error("--uio-state must encode exactly eight bytes")
            ep_out.write(pack_packet(0, 2, state), timeout=args.timeout_ms)
            print(f"sent UIO state update: {state.hex()}")

        rx_buf = bytearray()
        packet_count = 0
        bad_count = 0
        slot_counts = {}
        ppg_samples = [[], []]
        start = time.monotonic()

        print("reading... (Ctrl-C to stop)")
        while args.duration <= 0 or (time.monotonic() - start) < args.duration:
            try:
                chunk = dev.read(ep_in.bEndpointAddress, PACKET_LEN * 4, timeout=args.timeout_ms)
                rx_buf.extend(chunk)
            except usb.core.USBTimeoutError:
                print(f"  (no data for {args.timeout_ms} ms)")
                continue
            except usb.core.USBError as exc:
                print(f"USB read error: {exc}", file=sys.stderr)
                break

            # Resync on the start byte, then parse complete fixed-length packets.
            while len(rx_buf) >= PACKET_LEN:
                if rx_buf[0] != START_VAL:
                    del rx_buf[0]
                    continue
                packet = bytes(rx_buf[:PACKET_LEN])
                parsed = unpack_packet(packet)
                if parsed is None:
                    bad_count += 1
                    del rx_buf[0]
                    continue
                del rx_buf[:PACKET_LEN]

                slot, slot_type, data = parsed
                packet_count += 1
                slot_counts[(slot, slot_type)] = slot_counts.get((slot, slot_type), 0) + 1

                slot_name = SLOT_NAMES.get(slot, f"slot{slot}")
                type_name = TYPE_NAMES.get(slot_type, f"type{slot_type}")
                if args.ppg_stats and slot == 1 and slot_type == 0:
                    for offset in range(0, len(data) - 5, 6):
                        _, red, ir = struct.unpack_from("<Hhh", data, offset)
                        ppg_samples[0].append(red)
                        ppg_samples[1].append(ir)
                if args.raw:
                    print(f"#{packet_count} {slot_name}/{type_name} len={len(data)} data={data.hex()}")
                elif slot_type == 1:  # metrics
                    print(f"#{packet_count} {slot_name} metrics: {describe_metrics(slot, data)}")
                elif slot_type == 2:  # uio echo
                    print(f"#{packet_count} uio echo: {data.hex()}")
                # Signal (type 0) frames arrive at high rate -- summarized in the footer only.

        print(f"\ndone: packets={packet_count} bad={bad_count}")
        for (slot, slot_type), count in sorted(slot_counts.items()):
            print(f"  {SLOT_NAMES.get(slot, slot)}/{TYPE_NAMES.get(slot_type, slot_type)}: {count}")
        if args.ppg_stats:
            for name, samples in zip(("red", "ir"), ppg_samples):
                if not samples:
                    print(f"  PPG {name}: no samples")
                    continue
                steps = [abs(b - a) for a, b in zip(samples, samples[1:])]
                rail_hits = sum(sample in (-13750, 13750) for sample in samples)
                max_step = max(steps, default=0)
                mean_step = sum(steps) / len(steps) if steps else 0.0
                print(f"  PPG {name}: n={len(samples)} range=[{min(samples)}, {max(samples)}] "
                      f"rail_hits={rail_hits} ({100.0 * rail_hits / len(samples):.1f}%) "
                      f"mean_step={mean_step:.1f} max_step={max_step}")
    finally:
        usb.util.release_interface(dev, intf_num)
        usb.util.dispose_resources(dev)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
