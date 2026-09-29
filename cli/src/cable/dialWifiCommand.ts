// `harness dial wifi` — give a dial a WiFi network, over the cable.
//
// The password is asked for on a hidden prompt (a TTY) or read from HARNESS_WIFI_PSK (no TTY; empty means
// an open network). It is never a command-line argument, so it is not in `ps`, shell history or a log; it
// goes to the running daemon in the body of a loopback request, and the daemon sends it to the dial over USB.

const USAGE = `Usage:
  harness dial wifi <ssid> [--device <serial>]   set the network (asks for the password; or HARNESS_WIFI_PSK when there is no terminal)
  harness dial wifi --forget [--device <serial>] erase the saved network
  harness dial wifi --status                     show the WiFi of every dial
The dial must be plugged in over USB to change its WiFi.`

export interface DialWifiDeps {
  /** A call to the running daemon's localhost API. Throws when the daemon is not running. */
  call: (method: 'GET' | 'POST', path: string, body?: unknown) => Promise<{ status: number; json: Record<string, unknown> }>
  /** Hidden-input prompt. Only used when `isTTY`. */
  promptPassword: (prompt: string) => Promise<string>
  isTTY: boolean
  env: Record<string, string | undefined>
  output: (line: string) => void
  error: (line: string) => void
  sleep?: (ms: number) => Promise<void>
  /** How long to watch for the dial to join. */
  waitMs?: number
}

interface WifiRow { id?: string; mac?: string; attached?: boolean; transport?: string; wifi?: { state?: string; ssid?: string; rssi?: number; ip?: string; reason?: string } }

function describe(row: WifiRow): string {
  const who = row.id ?? row.mac ?? 'dial'
  const w = row.wifi
  const link = row.attached ? (row.transport === 'tcp' ? 'over WiFi' : 'over USB') : 'not connected'
  if (!w) return `${who}  (${link})  wifi: not reported`
  const bits = [w.state ?? '?', w.ssid, w.ip, typeof w.rssi === 'number' ? `${w.rssi} dBm` : undefined, w.reason].filter(Boolean)
  return `${who}  (${link})  wifi: ${bits.join('  ')}`
}

export async function dialWifiCommand(argv: string[], deps: DialWifiDeps): Promise<number> {
  const { call, output, error } = deps
  let forget = false
  let status = false
  let device: string | undefined
  const words: string[] = []
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i]
    if (arg === '--forget') forget = true
    else if (arg === '--status') status = true
    else if (arg === '--device') device = argv[++i]
    else if (arg.startsWith('--device=')) device = arg.slice('--device='.length)
    else if (/^--(psk|password|pass|key)(=|$)/.test(arg)) {
      error('\n  ✗ The password is never taken as an argument (it would show in `ps` and shell history).')
      error('    Type it at the prompt, or set HARNESS_WIFI_PSK when there is no terminal.\n')
      return 1
    } else if (arg.startsWith('-')) { error(`Unknown option: ${arg}\n${USAGE}`); return 1 }
    else words.push(arg)
  }
  if (device !== undefined && !device) { error(`--device needs a serial.\n${USAGE}`); return 1 }
  const modes = Number(forget) + Number(status) + Number(words.length > 0)
  if (modes !== 1 || words.length > 1) { error(USAGE); return 1 }

  const unreachable = (): number => {
    error('\n  ✗ The adapter is not running on this computer.')
    error('    Start it first:  harness start\n')
    return 1
  }
  const fail = (json: Record<string, unknown>, status: number): number => {
    error(`\n  ✗ ${typeof json.error === 'string' ? json.error : `Failed (${status}).`}\n`)
    return 1
  }

  if (status) {
    let res
    try { res = await call('GET', '/api/dial/wifi') } catch { return unreachable() }
    if (res.status !== 200) return fail(res.json, res.status)
    const rows = (res.json.devices ?? []) as WifiRow[]
    output(rows.length ? `\n${rows.map(r => `  ${describe(r)}`).join('\n')}\n` : '\n  No dial is known to this computer.\n')
    return 0
  }

  if (forget) {
    let res
    try { res = await call('POST', '/api/dial/wifi', { op: 'forget', ...(device ? { device } : {}) }) } catch { return unreachable() }
    if (res.status !== 200) return fail(res.json, res.status)
    output('\n  ✓ Saved WiFi network erased.\n')
    return 0
  }

  const ssid = words[0]
  let psk: string
  if (deps.isTTY) {
    psk = await deps.promptPassword(`  WiFi password for "${ssid}" (empty for an open network): `)
  } else if (deps.env.HARNESS_WIFI_PSK !== undefined) {
    psk = deps.env.HARNESS_WIFI_PSK
  } else {
    error('\n  ✗ No terminal to ask on. Set HARNESS_WIFI_PSK (empty for an open network) and run again.\n')
    return 1
  }
  let res
  try { res = await call('POST', '/api/dial/wifi', { op: 'set', ssid, psk, ...(device ? { device } : {}) }) } catch { return unreachable() }
  psk = ''
  if (res.status !== 200) return fail(res.json, res.status)
  output(`\n  Sent. Waiting for the dial to join "${ssid}"...`)

  // The answer is the dial's own wifi.status, which reaches the daemon as a status update.
  const sleep = deps.sleep ?? ((ms: number) => new Promise<void>(r => setTimeout(r, ms)))
  const until = Date.now() + (deps.waitMs ?? 20_000)
  const id = typeof res.json.id === 'string' ? res.json.id : device
  await sleep(1000) // let the dial answer for this request, not the last one
  for (;;) {
    let rows: WifiRow[] = []
    try { rows = ((await call('GET', '/api/dial/wifi')).json.devices ?? []) as WifiRow[] } catch { /* keep waiting */ }
    const w = rows.find(r => !id || r.id === id)?.wifi
    if (w?.ssid === ssid && w.state === 'connected') {
      output(`  ✓ Joined "${ssid}"${w.ip ? ` at ${w.ip}` : ''}. Unplug the cable and the dial stays reachable over WiFi.\n`)
      return 0
    }
    if (w?.state === 'failed' && (!w.ssid || w.ssid === ssid)) {
      error(`  ✗ The dial could not join "${ssid}"${w.reason ? ` (${w.reason})` : ''}. Check the name and password.\n`)
      return 1
    }
    if (Date.now() >= until) {
      output('  The dial has the network but has not joined yet. Check again with:  harness dial wifi --status\n')
      return 0
    }
    await sleep(1000)
  }
}
