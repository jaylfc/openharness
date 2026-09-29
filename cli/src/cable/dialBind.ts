// USB pairing is what authorizes a dial on WiFi. The token lives next to the rest of this computer's
// daemon state, keyed by the dial's MAC, so a second daemon on the same LAN cannot welcome the device:
// it never saw the USB session that minted the secret. Format: { "tokens": { "AA:BB:...": "<64 hex>" } }.
import { chmodSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { randomBytes } from 'node:crypto'

import { env } from '../config/env.js'

export const DIAL_BIND_FILE = 'dial-bind.json'

let dirOverride: string | undefined

/** Point the token file somewhere else (tests). Undefined restores the daemon's data dir. */
export function setDialBindDirForTest(dir: string | undefined): void {
  dirOverride = dir
}

function dir(): string {
  return dirOverride ?? env.ADAPTER_DATA_DIR
}

const normMac = (mac: string): string => mac.trim().toUpperCase()

function load(): Record<string, string> {
  try {
    const raw = JSON.parse(readFileSync(join(dir(), DIAL_BIND_FILE), 'utf8')) as { tokens?: Record<string, unknown> }
    const out: Record<string, string> = {}
    for (const [mac, token] of Object.entries(raw.tokens && typeof raw.tokens === 'object' ? raw.tokens : {})) {
      if (typeof token === 'string' && /^[0-9a-f]{64}$/.test(token)) out[normMac(mac)] = token
    }
    return out
  } catch {
    return {}
  }
}

function save(tokens: Record<string, string>): void {
  mkdirSync(dir(), { recursive: true })
  const file = join(dir(), DIAL_BIND_FILE)
  const tmp = `${file}.${process.pid}.tmp`
  writeFileSync(tmp, `${JSON.stringify({ tokens }, null, 2)}\n`, { mode: 0o600 })
  renameSync(tmp, file)
  try { chmodSync(file, 0o600) } catch { /* best effort */ }
}

/** Mint (or reuse) the token this computer presents after a USB hello. Empty when there is no MAC. */
export function bindTokenForUsb(mac: string): string {
  const key = normMac(mac)
  if (!key) return ''
  const tokens = load()
  if (tokens[key]) return tokens[key]
  const token = randomBytes(32).toString('hex')
  save({ ...tokens, [key]: token })
  return token
}

/** Token for a TCP hello. Null if this computer has never USB-paired that dial. */
export function bindTokenForTcp(mac: string): string | null {
  const key = normMac(mac)
  return key ? load()[key] ?? null : null
}

/** Every dial this computer has been plugged into, upper-case MAC. */
export function pairedDialMacs(): string[] {
  return Object.keys(load())
}

/** Forget one dial's token: it can no longer be reached over WiFi from here until it is plugged in again. */
export function unbindDial(mac: string): boolean {
  const key = normMac(mac)
  const tokens = load()
  if (!tokens[key]) return false
  delete tokens[key]
  save(tokens)
  return true
}

export function isTcpDialPath(path: string): boolean {
  return path.startsWith('tcp:')
}
