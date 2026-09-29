// The dial's WiFi state, as it reports it, and what may be asked of it. The password is not here: it
// goes from `harness dial wifi` to the device over USB and is never held in a status.

export type DialWifiState = 'off' | 'connecting' | 'connected' | 'failed'

export interface DialWifi {
  state: DialWifiState
  ssid?: string
  rssi?: number
  ip?: string
  /** A short word on `failed`: `auth`, `no-ap`, `timeout`. */
  reason?: string
}

const STATES = new Set<string>(['off', 'connecting', 'connected', 'failed'])

/** A device's `wifi.status` (or `hello.wifi`) as a status, or undefined when it is not one. */
export function readWifi(value: unknown): DialWifi | undefined {
  if (!value || typeof value !== 'object') return undefined
  const v = value as Record<string, unknown>
  if (typeof v.state !== 'string' || !STATES.has(v.state)) return undefined
  const short = (x: unknown, max: number): string | undefined =>
    typeof x === 'string' && x.length > 0 ? x.slice(0, max) : undefined
  const ssid = short(v.ssid, 64)
  const ip = short(v.ip, 45)
  const reason = short(v.reason, 32)
  return {
    state: v.state as DialWifiState,
    ...(ssid ? { ssid } : {}),
    ...(typeof v.rssi === 'number' && Number.isFinite(v.rssi) ? { rssi: Math.round(v.rssi) } : {}),
    ...(ip ? { ip } : {}),
    ...(reason ? { reason } : {}),
  }
}

/** An error message for credentials the device would refuse, or null when they are fine. */
export function wifiCredentialProblem(ssid: unknown, psk: unknown): string | null {
  if (typeof ssid !== 'string' || typeof psk !== 'string') return 'The network name and password must be text.'
  const ssidBytes = Buffer.byteLength(ssid)
  if (ssidBytes < 1 || ssidBytes > 32) return 'A network name is 1 to 32 bytes.'
  if (Buffer.byteLength(psk) > 63) return 'A WiFi password is at most 63 bytes (leave it empty for an open network).'
  return null
}
