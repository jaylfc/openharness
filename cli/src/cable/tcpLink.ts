// The dial's second transport: the same frames over TCP, found by mDNS `_harness-dial._tcp`.
// Connecting is not authorization. CableSession only welcomes a MAC this computer has USB-paired, and
// nothing here connects to a service whose TXT `mac` is not one it was asked for.
import { isIP, Socket } from 'node:net'
import Bonjour from 'bonjour-service'

export const DIAL_TCP_PORT = 17420
export const DIAL_MDNS_TYPE = 'harness-dial'
const CONNECT_TIMEOUT_MS = 5_000

export interface DialAddress {
  host: string
  port: number
}

/** What the LAN currently says about dials, by upper-case MAC. */
export interface DialBrowser {
  find(mac: string): DialAddress | undefined
  stop(): void
}

interface Advertised {
  name?: string
  host?: string
  port?: number
  addresses?: string[]
  txt?: Record<string, unknown>
}

/** The bit of bonjour-service this uses, so a test can stand in for the network. */
interface BonjourLike {
  find(opts: { type: string; protocol: 'tcp' }): { on(event: 'up' | 'down', fn: (service: Advertised) => void): unknown; stop(): void }
  destroy(): void
}

export interface BrowseOptions {
  /** How often the browse is started afresh. One long browse asks less and less often, and a dial that
   *  missed the first question can go minutes unseen; a fresh short one is what found dials reliably. */
  refreshMs?: number
  /** Said when a dial appears on the network and when it leaves it. */
  log?: (line: string) => void
  /** A stand-in for the network, for tests. */
  bonjour?: (onError: (why: string) => void) => BonjourLike
}

/** The dial's advertised MAC: its TXT `mac`, which may arrive as text or as bytes. */
const macOf = (service: Advertised): string => {
  const raw = service.txt?.mac
  const text = typeof raw === 'string' ? raw : Buffer.isBuffer(raw) ? raw.toString('utf8') : ''
  return text.trim().toUpperCase()
}

/** The last two MAC bytes, which the dial also puts in its name (`harness-4F98`): 4F98. */
const tagOfMac = (mac: string): string => mac.replace(/[^0-9A-F]/gi, '').slice(-4).toUpperCase()
const tagOfName = (service: Advertised): string => {
  const m = /harness-([0-9a-f]{4})(?:\b|\.|$)/i.exec(`${service.host ?? ''} ${service.name ?? ''}`)
  return m ? m[1].toUpperCase() : ''
}

/**
 * A long-lived browse for dials. Started only while some paired dial is out of reach, and stopped
 * as soon as none is, so a computer with no paired dial (or no network) never opens a multicast socket.
 * Throws if the socket cannot be made; the caller treats that as "no network right now".
 *
 * A dial is matched by the MAC in its TXT record, or, when a resolver drops the TXT, by the name it also
 * carries; either way the session refuses a hello from any other MAC, so a wrong match connects to
 * nothing. Its address is the IPv4 from the announcement, else its `.local` name, which the system
 * resolves.
 */
