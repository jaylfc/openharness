// The USB transport under cable_frame: bytes in from the native USB port, frames out to it.
//
// This is the device's connection to the outside world: the native USB port, and — only for a computer
// that has already been plugged in — an optional second transport over the LAN (wifi_cable.c).
//
// USB is the authorization. Plugging the cable in is what binds a computer to this dial (the daemon
// sends a `bind` token in `welcome`, see cable_client.c), and the LAN transport only ever carries the
// same frames for a computer that presents it. The WiFi side is opt-in and self-contained: without a
// saved network it is never started and costs nothing. Everything the dial says goes through the
// functions below; a frame goes out on the active transport (the greeting on both).
//
// USB WINS. While a USB session is up the LAN client is dropped and new LAN connections are closed. The
// LAN carries frames and nothing else: the console and the LOG frames stay on the cable, so a serial
// monitor and the daemon's log file work exactly as before.
//
// ── ONE PORT, AND WHAT THAT COSTS ───────────────────────────────────────────────────────────────────
// Measured on the board on 2026-08-24: it enumerates as exactly one device —
//
//     303a:1001   "USB JTAG/serial debug unit", Espressif   ← the SoC's own USB peripheral
//
// and nothing else. There is no second UART bridge to move the console to, so the console and this
// protocol share a wire. That is why cable_frame carries a LOG type: while a session is up, every
// ESP_LOG line goes out as a CABLE_TYPE_LOG frame and the daemon files it, so the wire carries frames
// and not a mixture of frames and text.
//
// What CANNOT be framed is the residue, and it is by design: the ROM, the second-stage bootloader and
// the panic handler all write to this port directly, before or instead of anything this file controls.
// The far end's decoder is built to walk through it — that is what the magic scan and the CRC are for.
//
// ── WHEN NOBODY IS LISTENING, THE CONSOLE IS A CONSOLE ──────────────────────────────────────────────
// Log framing is installed when a session starts and removed when it ends (cable_link_set_log_framing).
// Unplugged, or plugged into a machine with no daemon running, the port carries ordinary console text
// and `idf.py monitor` behaves exactly as it always has. Framing logs unconditionally would cost the
// only debugging instrument this board has, in exactly the situation where it is needed most.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cable_frame.h"

// Install the USB-Serial-JTAG driver and start the reader task.
//
// `cb` is invoked once per decoded frame, ON THE READER TASK, with the payload pointing into the
// decoder's own buffer — it is only valid for the duration of the call. Anything that must outlive it
// gets copied by the callback. Anything that touches LVGL takes display_lock() first.
//
// The callback's `ctx` argument is NOT the `ctx` passed here: it names the transport the frame arrived
// on (cable_xport_of). The LAN transport calls the same callback from its own task, so the callback
// must be safe to enter from two tasks. `ctx` here goes to `tick` only.
//
// Returns false if the driver would not install, which leaves the dial running with no link rather than
// failing to boot: a device that shows "Not connected" is diagnosable from across the room, and a device
// stuck in a boot loop is not.
// `tick` also runs on the reader, after each read/feed (including idle reads).
// Keep connection expiry here so it cannot race a welcome/frame callback on
// another core. It must be bounded and must not retain decoder payloads.
typedef void (*cable_tick_cb)(void *ctx);

// Which wire a frame came from or goes to.
typedef enum {
    CABLE_XPORT_NONE = 0,
    CABLE_XPORT_USB,
    CABLE_XPORT_TCP,
} cable_xport_t;

// The transport a frame callback's `ctx` names.
static inline void *cable_xport_ctx(cable_xport_t x) { return (void *)(uintptr_t)x; }
static inline cable_xport_t cable_xport_of(void *ctx) { return (cable_xport_t)(uintptr_t)ctx; }

bool cable_link_start(cable_frame_cb cb, cable_tick_cb tick, void *ctx);

// Frame `payload` and write it to the port. Returns true when the whole frame went out.
//
// A false return means the host is not draining the port (an unopened or unplugged CDC endpoint fills
// the FIFO and the write times out). That is a normal state for this device, not an error condition —
// the dial sits unplugged or in front of a machine with no daemon for most of its life. Callers should
// treat it as "not connected", never retry in a tight loop.
//
// The frame goes to the ACTIVE transport: the LAN client once a LAN session has been accepted (see
// cable_link_set_active), the USB port otherwise. Use cable_link_send_to for the few messages that name
// a wire.
//
// If a write is cut short mid-frame the peer sees a truncated frame, which its decoder resyncs past on
// the next magic — that recovery is exactly what the CRC-plus-magic-scan exists for, so a bad moment
// costs one message rather than the link.
bool cable_link_send(uint8_t type, const uint8_t *payload, size_t payload_len);

// Send to one named transport regardless of which is active: the greeting goes to both wires, and a
// reply goes back on the wire its request came in on. Returns false when that transport has no peer.
bool cable_link_send_to(cable_xport_t x, uint8_t type, const uint8_t *payload, size_t payload_len);

// Set by the session layer when a session is accepted (USB or TCP) and cleared when it ends. NONE
// behaves like USB for sending. The LAN task reads it to know that USB has won and the client must go.
void cable_link_set_active(cable_xport_t x);
cable_xport_t cable_link_active(void);

// Route ESP_LOG through the link as CABLE_TYPE_LOG frames (true), or back to the plain console (false).
//
// Called by the session layer, not by this one: whether a peer is listening is a message-layer fact
// (a `welcome` arrived), and this layer cannot see it. usb_serial_jtag_is_connected() answers a
// different question — see cable_link_host_present().
void cable_link_set_log_framing(bool on);

// Whether a USB HOST is currently driving this port.
//
// NOT the same question as "is the daemon talking to me", and the difference is the whole reason this is
// documented rather than inlined at the call site. The driver answers from USB frame activity, so it is
// true whenever the cable is in a running computer — including one with no daemon, which is where this
// device spends much of its life.
//
// It is therefore useful for exactly one thing: noticing that the cable left, or that the machine went
// to sleep. The layer above uses it as the end of a session. A session that ends because the DAEMON quit
// with the cable still in is invisible here — see the hello cadence in cable_client.c for what stands in.
bool cable_link_host_present(void);

// Framing health. The RATE is the diagnosis, not the totals: a handful of discarded bytes right after
// boot is the bootloader's parting words and is expected, while a steady trickle during a session means
// the two sides disagree about the format or the cable is bad. Without a count those look identical.
void cable_link_counters(uint32_t *corrupt_frames, uint32_t *discarded_bytes);
// Logs skipped because the transport was busy or stalled; diagnostic only.
uint32_t cable_link_dropped_logs(void);

// Drop any half-received frame. For the layer above to call when it decides a session has ended and a
// new one begun — leftover bytes belong to the old one, and carrying them across puts a stale half-frame
// in front of the first real frame of the new session.
//
// Call it only from the reader's frame or tick callback. The decoder has one
// reader and no lock; another task must not reset it during a feed. The reader
// also expires a partial frame after a 15-second gap in received bytes.
void cable_link_reset_decoder(void);
