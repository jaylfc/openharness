// The LAN transport: the same cable frames as USB, over one TCP connection, for the computer the cable
// bound. Optional and self-contained — nothing here runs until wifi_sta.c has an IP address.
//
// What it does: advertises `_harness-dial._tcp` on mDNS, listens on WIFI_CABLE_PORT, accepts ONE client
// at a time, and feeds what arrives through its own frame decoder (a stray probe on the LAN must never
// shred the USB decoder) into the same callback the USB reader uses. What it does not do: decide who is
// allowed. A connection is only a connection until the message layer (cable_client.c) accepts a
// `welcome` carrying the stored bind token and calls cable_link_set_active(CABLE_XPORT_TCP); a client
// that has not been accepted within WIFI_CABLE_AUTH_MS is closed, and while a USB session is up every
// client is closed.
//
// See WIFI_PROTOCOL.md (waveshare-175c worktree) for the contract with the daemon.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cable_frame.h"

#define WIFI_CABLE_PORT 17420
#define WIFI_CABLE_AUTH_MS 10000   // how long an unaccepted connection may sit before it is closed

// `on_frame` is the callback the frames go to (ctx = the TCP transport marker); `on_closed` runs on the
// LAN task whenever a client that was connected is gone, however it went, so the session layer can end
// a session that rode on it. Call once, before wifi_sta_init().
typedef void (*wifi_cable_closed_cb)(void);
void wifi_cable_init(cable_frame_cb on_frame, wifi_cable_closed_cb on_closed);

// The station got / lost its address. Up starts mDNS and the listener (creating the task the first
// time); down closes everything.
void wifi_cable_on_sta_up(void);
void wifi_cable_on_sta_down(void);

// Close the current client, if any. Safe from any task.
void wifi_cable_drop(void);
bool wifi_cable_client(void);

// Write bytes to the client, all of them or fail. Any failure closes the client. Called by
// cable_link.c with its transmit lock held; takes the socket lock inside it, never the other way round.
bool wifi_cable_write(const uint8_t *data, size_t n);