export function browseDials(onError: (why: string) => void = () => {}, options: BrowseOptions = {}): DialBrowser {
  const refreshMs = options.refreshMs ?? 15_000
  type Seen = DialAddress & { at: number; mac: string; tag: string }
  const seen = new Map<string, Seen>()   // by MAC when advertised, else by tag
  let bonjour: BonjourLike | undefined
  let browser: ReturnType<BonjourLike['find']> | undefined
  let stopped = false

  const keyOf = (mac: string, tag: string) => mac || `tag:${tag}`
  const start = () => {
    bonjour = (options.bonjour ?? ((err) => new Bonjour({}, (error: unknown) => err(String(error))) as unknown as BonjourLike))(onError)
    browser = bonjour.find({ type: DIAL_MDNS_TYPE, protocol: 'tcp' })
    browser.on('up', (service: Advertised) => {
      const mac = macOf(service)
      const tag = tagOfName(service) || (mac ? tagOfMac(mac) : '')
      const host = service.addresses?.find(a => isIP(a) === 4) ?? service.addresses?.[0] ?? service.host
      if ((!mac && !tag) || !host) return
      const key = keyOf(mac, tag)
      const before = seen.get(key)
      seen.set(key, { host, port: service.port || DIAL_TCP_PORT, at: Date.now(), mac, tag })
      if (!before || before.host !== host) options.log?.(`cable: a dial is on the network: ${mac || service.name || tag} at ${host}:${service.port || DIAL_TCP_PORT}`)
    })
    browser.on('down', (service: Advertised) => {
      const mac = macOf(service)
      const tag = tagOfName(service) || (mac ? tagOfMac(mac) : '')
      const key = keyOf(mac, tag)
      if (seen.delete(key)) options.log?.(`cable: a dial left the network: ${mac || service.name || tag}`)
    })
  }
  const stopOne = () => {
    try { browser?.stop() } catch { /* already stopped */ }
    try { bonjour?.destroy() } catch { /* already destroyed */ }
    browser = bonjour = undefined
  }

  start()
  const timer = setInterval(() => {
    if (stopped) return
    // Whatever was not heard again across the last two rounds is gone (a dial that lost power says no goodbye).
    for (const [key, entry] of seen) {
      if (Date.now() - entry.at > refreshMs * 2.5) { seen.delete(key); options.log?.(`cable: a dial left the network: ${entry.mac || entry.tag}`) }
    }
    stopOne()
    try { start() } catch (error) { onError(String(error)) }
  }, refreshMs)
  timer.unref?.()

  return {
    find: mac => {
      const wanted = mac.trim().toUpperCase()
      const hit = seen.get(wanted) ?? seen.get(`tag:${tagOfMac(wanted)}`)
      return hit ? { host: hit.host, port: hit.port } : undefined
    },
    stop: () => {
      stopped = true
      clearInterval(timer)
      seen.clear()
      stopOne()
    },
  }
}

/** A `CablePort` over one TCP connection. Same bytes as SerialLink. */
export class TcpLink {
  readonly path: string
  private closed = false
  private closePromise: Promise<void> | null = null
  private tail: Promise<void> = Promise.resolve()

  private constructor(host: string, port: number, private readonly sock: Socket,
                      private readonly onClosed: (why: string) => void) {
    this.path = `tcp:${host}:${port}`
  }

  static open(host: string, port: number, onData: (chunk: Buffer) => void,
              onClosed: (why: string) => void): Promise<TcpLink> {
    return new Promise((resolve, reject) => {
      const sock = new Socket()
      const link = new TcpLink(host, port, sock, onClosed)
      sock.setNoDelay(true)
      sock.setTimeout(CONNECT_TIMEOUT_MS, () => sock.destroy(new Error('connect timed out')))
      sock.once('error', reject)
      sock.connect(port, host, () => {
        sock.removeListener('error', reject)
        sock.setTimeout(0)
        sock.on('data', chunk => { if (!link.closed) onData(Buffer.from(chunk)) })
        sock.on('error', err => { void link.close(err.message) })
        sock.on('close', () => { if (!link.closed) void link.close('end of stream') })
        resolve(link)
      })
    })
  }

  get isOpen(): boolean {
    return !this.closed
  }

  write(bytes: Uint8Array): Promise<void> {
    if (this.closed) return Promise.reject(new Error('port closed'))
    const next = this.tail.then(() => new Promise<void>((resolve, reject) => {
      if (this.closed) { reject(new Error('port closed')); return }
      this.sock.write(Buffer.from(bytes), err => (err ? reject(err) : resolve()))
    }))
    this.tail = next.catch(() => {})
    return next
  }

  close(why = 'closed'): Promise<void> {
    if (this.closePromise) return this.closePromise
    this.closed = true
    this.sock.destroy()
    this.closePromise = Promise.resolve().then(() => this.onClosed(why))
    return this.closePromise
  }
}
