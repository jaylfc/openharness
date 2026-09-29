// Finding dials on the network through macOS's own resolver, by running `dns-sd`.
//
// macOS lets its resolver (mDNSResponder) talk on the local network but restricts other processes:
// which ones may send and receive LAN multicast is decided by the app that started them (the Local Network
// privacy setting). A daemon the Harness app started can browse with its own multicast socket; the same
// daemon started from a terminal, by an installer or by launchd hears nothing from any other device on
// the network, and reports nothing wrong. `dns-sd` is Apple's own client for the resolver, so it works
// whichever way the daemon was started, and the `.local` name it returns resolves through the same
// resolver when the connection is made.
import { spawn as nodeSpawn } from 'node:child_process'
import type { BrowseOptions, DialAddress, DialBrowser } from './tcpLink.js'

const SERVICE = '_harness-dial._tcp'
const BROWSE_RESTART_MS = 5_000
const RESOLVE_TIMEOUT_MS = 5_000

interface ChildLike {
  stdout: { on(event: 'data', fn: (chunk: Buffer | string) => void): unknown } | null
  on(event: 'exit' | 'error', fn: (arg?: unknown) => void): unknown
  kill(): unknown
}
export type SpawnLike = (command: string, args: string[]) => ChildLike

const defaultSpawn: SpawnLike = (command, args) => nodeSpawn(command, args, { stdio: ['ignore', 'pipe', 'ignore'] }) as unknown as ChildLike

/** Lines from a stream that delivers arbitrary chunks. */
function lineReader(onLine: (line: string) => void): (chunk: Buffer | string) => void {
  let rest = ''
  return chunk => {
    rest += chunk.toString()
    let i: number
    while ((i = rest.indexOf('\n')) >= 0) {
      onLine(rest.slice(0, i).replace(/\r$/, ''))
      rest = rest.slice(i + 1)
    }
  }
}

const tagOf = (host: string): string => /harness-([0-9a-f]{4})/i.exec(host)?.[1]?.toUpperCase() ?? ''
const tagOfMac = (mac: string): string => mac.replace(/[^0-9A-F]/gi, '').slice(-4).toUpperCase()

interface Found { host: string; port: number; mac: string; tag: string; at: number }

/**
 * Browse for dials with `dns-sd -B`, and resolve each instance with `dns-sd -L` for its host, port and
 * TXT `mac`. Throws nothing: if `dns-sd` cannot run, `onError` is told and the browser finds nothing.
 */
export function browseDialsDnsSd(onError: (why: string) => void, options: BrowseOptions & { spawn?: SpawnLike } = {}): DialBrowser {
  const run = options.spawn ?? defaultSpawn
  const refreshMs = options.refreshMs ?? 15_000
  const found = new Map<string, Found>()            // by instance name
  const resolving = new Map<string, ChildLike>()
  const timers = new Set<ReturnType<typeof setTimeout>>()
  let browse: ChildLike | undefined
  let stopped = false
  let failed = false

  const later = (fn: () => void, ms: number) => {
    const t = setTimeout(() => { timers.delete(t); if (!stopped) fn() }, ms)
    t.unref?.()
    timers.add(t)
  }

  const resolve = (instance: string) => {
    if (resolving.has(instance) || stopped) return
    let child: ChildLike
    try { child = run('dns-sd', ['-L', instance, SERVICE, 'local.']) } catch (error) { onError(String(error)); return }
    resolving.set(instance, child)
    let host = '', port = 0, mac = ''
    let done = false
    const finish = () => {
      if (done) return
      done = true
      resolving.delete(instance)
      try { child.kill() } catch { /* already gone */ }
      if (!host || !port || stopped) return
      const before = found.get(instance)
      const entry: Found = { host, port, mac, tag: tagOf(host) || (mac ? tagOfMac(mac) : ''), at: Date.now() }
      found.set(instance, entry)
      if (!before || before.host !== host || before.mac !== mac) options.log?.(`cable: a dial is on the network: ${mac || instance} at ${host}:${port}`)
    }
    child.stdout?.on('data', lineReader(line => {
      // "OpenHarness dial._harness-dial._tcp.local. can be reached at harness-4F98.local.:17420 (interface 16)"
      const reach = /can be reached at (\S+?)\.?:(\d+)\b/.exec(line)
      if (reach) { host = reach[1]; port = Number(reach[2]) }
      // The TXT record follows on its own line: " mac=7C:0C:5F:42:4F:98 ..."
      const txt = /(?:^|\s)mac=([0-9A-Fa-f:]{11,17})/.exec(line)
      if (txt) { mac = txt[1].toUpperCase(); finish() }
    }))
    child.on('exit', () => finish())
    child.on('error', () => finish())
    later(finish, RESOLVE_TIMEOUT_MS)   // a dial with no TXT is still worth its name and address
  }

  const startBrowse = () => {
    if (stopped) return
    let child: ChildLike
    try { child = run('dns-sd', ['-B', SERVICE, 'local.']) } catch (error) {
      if (!failed) onError(`dns-sd could not start: ${String(error)}`)
      failed = true
      later(startBrowse, BROWSE_RESTART_MS * 6)
      return
    }
    browse = child
    child.stdout?.on('data', lineReader(line => {
      // "17:50:01.456  Add        2  16 local.               _harness-dial._tcp.  OpenHarness dial"
      const m = /^\S+\s+(Add|Rmv)\s+\d+\s+\d+\s+\S+\s+\S+\s+(.+?)\s*$/.exec(line)
      if (!m) return
      const [, kind, instance] = m
      if (kind === 'Add') resolve(instance)
      else if (found.delete(instance)) options.log?.(`cable: a dial left the network: ${instance}`)
    }))
    const restart = () => { if (browse === child) { browse = undefined; later(startBrowse, BROWSE_RESTART_MS) } }
    child.on('exit', restart)
    child.on('error', () => { if (!failed) onError('dns-sd failed'); failed = true; restart() })
  }
  startBrowse()

  // The resolver's answers are re-read on a slow beat, so an address or a MAC that changed is noticed and a
  // dial that dropped off without a goodbye (power lost) is forgotten.
  const beat = setInterval(() => {
    if (stopped) return
    for (const [instance, entry] of found) {
      if (Date.now() - entry.at > refreshMs * 2.5) { found.delete(instance); options.log?.(`cable: a dial left the network: ${entry.mac || instance}`); continue }
      resolve(instance)
    }
  }, refreshMs)
  beat.unref?.()

  return {
    find: mac => {
      const wanted = mac.trim().toUpperCase()
      let hit: Found | undefined
      for (const entry of found.values()) {
        if (entry.mac === wanted || (!entry.mac && entry.tag && entry.tag === tagOfMac(wanted))) { hit = entry; break }
      }
      return hit ? ({ host: hit.host, port: hit.port } satisfies DialAddress) : undefined
    },
    stop: () => {
      stopped = true
      clearInterval(beat)
      for (const t of timers) clearTimeout(t)
      timers.clear()
      for (const child of resolving.values()) { try { child.kill() } catch { /* already gone */ } }
      resolving.clear()
      try { browse?.kill() } catch { /* already gone */ }
      browse = undefined
      found.clear()
    },
  }
}
